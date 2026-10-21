"""Claude Code session table for the StackChan status robot (pure logic, no I/O).

Maps Claude Code hook events (the JSON a hook receives on stdin), sc-run and the status-line script to the robot
model of REQ-003: up to five sessions in slots 0-4, each with a short project name, a state, an optional activity
(`act`) and context usage (`ctx`), plus one-off robot events.
"""

from __future__ import annotations

import logging
import re
import threading
import time
import unicodedata
from dataclasses import dataclass, field
from datetime import datetime
from datetime import time as dtime
from typing import Any, Callable

from results import CommandDetector, exit_code_from_error, summarize

log = logging.getLogger("sc.sessions")

MAX_SLOTS = 5
NAME_MAX = 12  # Unicode code points
TEXT_MAX = 40  # Unicode code points
EXPIRY_S = 30 * 60.0
ENDED_MEMORY_S = 60.0  # late async hooks of an ended session are ignored for this long
RESTART_GRACE_S = 5.0  # a SessionStart this close to other events of a busy session is out of order
RUN_LINGER_S = 10 * 60.0  # an sc-run session stays this long after the command ended
RUN_MAX_S = 3 * 3600.0  # an sc-run session without ScRunEnd (terminal closed) is dropped after this long
DEFAULT_NAME = "claude"
WEEKDAYS = ("mon", "tue", "wed", "thu", "fri", "sat", "sun")

# idle_prompt is not a call-back: Claude Code sends it about 60 s after every Stop, which would turn each finished
# task into a second "needs you". It only corrects a stale "work"/"wait" (see _on_notification).
NEEDS_TYPES = frozenset({"permission_prompt", "agent_needs_input", "elicitation_dialog", "elicitation_url_dialog"})

# Activity shown by the robot while a session works (protocol v1.1 `act`); Bash is "test" for test/build commands.
TOOL_ACTS = {
    "Edit": "edit", "Write": "edit", "MultiEdit": "edit", "NotebookEdit": "edit",
    "Read": "read", "Grep": "read", "Glob": "read", "LS": "read",
    "WebSearch": "web", "WebFetch": "web",
    "Task": "agents", "Agent": "agents",
}


# --- quiet hours -------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class TimeRange:
    """Daily window [start, end) in local time; wraps over midnight when end < start, empty when equal."""

    start: dtime
    end: dtime

    @classmethod
    def parse(cls, text: str) -> TimeRange:
        match = re.fullmatch(r"\s*(\d{1,2}):(\d{2})\s*-\s*(\d{1,2}):(\d{2})\s*", text)
        if not match:
            raise ValueError(f"bad time range {text!r}, expected HH:MM-HH:MM")
        h1, m1, h2, m2 = (int(part) for part in match.groups())
        return cls(dtime(h1, m1), dtime(h2, m2))

    def contains(self, moment: dtime) -> bool:
        if self.start <= self.end:
            return self.start <= moment < self.end
        return moment >= self.start or moment < self.end


@dataclass(frozen=True)
class QuietHours:
    """No sound during working hours on work days, and never during the night (any day)."""

    work_days: frozenset[int] = frozenset(range(5))  # datetime.weekday(): Monday = 0
    work: TimeRange | None = TimeRange(dtime(8, 0), dtime(18, 0))
    night: TimeRange | None = TimeRange(dtime(23, 0), dtime(8, 0))

    def sound_allowed(self, when: datetime) -> bool:
        moment = when.time()
        if self.night is not None and self.night.contains(moment):
            return False
        if self.work is not None and when.weekday() in self.work_days and self.work.contains(moment):
            return False
        return True


def quiet_hours_from_config(config: dict[str, Any]) -> QuietHours:
    """Build QuietHours from config keys quiet_days (["mon", ...]), quiet_work and quiet_night ("HH:MM-HH:MM",
    null or "" = off); missing keys keep the defaults."""
    default = QuietHours()
    days = default.work_days
    if "quiet_days" in config:
        days = frozenset(_weekday_index(day) for day in config["quiet_days"])
    return QuietHours(
        work_days=days,
        work=_range_option(config, "quiet_work", default.work),
        night=_range_option(config, "quiet_night", default.night),
    )


def _weekday_index(day: Any) -> int:
    key = str(day).strip().lower()[:3]
    if key not in WEEKDAYS:
        raise ValueError(f"bad weekday {day!r}, expected one of {', '.join(WEEKDAYS)}")
    return WEEKDAYS.index(key)


def _range_option(config: dict[str, Any], key: str, default: TimeRange | None) -> TimeRange | None:
    if key not in config:
        return default
    value = config[key]
    if value is None or value == "":
        return None
    return TimeRange.parse(str(value))


# --- text helpers --------------------------------------------------------------------------------------------


def clean_text(text: str) -> str:
    """Remove control and other non-printable characters (Unicode category C*)."""
    return "".join(ch for ch in text if not unicodedata.category(ch).startswith("C"))


def clip(text: str, limit: int) -> str:
    return text[:limit]


def project_name(payload: dict[str, Any]) -> str:
    """Basename of project_dir (added by sc-hook from CLAUDE_PROJECT_DIR) or cwd, cleaned, at most 12 code points.

    Accepts POSIX (WSL) and Windows paths."""
    for key in ("project_dir", "cwd"):
        value = payload.get(key)
        if not isinstance(value, str):
            continue
        base = re.split(r"[\\/]+", value.strip().rstrip("\\/"))[-1]
        name = clip(clean_text(base).strip(), NAME_MAX).strip()
        if name:
            return name
    return DEFAULT_NAME


# --- session table -------------------------------------------------------------------------------------------


@dataclass
class Session:
    session_id: str
    slot: int
    name: str
    st: str
    last_seen: float
    act: str = ""  # current activity while working (protocol v1.1), "" = none
    ctx: int | None = None  # context window usage in percent, from the status-line script
    agent_ids: set[str] = field(default_factory=set)  # running subagents
    anon_agents: int = 0  # running subagents whose SubagentStart had no agent_id
    run: bool = False  # created by sc-run
    ended_at: float | None = None  # sc-run: when the command finished

    def entry(self) -> dict[str, Any]:
        """This session in the state message; act and ctx only when set."""
        item: dict[str, Any] = {"slot": self.slot, "name": self.name, "st": self.st}
        act = "agents" if self.agent_ids or self.anon_agents else self.act
        if act:
            item["act"] = act
        if self.ctx is not None:
            item["ctx"] = self.ctx
        return item


@dataclass
class HookResult:
    """What a hook changed: whether the state snapshot changed, and robot events to send (without t/id)."""

    state_changed: bool = False
    events: list[dict[str, Any]] = field(default_factory=list)


Handler = Callable[[str, dict[str, Any], float], list[dict[str, Any]]]


class SessionTable:
    """Thread-safe table of Claude Code sessions keyed by session_id."""

    def __init__(
        self,
        quiet: QuietHours | None = None,
        *,
        max_slots: int = MAX_SLOTS,
        expiry_s: float = EXPIRY_S,
        clock: Callable[[], float] = time.monotonic,
        local_now: Callable[[], datetime] = datetime.now,
        detector: CommandDetector | None = None,
    ) -> None:
        self._quiet = quiet if quiet is not None else QuietHours()
        self._max_slots = max_slots
        self._expiry_s = expiry_s
        self._clock = clock
        self._local_now = local_now
        self._detector = detector if detector is not None else CommandDetector()
        self._lock = threading.Lock()
        self._sessions: dict[str, Session] = {}
        self._ended: dict[str, float] = {}  # session_id -> end time, to ignore late async hooks
        self._handlers: dict[str, Handler] = {
            "SessionStart": self._on_session_start,
            "SessionEnd": self._on_session_end,
            "UserPromptSubmit": self._on_prompt,
            "PreToolUse": self._on_pre_tool,
            "PostToolUse": self._on_post_tool,
            "PostToolUseFailure": self._on_tool_failure,
            "Notification": self._on_notification,
            "Stop": lambda sid, payload, now: self._finish(sid, payload, now, "done", "done", "done"),
            "StopFailure": lambda sid, payload, now: self._finish(sid, payload, now, "err", "err", "error"),
            "PreCompact": self._on_pre_compact,
            "SubagentStart": self._on_subagent_start,
            "SubagentStop": self._on_subagent_stop,
            "ScRunStart": self._on_run_start,
            "ScRunEnd": self._on_run_end,
            "StatusLine": self._on_status_line,
        }

    def handle(self, payload: dict[str, Any]) -> HookResult:
        event = payload.get("hook_event_name")
        sid = payload.get("session_id")
        if not isinstance(event, str) or not isinstance(sid, str) or not sid:
            return HookResult()
        handler = self._handlers.get(event)
        if handler is None:
            return HookResult()
        with self._lock:
            now = self._clock()
            self._forget_ended(now)
            if sid in self._ended and event not in ("SessionStart", "SessionEnd"):
                return HookResult()  # late async hook of an ended session
            before = self._entries()
            events = handler(sid, payload, now)
            return HookResult(state_changed=self._entries() != before, events=events)

    def expire(self) -> bool:
        """Drop sessions without events for expiry_s (sc-run sessions: RUN_LINGER_S after their end); True when
        the snapshot changed."""
        with self._lock:
            now = self._clock()
            stale = [s for s in self._sessions.values() if self._expired(s, now)]
            for session in stale:
                del self._sessions[session.session_id]
                log.info("session %s %r slot %d expired", session.session_id[:8], session.name, session.slot)
            return bool(stale)

    def snapshot(self) -> dict[str, Any]:
        """Body of the protocol's state message: {"sessions": [{slot, name, st, act?, ctx?}, ...]} sorted by slot."""
        with self._lock:
            return {"sessions": self._entries()}

    # --- event handlers (lock held); each returns the robot events to send ---

    def _on_session_start(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        self._ended.pop(sid, None)
        session = self._sessions.get(sid)
        if session is None:
            self._create(sid, payload, now, "idle", explicit=True)
            return []
        quiet_for = now - session.last_seen
        session.last_seen = now
        # SessionStart also fires after a compaction (source "compact"); the session keeps working then. Async
        # hooks may arrive out of order, so a busy session with events in the last seconds is not reset either.
        if payload.get("source") != "compact" and (session.st in ("done", "err") or quiet_for > RESTART_GRACE_S):
            self._set_state(session, "idle")
            session.act = ""
        return []

    def _on_session_end(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        self._ended[sid] = now
        session = self._sessions.pop(sid, None)
        if session is not None:
            log.info("session %s %r slot %d ended", sid[:8], session.name, session.slot)
        return []

    def _on_prompt(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        session = self._find(sid, payload, now, create="work", explicit=True)
        if session is not None:
            self._set_state(session, "work")
            session.act = "think"
        return []

    def _on_pre_tool(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        # Never creates a session and never changes st (wait -> work happens on PostToolUse). Tool calls of
        # subagents (agent_id set) only keep the session alive: they must not flip the main activity.
        session = self._find(sid, payload, now)
        if session is not None and not payload.get("agent_id") and session.st == "work":
            session.act = self._tool_act(payload)
        return []

    def _on_post_tool(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        # A tool ran, so a pending permission prompt was answered: back from "wait" to "work". No new sessions:
        # PostToolUse fires for every tool call. Subagent tool calls do not answer the main session's prompt.
        session = self._find(sid, payload, now)
        if session is None:
            return []
        if session.st == "wait" and not payload.get("agent_id"):
            self._set_state(session, "work")
        response = payload.get("tool_response")
        if isinstance(response, dict):
            if response.get("interrupted"):
                return []
            output = "\n".join(str(response.get(key) or "") for key in ("stdout", "stderr"))
        else:
            output = response if isinstance(response, str) else ""
        return self._test_result(session, payload, output, 0)

    def _on_tool_failure(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        # Only failing test and build commands are shown; other failures are routine and give no event.
        session = self._find(sid, payload, now, create="work")
        if session is None:
            return []
        if session.st != "wait":  # a failing tool (e.g. of a subagent) does not answer a pending prompt
            self._set_state(session, "work")
        if payload.get("is_interrupt"):
            return []
        error = payload.get("error")
        code = exit_code_from_error(error)
        return self._test_result(session, payload, error if isinstance(error, str) else "", code or 1)

    def _on_notification(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        kind = payload.get("notification_type")
        if kind == "idle_prompt":
            # Claude Code waits for a prompt. After an interrupt no Stop arrives, so a "work" or "wait" left over is
            # corrected to "idle" silently; "done" and "err" keep their meaning until the next prompt.
            session = self._find(sid, payload, now)
            if session is not None and session.st in ("work", "wait"):
                self._set_state(session, "idle")
                session.act = ""
            return []
        if kind not in NEEDS_TYPES:
            return []
        session = self._find(sid, payload, now, create="wait")
        if session is None:
            return []
        self._set_state(session, "wait")
        session.act = ""
        return [self._event(session, "needs", "needs you", snd=self._quiet.sound_allowed(self._local_now()))]

    def _finish(
        self, sid: str, payload: dict[str, Any], now: float, st: str, ev: str, text: str
    ) -> list[dict[str, Any]]:
        session = self._find(sid, payload, now, create=st)
        if session is None:
            return []
        self._set_state(session, st)
        session.act = ""
        session.agent_ids.clear()
        session.anon_agents = 0
        return [self._event(session, ev, text)]

    def _on_pre_compact(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        session = self._find(sid, payload, now, create="work")
        return [self._event(session, "compact", "compacting")] if session is not None else []

    def _on_subagent_start(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        session = self._find(sid, payload, now)
        if session is not None:
            agent = payload.get("agent_id")
            if isinstance(agent, str) and agent:
                session.agent_ids.add(agent)
            else:
                session.anon_agents += 1
        return []

    def _on_subagent_stop(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        session = self._find(sid, payload, now)
        if session is not None:
            agent = payload.get("agent_id")
            if isinstance(agent, str) and agent in session.agent_ids:
                session.agent_ids.discard(agent)
            elif session.anon_agents:  # never below zero
                session.anon_agents -= 1
        return []

    def _on_run_start(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        session = self._find(sid, payload, now, create="work", explicit=True)
        if session is not None:
            session.run, session.ended_at = True, None
            self._set_state(session, "work")
            session.act = "test"
        return []

    def _on_run_end(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        session = self._find(sid, payload, now, create="work", explicit=True)
        if session is None:
            return []
        session.run, session.ended_at, session.act = True, now, ""
        code = payload.get("exit_code")
        code = code if isinstance(code, int) and not isinstance(code, bool) else None
        outcome = summarize(payload.get("output"), code, self._detector.kind(payload.get("command")) or "run")
        succeeded = code == 0 if code is not None else outcome.ok
        self._set_state(session, "done" if succeeded else "err")
        return [self._event(session, "pass" if outcome.ok else "fail", outcome.summary)]

    def _on_status_line(self, sid: str, payload: dict[str, Any], now: float) -> list[dict[str, Any]]:
        session = self._find(sid, payload, now)  # never creates
        ctx = payload.get("ctx")
        if session is not None and isinstance(ctx, (int, float)) and not isinstance(ctx, bool):
            session.ctx = max(0, min(100, int(round(ctx))))
        return []

    # --- helpers (lock held) ---

    def _entries(self) -> list[dict[str, Any]]:
        return [s.entry() for s in sorted(self._sessions.values(), key=lambda s: s.slot)]

    def _find(
        self, sid: str, payload: dict[str, Any], now: float, create: str | None = None, explicit: bool = False
    ) -> Session | None:
        """The session (refreshed), created with state `create` when it is unknown and `create` is given."""
        session = self._sessions.get(sid)
        if session is None and create is not None:
            # Created on any mapped event, so a session survives an agent restart or a 30-minute expiry.
            session = self._create(sid, payload, now, create, explicit)
        if session is not None:
            session.last_seen = now
        return session

    def _tool_act(self, payload: dict[str, Any]) -> str:
        tool = payload.get("tool_name")
        if tool == "Bash":
            return "test" if self._detector.kind(self._command(payload)) else "run"
        return TOOL_ACTS.get(tool, "run") if isinstance(tool, str) else "run"

    @staticmethod
    def _command(payload: dict[str, Any]) -> str | None:
        tool_input = payload.get("tool_input")
        command = tool_input.get("command") if isinstance(tool_input, dict) else None
        return command if isinstance(command, str) else None

    def _test_result(
        self, session: Session, payload: dict[str, Any], output: str, exit_code: int
    ) -> list[dict[str, Any]]:
        """pass/fail event for a finished Bash test or build command; nothing for other tools and commands."""
        if payload.get("tool_name") != "Bash":
            return []
        kind = self._detector.kind(self._command(payload))
        if kind is None:
            return []
        outcome = summarize(output, exit_code, kind)
        return [self._event(session, "pass" if outcome.ok else "fail", outcome.summary)]

    def _event(self, session: Session, ev: str, text: str, snd: bool | None = None) -> dict[str, Any]:
        event: dict[str, Any] = {"slot": session.slot, "ev": ev, "text": clip(f"{session.name}: {text}", TEXT_MAX)}
        if snd is not None:
            event["snd"] = snd
        log.info("event %s slot %d %r%s", ev, session.slot, event["text"], " (sound)" if snd else "")
        return event

    def _expired(self, session: Session, now: float) -> bool:
        if session.run:
            if session.ended_at is not None:
                return now - session.ended_at > RUN_LINGER_S
            return now - session.last_seen > RUN_MAX_S
        return now - session.last_seen > self._expiry_s

    def _create(self, sid: str, payload: dict[str, Any], now: float, st: str, explicit: bool) -> Session | None:
        name = project_name(payload)
        used = {s.slot for s in self._sessions.values()}
        slot = next((i for i in range(self._max_slots) if i not in used), None)
        if slot is None:
            # Only explicit starts are logged at info, so a busy ignored session does not flood the log.
            level = logging.INFO if explicit else logging.DEBUG
            log.log(level, "all %d slots busy, ignoring session %s %r", self._max_slots, sid[:8], name)
            return None
        session = Session(session_id=sid, slot=slot, name=name, st=st, last_seen=now)
        self._sessions[sid] = session
        log.info("session %s %r slot %d created (%s)", sid[:8], name, slot, st)
        return session

    def _set_state(self, session: Session, st: str) -> None:
        if session.st != st:
            log.info("session %s %r slot %d: %s -> %s", session.session_id[:8], session.name, session.slot,
                     session.st, st)
            session.st = st

    def _forget_ended(self, now: float) -> None:
        for sid in [sid for sid, ended in self._ended.items() if now - ended > ENDED_MEMORY_S]:
            del self._ended[sid]

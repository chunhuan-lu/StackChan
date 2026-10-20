"""Claude Code session table for the StackChan status robot (pure logic, no I/O).

Maps Claude Code hook events (the JSON a hook receives on stdin) to the robot model of REQ-003: up to five
sessions in slots 0-4, each with a short project name and a state, plus one-off robot events.
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

log = logging.getLogger("sc.sessions")

MAX_SLOTS = 5
NAME_MAX = 12  # Unicode code points
TEXT_MAX = 40  # Unicode code points
EXPIRY_S = 30 * 60.0
ENDED_MEMORY_S = 60.0  # late async hooks of an ended session are ignored for this long
RESTART_GRACE_S = 5.0  # a SessionStart this close to other events of a busy session is out of order
DEFAULT_NAME = "claude"
WEEKDAYS = ("mon", "tue", "wed", "thu", "fri", "sat", "sun")

# idle_prompt is not a call-back: Claude Code sends it about 60 s after every Stop, which would turn each finished
# task into a second "needs you". It only corrects a stale "work"/"wait" (see _idle_prompt).
NEEDS_TYPES = frozenset({"permission_prompt", "agent_needs_input", "elicitation_dialog", "elicitation_url_dialog"})

# hook_event_name -> (new session state or None, robot event or None, bubble text after "<name>: ")
_RULES: dict[str, tuple[str | None, str | None, str]] = {
    "UserPromptSubmit": ("work", None, ""),
    "PostToolUseFailure": ("work", "fail", "failed"),
    "Notification": ("wait", "needs", "needs you"),
    "Stop": ("done", "done", "done"),
    "StopFailure": ("err", "err", "error"),
    "PreCompact": (None, "compact", "compacting"),
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


@dataclass
class HookResult:
    """What a hook changed: whether the state snapshot changed, and robot events to send (without t/id)."""

    state_changed: bool = False
    events: list[dict[str, Any]] = field(default_factory=list)


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
    ) -> None:
        self._quiet = quiet if quiet is not None else QuietHours()
        self._max_slots = max_slots
        self._expiry_s = expiry_s
        self._clock = clock
        self._local_now = local_now
        self._lock = threading.Lock()
        self._sessions: dict[str, Session] = {}
        self._ended: dict[str, float] = {}  # session_id -> end time, to ignore late async hooks

    def handle(self, payload: dict[str, Any]) -> HookResult:
        event = payload.get("hook_event_name")
        sid = payload.get("session_id")
        if not isinstance(event, str) or not isinstance(sid, str) or not sid:
            return HookResult()
        with self._lock:
            now = self._clock()
            self._forget_ended(now)
            if event == "SessionEnd":
                return self._end(sid, now)
            if event == "SessionStart":
                return self._start(sid, payload, now)
            if event == "PostToolUse":
                return self._tool_done(sid, payload, now)
            rule = _RULES.get(event)
            if rule is None or sid in self._ended:
                return HookResult()
            if event == "Notification":
                kind = payload.get("notification_type")
                if kind == "idle_prompt":
                    return self._idle_prompt(sid, now)
                if kind not in NEEDS_TYPES:
                    return HookResult()
            return self._apply(sid, event, payload, now, rule)

    def expire(self) -> bool:
        """Drop sessions without events for expiry_s; True when the snapshot changed."""
        with self._lock:
            now = self._clock()
            stale = [s for s in self._sessions.values() if now - s.last_seen > self._expiry_s]
            for session in stale:
                del self._sessions[session.session_id]
                log.info("session %s %r slot %d expired", session.session_id[:8], session.name, session.slot)
            return bool(stale)

    def snapshot(self) -> dict[str, Any]:
        """Body of the protocol's state message: {"sessions": [{slot, name, st}, ...]} sorted by slot."""
        with self._lock:
            ordered = sorted(self._sessions.values(), key=lambda s: s.slot)
            return {"sessions": [{"slot": s.slot, "name": s.name, "st": s.st} for s in ordered]}

    # --- internals (lock held) ---

    def _start(self, sid: str, payload: dict[str, Any], now: float) -> HookResult:
        self._ended.pop(sid, None)
        session = self._sessions.get(sid)
        if session is None:
            return HookResult(state_changed=self._create(sid, payload, now, "idle", explicit=True) is not None)
        quiet_for = now - session.last_seen
        session.last_seen = now
        # SessionStart also fires after a compaction (source "compact"); the session keeps working then. Async
        # hooks may arrive out of order, so a busy session with events in the last seconds is not reset either.
        if payload.get("source") == "compact":
            return HookResult()
        if session.st in ("done", "err") or quiet_for > RESTART_GRACE_S:
            return HookResult(state_changed=self._set_state(session, "idle"))
        return HookResult()

    def _tool_done(self, sid: str, payload: dict[str, Any], now: float) -> HookResult:
        # A tool ran, so a pending permission prompt was answered: back from "wait" to "work". State only, and
        # no new sessions: PostToolUse fires for every tool call. Tool calls of subagents (agent_id set) do not
        # answer the main session's prompt.
        session = self._sessions.get(sid)
        if session is None or sid in self._ended or payload.get("agent_id"):
            return HookResult()
        session.last_seen = now
        if session.st == "wait":
            return HookResult(state_changed=self._set_state(session, "work"))
        return HookResult()

    def _idle_prompt(self, sid: str, now: float) -> HookResult:
        # Claude Code waits for a prompt. After an interrupt no Stop arrives, so a "work" or "wait" left over is
        # corrected to "idle" silently; "done" and "err" keep their meaning until the next prompt.
        session = self._sessions.get(sid)
        if session is None:
            return HookResult()
        session.last_seen = now
        if session.st in ("work", "wait"):
            return HookResult(state_changed=self._set_state(session, "idle"))
        return HookResult()

    def _end(self, sid: str, now: float) -> HookResult:
        self._ended[sid] = now
        session = self._sessions.pop(sid, None)
        if session is None:
            return HookResult()
        log.info("session %s %r slot %d ended", sid[:8], session.name, session.slot)
        return HookResult(state_changed=True)

    def _apply(
        self, sid: str, event: str, payload: dict[str, Any], now: float, rule: tuple[str | None, str | None, str]
    ) -> HookResult:
        new_st, ev, suffix = rule
        session = self._sessions.get(sid)
        created = False
        if session is None:
            # Created on any mapped event, so a session survives an agent restart or a 30-minute expiry.
            session = self._create(sid, payload, now, new_st or "work", explicit=(event == "UserPromptSubmit"))
            if session is None:
                return HookResult()
            created = True
        if event == "PostToolUseFailure" and session.st == "wait":
            new_st = None  # a failing tool (e.g. of a subagent) does not answer the pending prompt
        session.last_seen = now
        changed = created
        if new_st is not None and not created:
            changed = self._set_state(session, new_st)
        result = HookResult(state_changed=changed)
        if ev is not None:
            text = clip(f"{session.name}: {suffix}", TEXT_MAX)
            robot_event: dict[str, Any] = {"slot": session.slot, "ev": ev, "text": text}
            if ev == "needs":
                robot_event["snd"] = self._quiet.sound_allowed(self._local_now())
            sound = " (sound)" if robot_event.get("snd") else ""
            log.info("event %s slot %d %r%s", ev, session.slot, session.name, sound)
            result.events.append(robot_event)
        return result

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

    def _set_state(self, session: Session, st: str) -> bool:
        if session.st == st:
            return False
        log.info("session %s %r slot %d: %s -> %s", session.session_id[:8], session.name, session.slot, session.st, st)
        session.st = st
        return True

    def _forget_ended(self, now: float) -> None:
        for sid in [sid for sid, ended in self._ended.items() if now - ended > ENDED_MEMORY_S]:
            del self._ended[sid]

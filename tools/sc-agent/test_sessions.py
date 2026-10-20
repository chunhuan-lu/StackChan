"""Tests for the Claude Code session table (sessions.py). Stdlib only."""

import sys

sys.dont_write_bytecode = True

import logging  # noqa: E402
import unittest  # noqa: E402
from datetime import datetime  # noqa: E402

from sessions import (  # noqa: E402
    ENDED_MEMORY_S,
    EXPIRY_S,
    RESTART_GRACE_S,
    QuietHours,
    SessionTable,
    TimeRange,
    project_name,
    quiet_hours_from_config,
)

WEEKDAY_10 = datetime(2026, 10, 21, 10, 0)  # Wednesday
WEEKDAY_19 = datetime(2026, 10, 21, 19, 0)
SATURDAY_10 = datetime(2026, 10, 24, 10, 0)
SUNDAY_2330 = datetime(2026, 10, 25, 23, 30)


class FakeClock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now


def hook(event: str, sid: str = "s1", cwd: str = "/home/me/StackChan", **extra):
    return {"hook_event_name": event, "session_id": sid, "cwd": cwd, **extra}


class TableTestCase(unittest.TestCase):
    def setUp(self) -> None:
        logging.disable(logging.CRITICAL)
        self.clock = FakeClock()
        self.local_now = SATURDAY_10
        self.table = SessionTable(clock=self.clock, local_now=lambda: self.local_now)

    def tearDown(self) -> None:
        logging.disable(logging.NOTSET)

    def sessions(self):
        return self.table.snapshot()["sessions"]


class HookMappingTest(TableTestCase):
    def test_session_start_creates_idle(self):
        result = self.table.handle(hook("SessionStart"))
        self.assertTrue(result.state_changed)
        self.assertEqual(result.events, [])
        self.assertEqual(self.sessions(), [{"slot": 0, "name": "StackChan", "st": "idle"}])

    def test_user_prompt_submit_works_and_creates(self):
        result = self.table.handle(hook("UserPromptSubmit", sid="new"))
        self.assertTrue(result.state_changed)
        self.assertEqual(self.sessions(), [{"slot": 0, "name": "StackChan", "st": "work"}])

    def test_post_tool_use_failure(self):
        self.table.handle(hook("SessionStart"))
        result = self.table.handle(hook("PostToolUseFailure", error="boom"))
        self.assertTrue(result.state_changed)
        self.assertEqual(result.events, [{"slot": 0, "ev": "fail", "text": "StackChan: failed"}])
        self.assertEqual(self.sessions()[0]["st"], "work")
        result = self.table.handle(hook("PostToolUseFailure"))
        self.assertFalse(result.state_changed)  # already working
        self.assertEqual(len(result.events), 1)

    def test_post_tool_use_failure_keeps_wait(self):
        self.table.handle(hook("UserPromptSubmit"))
        self.table.handle(hook("Notification", notification_type="permission_prompt"))
        result = self.table.handle(hook("PostToolUseFailure", agent_id="sub1"))
        self.assertFalse(result.state_changed)
        self.assertEqual(result.events, [{"slot": 0, "ev": "fail", "text": "StackChan: failed"}])
        self.assertEqual(self.sessions()[0]["st"], "wait")

    def test_notification_needs_input(self):
        for kind in ("permission_prompt", "agent_needs_input", "elicitation_dialog", "elicitation_url_dialog"):
            with self.subTest(kind=kind):
                self.table.handle(hook("UserPromptSubmit"))
                result = self.table.handle(hook("Notification", notification_type=kind))
                self.assertTrue(result.state_changed)
                needs = {"slot": 0, "ev": "needs", "text": "StackChan: needs you", "snd": True}
                self.assertEqual(result.events, [needs])
                self.assertEqual(self.sessions()[0]["st"], "wait")

    def test_needs_sound_follows_quiet_hours(self):
        self.table.handle(hook("UserPromptSubmit"))
        self.local_now = WEEKDAY_10
        result = self.table.handle(hook("Notification", notification_type="permission_prompt"))
        self.assertFalse(result.events[0]["snd"])

    def test_post_tool_use_clears_wait(self):
        self.table.handle(hook("UserPromptSubmit"))
        self.table.handle(hook("Notification", notification_type="permission_prompt"))
        result = self.table.handle(hook("PostToolUse"))
        self.assertTrue(result.state_changed)
        self.assertEqual(result.events, [])
        self.assertEqual(self.sessions()[0]["st"], "work")
        self.assertFalse(self.table.handle(hook("PostToolUse")).state_changed)  # already working

    def test_post_tool_use_creates_no_session(self):
        result = self.table.handle(hook("PostToolUse"))
        self.assertFalse(result.state_changed)
        self.assertEqual(self.sessions(), [])

    def test_post_tool_use_of_subagent_keeps_wait(self):
        self.table.handle(hook("UserPromptSubmit"))
        self.table.handle(hook("Notification", notification_type="permission_prompt"))
        self.assertFalse(self.table.handle(hook("PostToolUse", agent_id="sub1", agent_type="Explore")).state_changed)
        self.assertEqual(self.sessions()[0]["st"], "wait")
        self.assertTrue(self.table.handle(hook("PostToolUse", agent_id="")).state_changed)  # main agent
        self.assertEqual(self.sessions()[0]["st"], "work")

    def test_idle_prompt_corrects_stale_work_and_wait(self):
        self.table.handle(hook("UserPromptSubmit"))  # interrupted by the user: no Stop follows
        result = self.table.handle(hook("Notification", notification_type="idle_prompt"))
        self.assertEqual((result.state_changed, result.events), (True, []))
        self.assertEqual(self.sessions()[0]["st"], "idle")
        self.table.handle(hook("Notification", notification_type="permission_prompt"))
        self.assertTrue(self.table.handle(hook("Notification", notification_type="idle_prompt")).state_changed)
        self.assertEqual(self.sessions()[0]["st"], "idle")

    def test_idle_prompt_keeps_done_and_err(self):
        for event, st in (("Stop", "done"), ("StopFailure", "err")):
            with self.subTest(event=event):
                self.table.handle(hook(event))
                result = self.table.handle(hook("Notification", notification_type="idle_prompt"))
                self.assertEqual((result.state_changed, result.events), (False, []))
                self.assertEqual(self.sessions()[0]["st"], st)

    def test_idle_prompt_creates_no_session(self):
        result = self.table.handle(hook("Notification", notification_type="idle_prompt"))
        self.assertEqual((result.state_changed, result.events), (False, []))
        self.assertEqual(self.sessions(), [])

    def test_other_notifications_ignored(self):
        self.table.handle(hook("UserPromptSubmit"))
        for kind in ("auth_success", "elicitation_complete", None):
            result = self.table.handle(hook("Notification", notification_type=kind))
            self.assertFalse(result.state_changed)
            self.assertEqual(result.events, [])
        self.assertEqual(self.sessions()[0]["st"], "work")

    def test_stop(self):
        self.table.handle(hook("UserPromptSubmit"))
        result = self.table.handle(hook("Stop"))
        self.assertTrue(result.state_changed)
        self.assertEqual(result.events, [{"slot": 0, "ev": "done", "text": "StackChan: done"}])
        self.assertEqual(self.sessions()[0]["st"], "done")

    def test_stop_failure(self):
        self.table.handle(hook("UserPromptSubmit"))
        result = self.table.handle(hook("StopFailure", error="rate_limit"))
        self.assertEqual(result.events, [{"slot": 0, "ev": "err", "text": "StackChan: error"}])
        self.assertEqual(self.sessions()[0]["st"], "err")

    def test_pre_compact_keeps_state(self):
        self.table.handle(hook("UserPromptSubmit"))
        result = self.table.handle(hook("PreCompact"))
        self.assertFalse(result.state_changed)
        self.assertEqual(result.events, [{"slot": 0, "ev": "compact", "text": "StackChan: compacting"}])
        self.assertEqual(self.sessions()[0]["st"], "work")

    def test_session_start_after_compaction_keeps_state(self):
        self.table.handle(hook("UserPromptSubmit"))
        self.clock.now += RESTART_GRACE_S + 1
        self.assertFalse(self.table.handle(hook("SessionStart", source="compact")).state_changed)
        self.assertEqual(self.sessions()[0]["st"], "work")
        self.clock.now += RESTART_GRACE_S + 1
        self.assertTrue(self.table.handle(hook("SessionStart", source="clear")).state_changed)
        self.assertEqual(self.sessions()[0]["st"], "idle")

    def test_out_of_order_session_start_keeps_busy_session(self):
        for busy in ("UserPromptSubmit", "Notification"):
            with self.subTest(busy=busy):
                self.table.handle(hook(busy, notification_type="permission_prompt"))
                st = self.sessions()[0]["st"]
                self.clock.now += RESTART_GRACE_S - 1
                self.assertFalse(self.table.handle(hook("SessionStart", source="startup")).state_changed)
                self.assertEqual(self.sessions()[0]["st"], st)

    def test_session_start_resets_done_and_err_at_once(self):
        for event in ("Stop", "StopFailure"):
            with self.subTest(event=event):
                self.table.handle(hook(event))
                self.assertTrue(self.table.handle(hook("SessionStart", source="clear")).state_changed)
                self.assertEqual(self.sessions()[0]["st"], "idle")

    def test_session_end_removes(self):
        self.table.handle(hook("SessionStart"))
        self.assertTrue(self.table.handle(hook("SessionEnd", reason="exit")).state_changed)
        self.assertEqual(self.sessions(), [])
        self.assertFalse(self.table.handle(hook("SessionEnd")).state_changed)

    def test_unknown_and_malformed_ignored(self):
        self.table.handle(hook("SessionStart"))
        for payload in (hook("PreToolUse"), hook("SubagentStop"), {"session_id": "s1"}, hook("Stop", sid=""), {}):
            with self.subTest(payload=payload):
                result = self.table.handle(payload)
                self.assertFalse(result.state_changed)
                self.assertEqual(result.events, [])
        self.assertEqual(self.sessions()[0]["st"], "idle")

    def test_mapped_event_creates_unknown_session(self):
        # e.g. after an agent restart: the "done" call-back must not be lost.
        result = self.table.handle(hook("Stop", sid="old"))
        self.assertTrue(result.state_changed)
        self.assertEqual(result.events[0]["ev"], "done")
        self.assertEqual(self.sessions(), [{"slot": 0, "name": "StackChan", "st": "done"}])

    def test_late_hook_after_session_end_ignored(self):
        self.table.handle(hook("UserPromptSubmit"))
        self.table.handle(hook("SessionEnd"))
        result = self.table.handle(hook("Stop"))  # async hooks may arrive out of order
        self.assertEqual((result.state_changed, result.events), (False, []))
        self.assertEqual(self.sessions(), [])
        self.clock.now += ENDED_MEMORY_S + 1
        self.assertTrue(self.table.handle(hook("Stop")).state_changed)

    def test_resume_after_end(self):
        self.table.handle(hook("SessionStart"))
        self.table.handle(hook("SessionEnd"))
        self.assertTrue(self.table.handle(hook("SessionStart", source="resume")).state_changed)
        self.assertEqual(len(self.sessions()), 1)

    def test_text_limit(self):
        table = SessionTable(clock=self.clock)
        table.handle(hook("SessionStart"))
        table._sessions["s1"].name = "N" * 39  # longer than any real name, to exercise the limit
        result = table.handle(hook("Stop"))
        self.assertEqual(len(result.events[0]["text"]), 40)


class SlotTest(TableTestCase):
    def test_first_free_slot_and_reuse(self):
        for i in range(5):
            self.table.handle(hook("SessionStart", sid=f"s{i}", cwd=f"/p/proj{i}"))
        self.assertEqual([s["slot"] for s in self.sessions()], [0, 1, 2, 3, 4])
        self.table.handle(hook("SessionEnd", sid="s1"))
        self.table.handle(hook("SessionEnd", sid="s3"))
        self.table.handle(hook("SessionStart", sid="s5", cwd="/p/new"))
        self.assertEqual({s["slot"]: s["name"] for s in self.sessions()}[1], "new")
        self.assertEqual([s["slot"] for s in self.sessions()], [0, 1, 2, 4])

    def test_sixth_session_ignored(self):
        for i in range(5):
            self.table.handle(hook("SessionStart", sid=f"s{i}"))
        for event in ("SessionStart", "UserPromptSubmit", "Stop"):
            result = self.table.handle(hook(event, sid="s6", notification_type="permission_prompt"))
            self.assertEqual((result.state_changed, result.events), (False, []))
        self.assertEqual(len(self.sessions()), 5)
        self.table.handle(hook("SessionEnd", sid="s2"))
        self.table.handle(hook("UserPromptSubmit", sid="s6"))
        self.assertEqual({s["slot"]: s["st"] for s in self.sessions()}[2], "work")

    def test_snapshot_sorted_by_slot(self):
        for i in range(3):
            self.table.handle(hook("SessionStart", sid=f"s{i}"))
        self.table.handle(hook("SessionEnd", sid="s0"))
        self.table.handle(hook("SessionStart", sid="s9"))
        self.assertEqual([s["slot"] for s in self.sessions()], [0, 1, 2])


class ExpiryTest(TableTestCase):
    def test_expires_after_30_minutes_without_events(self):
        self.table.handle(hook("SessionStart", sid="a"))
        self.table.handle(hook("SessionStart", sid="b"))
        self.clock.now += EXPIRY_S - 10
        self.table.handle(hook("UserPromptSubmit", sid="b"))
        self.assertFalse(self.table.expire())
        self.clock.now += 11
        self.assertTrue(self.table.expire())
        self.assertEqual([s["slot"] for s in self.sessions()], [1])
        self.assertFalse(self.table.expire())

    def test_ignored_event_does_not_refresh(self):
        self.table.handle(hook("SessionStart"))
        self.clock.now += EXPIRY_S - 1
        self.table.handle(hook("PreToolUse"))
        self.clock.now += 2
        self.assertTrue(self.table.expire())


class NameTest(unittest.TestCase):
    def test_posix_and_windows_paths(self):
        self.assertEqual(project_name({"cwd": "/home/me/StackChan"}), "StackChan")
        self.assertEqual(project_name({"cwd": "/home/me/StackChan/"}), "StackChan")
        self.assertEqual(project_name({"cwd": "E:\\workbench\\M5\\docs"}), "docs")
        self.assertEqual(project_name({"cwd": "C:/Users/me/web-app"}), "web-app")

    def test_project_dir_preferred(self):
        self.assertEqual(project_name({"cwd": "/w/StackChan/firmware", "project_dir": "/w/StackChan"}), "StackChan")
        self.assertEqual(project_name({"cwd": "/w/app", "project_dir": ""}), "app")

    def test_truncated_to_12_code_points(self):
        self.assertEqual(project_name({"cwd": "/p/abcdefghijklmnop"}), "abcdefghijkl")
        self.assertEqual(project_name({"cwd": "/p/机器人状态显示项目第二版本"}), "机器人状态显示项目第二版")

    def test_control_chars_removed(self):
        self.assertEqual(project_name({"cwd": "/p/St\x07ack\x1bChan\x00\n"}), "StackChan")
        self.assertEqual(project_name({"cwd": "/p/​ab‮cd"}), "abcd")  # format characters too

    def test_fallback(self):
        self.assertEqual(project_name({}), "claude")
        self.assertEqual(project_name({"cwd": "/"}), "claude")


class QuietHoursTest(unittest.TestCase):
    def test_default_table(self):
        quiet = QuietHours()
        cases = [
            (WEEKDAY_10, False),
            (SATURDAY_10, True),
            (SUNDAY_2330, False),
            (datetime(2026, 10, 21, 23, 30), False),
            (WEEKDAY_19, True),
            (datetime(2026, 10, 21, 7, 59), False),  # night
            (datetime(2026, 10, 21, 18, 0), True),  # work ends at 18:00
            (datetime(2026, 10, 24, 8, 0), True),  # Saturday morning after the night
            (datetime(2026, 10, 24, 22, 59), True),
        ]
        for when, expected in cases:
            with self.subTest(when=when):
                self.assertEqual(quiet.sound_allowed(when), expected)

    def test_from_config(self):
        config = {"quiet_days": ["Mon", "tue"], "quiet_work": "09:30-17:00", "quiet_night": None}
        quiet = quiet_hours_from_config(config)
        self.assertEqual(quiet.work_days, frozenset({0, 1}))
        self.assertEqual(quiet.work, TimeRange.parse("09:30-17:00"))
        self.assertIsNone(quiet.night)
        self.assertTrue(quiet.sound_allowed(datetime(2026, 10, 21, 10, 0)))  # Wednesday is no work day now
        self.assertTrue(quiet.sound_allowed(datetime(2026, 10, 19, 23, 30)))  # no night rule
        self.assertEqual(quiet_hours_from_config({}), QuietHours())

    def test_bad_config(self):
        for config in ({"quiet_days": ["xyz"]}, {"quiet_work": "8-18"}, {"quiet_night": "25:00-08:00"}):
            with self.subTest(config=config), self.assertRaises(ValueError):
                quiet_hours_from_config(config)


if __name__ == "__main__":
    unittest.main()

"""Tests for the sc-agent daemon with an emulated robot: no serial port and no pyserial needed. Stdlib only.

The HTTP endpoint binds an ephemeral port on 127.0.0.1.
"""

import sys

sys.dont_write_bytecode = True

import json  # noqa: E402
import logging  # noqa: E402
import tempfile  # noqa: E402
import threading  # noqa: E402
import time  # noqa: E402
import unittest  # noqa: E402
import urllib.error  # noqa: E402
import urllib.request  # noqa: E402
from dataclasses import replace  # noqa: E402
from pathlib import Path  # noqa: E402

import merge_hooks  # noqa: E402
from sc_agent import Agent, Config, ConfigError, LinkTiming, PortInfo, load_config, load_or_create_token  # noqa: E402
from sc_link import MAX_DEVICE_JSON, encode_frame, parse_line  # noqa: E402

TOKEN = "unit-test-token-not-a-secret"
FAST = LinkTiming(
    ack_timeout=0.15, ping_interval=0.4, state_interval=0.05, discovery_interval=0.05, hello_interval=0.2,
    port_cooldown=1.0,
)
HELLO_TRIES = 1 + FAST.retries  # frames per unanswered hello
NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))
WORK = [{"slot": 0, "name": "StackChan", "st": "work", "act": "think"}]


class FakeWriteTimeout(Exception):
    pass


class FakeRobot:
    """Device side of the link: parses host frames and answers like the firmware, with a log line in front.

    Every ack carries "boot", "n" (used slots) and "ai" like the current firmware; legacy=True leaves them out."""

    def __init__(self, port: str = "COM99", serial: str | None = "SN-99", legacy: bool = False) -> None:
        self.port, self.serial, self.legacy = port, serial, legacy
        self.lock = threading.Lock()
        self.frames: list[dict] = []  # every valid frame received, repeats included
        self.out = bytearray()
        self.boot = 1001
        self.n = 0  # sessions the robot currently holds
        self.ai = False  # AI Agent started
        self.settings = {"sleep": 1, "face_track": 1}
        self.drop = 0  # ignore this many next frames (frame loss) ...
        self.drop_type: str | None = None  # ... of this message type only, when set
        self.silent = False  # firmware without the link: never answers
        self.fail_reads = False
        self.closed = True
        self.present = True
        self.opens = 0

    def write(self, data: bytes) -> int:
        with self.lock:
            for line in data.split(b"\n"):
                frame = parse_line(line)
                if frame is None:
                    continue
                self.frames.append(frame)
                if self.drop > 0 and self.drop_type in (None, frame["t"]):
                    self.drop -= 1
                    continue
                if self.silent:
                    continue
                if frame["t"] == "state":
                    self.n = len(frame["sessions"])
                elif frame["t"] == "clear":
                    self.n = 0
                elif frame["t"] == "agent":
                    self.ai = True
                ack = {"t": "ack", "id": frame["id"], "ok": True, "st": "idle"}
                if frame["t"] == "set":
                    if frame["k"] in self.settings:
                        if "v" in frame:
                            self.settings[frame["k"]] = max(0, min(1, frame["v"]))  # both test settings are bools
                        ack["val"] = self.settings[frame["k"]]
                    else:
                        ack.update(ok=False, err="key")
                if not self.legacy:
                    ack.update(boot=self.boot, n=self.n, ai=self.ai)
                if frame["t"] == "hello":
                    ack.update(v=1, fw="test-fw", boot=self.boot, up=5)
                self.out += b"\x1b[0;32mI (100) robot: got " + frame["t"].encode() + b"\x1b[0m\r\n"
                self.out += encode_frame(ack, max_json=MAX_DEVICE_JSON).rstrip(b"\n") + b"\r\n"
        return len(data)

    def read(self, size: int) -> bytes:
        if self.fail_reads:
            raise OSError("device disappeared")
        with self.lock:
            data = bytes(self.out[:size])
            del self.out[:size]
        if not data:
            time.sleep(0.01)
        return data

    def close(self) -> None:
        self.closed = True

    def reboot(self) -> None:
        """Restart without USB re-enumeration: new boot id, sessions gone."""
        with self.lock:
            self.boot += 1
            self.n = 0

    def wipe(self) -> None:
        """The robot dropped its sessions on its own (e.g. staleness timeout)."""
        with self.lock:
            self.n = 0

    def types(self) -> list[str]:
        with self.lock:
            return [f["t"] for f in self.frames]

    def types_after_hello(self, n: int) -> list[str]:
        """Message types received after the n-th hello frame (1-based)."""
        types = self.types()
        hellos = [i for i, t in enumerate(types) if t == "hello"]
        return types[hellos[n - 1] + 1 :] if len(hellos) >= n else []

    def last(self, kind: str) -> dict | None:
        with self.lock:
            matches = [f for f in self.frames if f["t"] == kind]
        return matches[-1] if matches else None


class FakeBackend:
    write_timeout_error = FakeWriteTimeout

    def __init__(self, *robots: FakeRobot) -> None:
        self.robots = {robot.port: robot for robot in robots}
        self.present = True  # unplug everything at once

    @property
    def opened(self) -> int:
        return sum(robot.opens for robot in self.robots.values())

    def find_ports(self) -> list[PortInfo]:
        if not self.present:
            return []
        return sorted((PortInfo(r.port, r.serial) for r in self.robots.values() if r.present), key=lambda p: p.device)

    def open(self, name: str) -> FakeRobot:
        robot = self.robots[name]
        robot.opens += 1
        robot.closed = False
        robot.fail_reads = False
        return robot


def wait_until(predicate, timeout: float = 3.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()


class AgentTestCase(unittest.TestCase):
    robots: tuple = ()
    config = Config(http_port=0)  # port 0 (ephemeral) is only possible when the Config is built directly
    timing = FAST

    def setUp(self) -> None:
        logging.disable(logging.CRITICAL)
        self.tmp = tempfile.TemporaryDirectory()
        self.backend = FakeBackend(*(self.robots or (FakeRobot(),)))
        self.robot = next(iter(self.backend.robots.values()))
        self.agent = Agent(self.config, TOKEN, Path(self.tmp.name), self.backend, timing=self.timing)
        self.agent.start()
        self.base = f"http://127.0.0.1:{self.agent.http_port}"

    def tearDown(self) -> None:
        self.agent.stop()
        self.tmp.cleanup()
        logging.disable(logging.NOTSET)

    def request(self, method: str, path: str, body: dict | None = None, token: str | None = TOKEN):
        data = json.dumps(body).encode() if body is not None else b""
        req = urllib.request.Request(self.base + path, data=data if method == "POST" else None, method=method)
        if token is not None:
            req.add_header("X-SC-Token", token)
        try:
            with NO_PROXY.open(req, timeout=10) as resp:
                return resp.status, json.loads(resp.read() or b"null")
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read() or b"null")

    def hook(self, event: str, sid: str = "s1", **extra):
        payload = {"hook_event_name": event, "session_id": sid, "cwd": "/home/me/StackChan", **extra}
        status, _ = self.request("POST", "/hook", payload)
        self.assertEqual(status, 200)

    def wait_linked(self, timeout: float = 3.0) -> None:
        linked = wait_until(lambda: self.agent.status()["link"] == "linked", timeout)
        self.assertTrue(linked, self.agent.status())


class AgentTest(AgentTestCase):
    def test_hello_then_state_on_connect(self):
        self.wait_linked()
        self.assertTrue(wait_until(lambda: "state" in self.robot.types()))
        types = self.robot.types()
        self.assertEqual(types[0], "hello")
        hello = self.robot.frames[0]
        self.assertEqual((hello["v"], hello["agent"][:9]), (1, "sc-agent/"))
        self.assertEqual(self.robot.last("state")["sessions"], [])
        status = self.agent.status()
        self.assertEqual((status["port"], status["serial"], status["boot"], status["fw"], status["robot_st"]),
                         ("COM99", "SN-99", 1001, "test-fw", "idle"))

    def test_ids_strictly_increase(self):
        self.wait_linked()
        self.hook("SessionStart")
        self.assertTrue(wait_until(lambda: self.robot.types().count("ping") >= 1, timeout=2))
        ids = [f["id"] for f in self.robot.frames]
        self.assertEqual(ids, sorted(set(ids)))

    def test_hooks_drive_state_and_events(self):
        self.wait_linked()
        self.hook("SessionStart")
        self.hook("UserPromptSubmit")
        self.hook("Notification", notification_type="permission_prompt")
        self.assertTrue(wait_until(lambda: self.robot.last("event") is not None))
        event = self.robot.last("event")
        self.assertEqual((event["slot"], event["ev"], event["text"]), (0, "needs", "StackChan: needs you"))
        self.assertIsInstance(event["snd"], bool)
        # The state with "wait" goes out before the event.
        with self.robot.lock:
            frames = list(self.robot.frames)
        event_index = next(i for i, f in enumerate(frames) if f["t"] == "event")
        states = [f for f in frames[:event_index] if f["t"] == "state"]
        self.assertEqual(states[-1]["sessions"], [{"slot": 0, "name": "StackChan", "st": "wait"}])
        self.assertEqual(self.agent.status()["sessions"], states[-1]["sessions"])

    def test_state_debounced(self):
        self.wait_linked()
        before = self.robot.types().count("state")
        for i in range(10):
            self.hook("SessionStart", sid=f"s{i % 3}")
            self.hook("SessionEnd", sid=f"s{i % 3}")
        self.hook("SessionStart", sid="last")
        self.assertTrue(wait_until(lambda: self.robot.last("state")["sessions"] == [
            {"slot": 0, "name": "StackChan", "st": "idle"}]))
        self.assertLess(self.robot.types().count("state") - before, 21)

    def test_retry_with_same_id(self):
        self.wait_linked()
        self.assertTrue(wait_until(lambda: "state" in self.robot.types()))
        self.robot.drop_type, self.robot.drop = "state", 2
        self.hook("UserPromptSubmit")
        self.assertTrue(wait_until(lambda: (self.robot.last("state") or {}).get("sessions")))
        self.assertTrue(wait_until(lambda: self.agent.status()["stats"]["retries"] >= 2))
        with self.robot.lock:
            work = [f for f in self.robot.frames if f["t"] == "state" and f["sessions"]]
        self.assertEqual(len(work), 3)
        self.assertEqual(len({f["id"] for f in work}), 1)
        self.assertEqual(self.agent.status()["link"], "linked")

    def test_lost_acks_restart_with_hello(self):
        self.wait_linked()
        self.robot.drop = 3  # all three attempts of the next message are lost
        self.assertTrue(wait_until(lambda: self.robot.types().count("hello") >= 2, timeout=4))
        self.wait_linked()
        self.assertTrue(wait_until(lambda: "state" in self.robot.types_after_hello(2)))  # state resent

    def test_reboot_without_reenumeration_resends_state(self):
        self.wait_linked()
        self.hook("UserPromptSubmit")
        self.assertTrue(wait_until(lambda: self.robot.n == 1))
        states = self.robot.types().count("state")
        self.robot.reboot()  # the next ping ack carries the new boot id and n=0
        self.assertTrue(wait_until(lambda: self.robot.types().count("state") > states and self.robot.n == 1))
        self.assertEqual(self.robot.last("state")["sessions"], WORK)
        self.assertEqual(self.agent.status()["boot"], 1002)
        self.assertEqual(self.backend.opened, 1)  # no reconnect, no new hello
        self.assertEqual(self.robot.types().count("hello"), 1)

    def test_device_wipe_resends_state(self):
        self.wait_linked()
        self.hook("UserPromptSubmit")
        self.assertTrue(wait_until(lambda: self.robot.n == 1))
        states = self.robot.types().count("state")
        time.sleep(3 * FAST.ping_interval)
        self.assertEqual(self.robot.types().count("state"), states)  # matching n: no extra states
        self.robot.wipe()
        self.assertTrue(wait_until(lambda: self.robot.types().count("state") > states and self.robot.n == 1))
        self.assertEqual(self.robot.last("state")["sessions"], WORK)
        self.assertEqual(self.agent.status()["boot"], 1001)

    def test_token_required(self):
        payload = {"hook_event_name": "SessionStart", "session_id": "x", "cwd": "/p/x"}
        self.assertEqual(self.request("POST", "/hook", payload, token=None)[0], 403)
        self.assertEqual(self.request("POST", "/hook", payload, token="wrong")[0], 403)
        self.assertEqual(self.request("POST", "/release", {}, token=None)[0], 403)
        self.assertEqual(self.request("POST", "/acquire", {}, token="wrong")[0], 403)
        self.assertEqual(self.request("POST", "/shutdown", {}, token="wrong")[0], 403)
        self.assertEqual(self.request("GET", "/status", token=None)[0], 403)
        self.assertEqual(self.agent.status()["sessions"], [])
        self.assertFalse(self.agent.stop_requested.is_set())
        status, body = self.request("GET", f"/status?token={TOKEN}", token=None)
        self.assertEqual((status, body["agent"]), (200, "0.1.0"))
        self.assertEqual(self.request("GET", "/nothing")[0], 404)

    def test_shutdown_request(self):
        self.assertEqual(self.request("POST", "/shutdown", {}), (200, {"ok": True}))
        self.assertTrue(self.agent.stop_requested.wait(2))

    def test_bad_hook_body(self):
        req = urllib.request.Request(self.base + "/hook", data=b"not json", method="POST")
        req.add_header("X-SC-Token", TOKEN)
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            NO_PROXY.open(req, timeout=5)
        self.assertEqual(ctx.exception.code, 400)
        ctx.exception.close()
        self.assertEqual(self.request("POST", "/hook", [1, 2])[0], 400)  # type: ignore[arg-type]

    def test_release_and_acquire(self):
        self.wait_linked()
        status, body = self.request("POST", "/release", {})
        self.assertEqual((status, body), (200, {"ok": True, "port": "COM99"}))
        self.assertTrue(self.robot.closed)
        self.assertEqual(self.agent.status()["link"], "released")
        opened = self.backend.opened
        time.sleep(0.3)
        self.assertEqual(self.backend.opened, opened)  # stays away from the port
        self.assertEqual(self.request("POST", "/acquire", {})[0], 200)
        self.wait_linked()
        self.assertEqual(self.robot.types().count("hello"), 2)

    def test_agent_command(self):
        self.wait_linked()
        self.assertIs(self.agent.status()["robot_ai"], False)
        self.assertEqual(self.request("POST", "/agent", {}, token="wrong")[0], 403)
        self.assertEqual(self.request("POST", "/agent", {}), (200, {"ok": True}))
        self.assertTrue(wait_until(lambda: self.agent.status()["robot_ai"] is True))
        self.assertEqual(self.robot.types().count("agent"), 1)

    def test_setting_command(self):
        self.wait_linked()
        self.assertEqual(self.request("POST", "/setting", {"key": "sleep", "value": 0}, token="wrong")[0], 403)
        self.assertEqual(self.request("POST", "/setting", {"key": "sleep", "value": False}), (200, {"ok": True}))
        self.assertTrue(wait_until(lambda: self.agent.status()["robot_settings"] == {"sleep": 0}))
        self.assertEqual(self.robot.settings["sleep"], 0)
        sent = [f for f in self.robot.frames if f["t"] == "set"]
        self.assertEqual((sent[0]["k"], sent[0]["v"]), ("sleep", 0))

        self.assertEqual(self.request("POST", "/setting", {"key": "face_track"})[0], 200)  # read only
        self.assertTrue(wait_until(lambda: self.agent.status()["robot_settings"].get("face_track") == 1))
        self.assertNotIn("v", [f for f in self.robot.frames if f["t"] == "set"][-1])

        self.assertEqual(self.request("POST", "/setting", {"key": "nope", "value": 1})[0], 200)
        self.assertTrue(wait_until(lambda: self.agent.status()["stats"]["nacks"] == 1))
        self.assertNotIn("nope", self.agent.status()["robot_settings"])

    def test_setting_command_rejects_bad_bodies(self):
        self.wait_linked()
        for body in ({}, {"key": ""}, {"key": "Sleep"}, {"key": "a" * 16}, {"key": "sleep", "value": "1"},
                     {"key": "sleep", "value": 1.5}, ["sleep"]):
            self.assertEqual(self.request("POST", "/setting", body)[0], 400, body)
        self.assertEqual(self.request("POST", "/setting", None)[0], 400)
        time.sleep(0.2)
        self.assertNotIn("set", self.robot.types())

    def test_head_command(self):
        self.wait_linked()
        self.assertEqual(self.request("POST", "/head", {"yaw": 20, "speed": 150}), (200, {"ok": True}))
        self.assertTrue(wait_until(lambda: "head" in self.robot.types()))
        sent = [f for f in self.robot.frames if f["t"] == "head"][0]
        self.assertEqual((sent["yaw"], sent["spd"], "pitch" in sent), (20, 150, False))
        for body in ({}, {"speed": 100}, {"yaw": "20"}, {"pitch": 1.5}, {"yaw": True}, [20]):
            self.assertEqual(self.request("POST", "/head", body)[0], 400, body)

    def test_agent_command_waits_for_link(self):
        self.wait_linked()
        self.request("POST", "/release", {})
        self.assertEqual(self.request("POST", "/agent", {})[0], 200)  # e.g. sent right after flashing
        time.sleep(0.2)
        self.assertNotIn("agent", self.robot.types())
        self.request("POST", "/acquire", {})
        self.wait_linked()
        self.assertTrue(wait_until(lambda: "agent" in self.robot.types_after_hello(2)))

    def test_release_without_robot(self):
        self.backend.present = False
        self.wait_linked()  # still linked: discovery only matters when the port is closed
        self.assertEqual(self.request("POST", "/release", {})[1], {"ok": True, "port": "COM99"})
        self.assertEqual(self.request("POST", "/acquire", {})[0], 200)
        self.assertTrue(wait_until(lambda: self.agent.status()["link"] == "searching"))
        self.assertEqual(self.request("POST", "/release", {})[1], {"ok": True, "port": None})

    def test_reconnect_after_device_loss_and_reboot(self):
        self.wait_linked()
        self.hook("UserPromptSubmit")
        self.robot.boot = 2001
        self.robot.fail_reads = True
        self.assertTrue(wait_until(lambda: self.backend.opened >= 2))
        self.wait_linked()
        self.assertEqual(self.agent.status()["boot"], 2001)
        self.assertTrue(wait_until(lambda: "state" in self.robot.types_after_hello(2)))
        self.assertEqual(self.robot.last("state")["sessions"], WORK)

    def test_known_port_is_held_while_silent(self):
        self.wait_linked()
        self.robot.silent = True  # e.g. flashed with firmware without the link
        hellos = 1 + (FAST.silent_hellos + 1) * HELLO_TRIES
        self.assertTrue(wait_until(lambda: self.robot.types().count("hello") >= hellos, timeout=8))
        self.assertFalse(self.robot.closed)
        self.assertEqual(self.backend.opened, 1)

    def test_serial_log(self):
        self.wait_linked()
        self.hook("SessionStart")
        self.assertTrue(wait_until(lambda: self.robot.types().count("state") >= 2))
        time.sleep(0.1)
        logs = list((Path(self.tmp.name) / "logs").glob("serial-*.log"))
        self.assertEqual(len(logs), 1)
        text = logs[0].read_text(encoding="utf-8")
        self.assertIn("] I (100) robot: got hello", text)
        self.assertIn("--- port COM99 opened", text)
        self.assertNotIn("@@sc", text)
        self.assertNotIn("\x1b", text)

    def test_clear_on_shutdown(self):
        self.wait_linked()
        self.agent.stop()
        self.assertEqual(self.robot.types()[-1], "clear")
        self.assertTrue(self.robot.closed)


class ActivityTest(AgentTestCase):
    def states(self) -> list[dict]:
        with self.robot.lock:
            return [f["sessions"] for f in self.robot.frames if f["t"] == "state"]

    def test_act_and_ctx_only_when_set(self):
        self.wait_linked()
        self.hook("SessionStart")
        idle = [{"slot": 0, "name": "StackChan", "st": "idle"}]
        self.assertTrue(wait_until(lambda: self.states()[-1] == idle))
        self.hook("StatusLine", ctx=57)
        self.assertTrue(wait_until(lambda: self.states()[-1] == [dict(idle[0], ctx=57)]))
        self.hook("UserPromptSubmit")
        self.hook("PreToolUse", tool_name="Edit", tool_input={"file_path": "x"})
        expected = [{"slot": 0, "name": "StackChan", "st": "work", "act": "edit", "ctx": 57}]
        self.assertTrue(wait_until(lambda: self.states()[-1] == expected), self.states()[-1])

    def test_test_result_event(self):
        self.wait_linked()
        self.hook("UserPromptSubmit")
        response = {"stdout": "== 12 passed in 0.40s ==", "stderr": "", "interrupted": False}
        self.hook("PostToolUse", tool_name="Bash", tool_input={"command": "pytest"}, tool_response=response)
        self.assertTrue(wait_until(lambda: self.robot.last("event") is not None))
        event = self.robot.last("event")
        self.assertEqual((event["ev"], event["text"]), ("pass", "StackChan: 12 passed"))
        self.assertNotIn("snd", event)


class DebounceTest(AgentTestCase):
    timing = replace(FAST, state_interval=0.4)

    def test_act_that_flips_back_sends_no_state(self):
        self.wait_linked()
        self.hook("UserPromptSubmit")
        self.hook("PreToolUse", tool_name="Edit")
        edit = [{"slot": 0, "name": "StackChan", "st": "work", "act": "edit"}]
        self.assertTrue(wait_until(lambda: (self.robot.last("state") or {}).get("sessions") == edit))
        time.sleep(0.5)
        states = self.robot.types().count("state")
        self.hook("PreToolUse", tool_name="Read")
        self.hook("PreToolUse", tool_name="Edit")  # back within the debounce: nothing to send
        time.sleep(0.8)
        self.assertEqual(self.robot.types().count("state"), states)
        self.hook("PreToolUse", tool_name="Read")
        self.assertTrue(wait_until(lambda: self.robot.types().count("state") == states + 1))
        self.assertEqual(self.robot.last("state")["sessions"][0]["act"], "read")


class LegacyFirmwareTest(AgentTestCase):
    def setUp(self) -> None:
        self.robots = (FakeRobot(legacy=True),)
        super().setUp()

    def test_acks_without_boot_and_n(self):
        self.wait_linked()
        self.hook("UserPromptSubmit")
        self.assertTrue(wait_until(lambda: (self.robot.last("state") or {}).get("sessions") == WORK))
        states = self.robot.types().count("state")
        self.assertTrue(wait_until(lambda: self.robot.types().count("ping") >= 3, timeout=3))
        self.assertEqual(self.robot.types().count("state"), states)


class SilentPortTest(AgentTestCase):
    def setUp(self) -> None:
        self.silent = FakeRobot(port="COM10", serial="SN-OTHER")
        self.silent.silent = True  # another ESP32-S3 board with the same VID:PID
        self.robots = (self.silent, FakeRobot(port="COM11", serial="SN-ROBOT"))
        super().setUp()
        self.real = self.backend.robots["COM11"]

    def test_single_silent_port_is_left_alone(self):
        self.backend.robots["COM11"].present = False
        self.assertTrue(wait_until(lambda: self.silent.opens == 1 and self.silent.closed, timeout=8))
        self.assertEqual(self.silent.types(), ["hello"] * (FAST.silent_hellos * HELLO_TRIES))
        self.assertEqual(self.agent.status()["link"], "searching")
        time.sleep(FAST.port_cooldown / 2)
        self.assertEqual(self.silent.opens, 1)  # cooling down
        self.assertTrue(wait_until(lambda: self.silent.opens == 2, timeout=3))  # tried again afterwards

    def test_rotates_to_the_answering_port_and_prefers_it(self):
        self.wait_linked(timeout=8)
        status = self.agent.status()
        self.assertEqual((status["port"], status["serial"]), ("COM11", "SN-ROBOT"))
        self.assertEqual(self.silent.opens, 1)
        self.assertEqual(len(self.silent.types()), FAST.silent_hellos * HELLO_TRIES)
        time.sleep(FAST.port_cooldown + 0.2)  # the silent port is no longer cooling down
        self.real.fail_reads = True  # robot lost: reconnect goes straight to its serial number
        self.assertTrue(wait_until(lambda: self.real.opens == 2, timeout=3))
        self.wait_linked()
        self.assertEqual(self.silent.opens, 1)


class PreferredSerialTest(AgentTestCase):
    config = Config(http_port=0, serial_number="SN-B")

    def setUp(self) -> None:
        self.robots = (FakeRobot(port="COM1", serial="SN-A"), FakeRobot(port="COM2", serial="SN-B"))
        super().setUp()

    def test_configured_serial_number_first(self):
        self.wait_linked()
        self.assertEqual(self.agent.status()["port"], "COM2")
        self.assertEqual(self.backend.robots["COM1"].opens, 0)


class FilesTest(unittest.TestCase):
    def setUp(self) -> None:
        logging.disable(logging.CRITICAL)
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()
        logging.disable(logging.NOTSET)

    def test_token_created_once(self):
        token = load_or_create_token(self.dir / "token")
        self.assertRegex(token, r"^[0-9a-f]{32}$")
        self.assertEqual(load_or_create_token(self.dir / "token"), token)

    def test_config_defaults_and_overrides(self):
        self.assertEqual(load_config(self.dir / "config.json"), Config())
        (self.dir / "config.json").write_text(json.dumps({
            "_comment": "x", "http_port": 47000, "usb_vid": "303a", "usb_pid": 4097, "serial_port": "COM7",
            "serial_number": "F4:12:FA:00:00:01", "quiet_work": "09:00-17:30", "release_timeout_s": 0,
        }), encoding="utf-8")
        config = load_config(self.dir / "config.json")
        self.assertEqual((config.http_port, config.usb_vid, config.usb_pid, config.serial_port),
                         (47000, 0x303A, 0x1001, "COM7"))
        self.assertEqual(config.serial_number, "F4:12:FA:00:00:01")
        self.assertEqual(config.release_timeout_s, 0)
        self.assertEqual(str(config.quiet.work.start), "09:00:00")

    def test_bad_config(self):
        for text in ('{"quiet_night": "late"}', '{"http_port": 0}', '{"http_port": 70000}', '{"http_port": "x"}'):
            with self.subTest(text=text):
                (self.dir / "config.json").write_text(text, encoding="utf-8")
                with self.assertRaises(ConfigError):
                    load_config(self.dir / "config.json")

    def test_merge_hooks_is_idempotent(self):
        snippet = json.loads((Path(__file__).with_name("hooks-settings.json")).read_text(encoding="utf-8"))
        own = {"type": "command", "command": "echo mine"}
        settings = {"model": "x", "hooks": {"Stop": [{"hooks": [own]}]}}
        changed = merge_hooks.merge(settings, snippet)
        self.assertEqual(sorted(changed), sorted(snippet["hooks"]))
        self.assertEqual(settings["hooks"]["Stop"][0]["hooks"], [own])
        self.assertEqual(settings["hooks"]["Stop"][1]["hooks"][0]["command"], "~/.claude/sc-hook.sh")
        self.assertEqual(merge_hooks.merge(settings, snippet), [])
        self.assertEqual(settings["model"], "x")

    def test_session_end_hook_is_synchronous(self):
        snippet = json.loads((Path(__file__).with_name("hooks-settings.json")).read_text(encoding="utf-8"))
        for event, entries in snippet["hooks"].items():
            hook = entries[0]["hooks"][0]
            with self.subTest(event=event):
                if event == "SessionEnd":  # async hooks still running at teardown are killed
                    self.assertEqual((hook.get("async"), hook["timeout"]), (None, 3))
                else:
                    self.assertTrue(hook["async"])

    def test_merge_hooks_upgrades_old_entries(self):
        snippet = json.loads((Path(__file__).with_name("hooks-settings.json")).read_text(encoding="utf-8"))
        old = {"type": "command", "command": "~/.claude/sc-hook.sh", "async": True}
        own = {"type": "command", "command": "echo mine"}
        settings = {"hooks": {"SessionEnd": [{"hooks": [old, own]}], "Stop": [{"hooks": [old]}]}}
        changed = merge_hooks.merge(settings, snippet)
        self.assertIn("SessionEnd", changed)
        self.assertEqual(settings["hooks"]["SessionEnd"], [{"hooks": [own]}] + snippet["hooks"]["SessionEnd"])
        self.assertEqual(settings["hooks"]["Stop"], snippet["hooks"]["Stop"])
        self.assertNotIn("Stop", changed)  # already current
        self.assertEqual(merge_hooks.merge(settings, snippet), [])


if __name__ == "__main__":
    unittest.main()

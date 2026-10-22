"""Tests for the status-line script (sc_statusline.py). Stdlib only; binds an ephemeral port on 127.0.0.1."""

import sys

sys.dont_write_bytecode = True

import json  # noqa: E402
import os  # noqa: E402
import subprocess  # noqa: E402
import tempfile  # noqa: E402
import threading  # noqa: E402
import time  # noqa: E402
import unittest  # noqa: E402
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer  # noqa: E402
from pathlib import Path  # noqa: E402

import sc_statusline as sl  # noqa: E402

SAMPLE = {
    "session_id": "abc123",
    "cwd": "/home/me/StackChan",
    "model": {"id": "claude-opus-5-5", "display_name": "Opus 5.5"},
    "workspace": {"current_dir": "/home/me/StackChan", "project_dir": "/home/me/StackChan"},
    "cost": {"total_cost_usd": 1.234, "total_duration_ms": 1000},
    "context_window": {"context_window_size": 200000, "used_percentage": 42.4, "remaining_percentage": 57.6},
}


class Recorder(BaseHTTPRequestHandler):
    requests: list = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        Recorder.requests.append((self.path, self.headers.get("X-SC-Token"), json.loads(body)))
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *args):
        pass


class ContextTest(unittest.TestCase):
    def test_used_percentage(self):
        self.assertEqual(sl.context_percent(SAMPLE), 42)

    def test_from_current_usage(self):
        window = {"context_window_size": 200000, "used_percentage": None, "current_usage": {
            "input_tokens": 1000, "output_tokens": 5000, "cache_creation_input_tokens": 500,
            "cache_read_input_tokens": 98500}}
        self.assertEqual(sl.context_percent({"context_window": window}), 50)

    def test_unknown(self):
        for data in ({}, {"context_window": None}, {"context_window": {"used_percentage": None, "current_usage": None}},
                     {"context_window": {"used_percentage": True}}):
            with self.subTest(data=data):
                self.assertIsNone(sl.context_percent(data))
        self.assertEqual(sl.context_percent({"context_window": {"used_percentage": 140}}), 100)


class TextTest(unittest.TestCase):
    def test_full_line(self):
        self.assertEqual(sl.status_text(SAMPLE, 42), "Opus 5.5 · ctx 42% · $1.23")

    def test_partial(self):
        self.assertEqual(sl.status_text({"model": {"display_name": "Sonnet"}, "cost": {"total_cost_usd": 0}}, None),
                         "Sonnet")
        self.assertEqual(sl.status_text({}, 7), "ctx 7%")
        self.assertEqual(sl.status_text({}, None), "")


class OptInTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.project = Path(self.tmp.name) / "proj"
        (self.project / ".claude").mkdir(parents=True)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_rules(self):
        plain = {"cwd": str(self.project), "workspace": {"project_dir": str(self.project)}}
        self.assertFalse(sl.opted_in(plain, {}))
        self.assertTrue(sl.opted_in({}, {"SC_LINK": "1"}))
        (self.project / ".claude" / "stackchan").write_text("", encoding="utf-8")
        self.assertTrue(sl.opted_in(plain, {}))
        self.assertTrue(sl.opted_in({"workspace": {"project_dir": str(self.project)}}, {}))
        self.assertTrue(sl.opted_in({"cwd": str(self.project)}, {}))
        self.assertTrue(sl.opted_in({}, {"CLAUDE_PROJECT_DIR": str(self.project)}))
        self.assertFalse(sl.opted_in(plain, {"SC_LINK": "0"}))


class ReportTest(unittest.TestCase):
    def setUp(self) -> None:
        Recorder.requests = []
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Recorder)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.tmp = tempfile.TemporaryDirectory()
        token = Path(self.tmp.name) / "token"
        token.write_text("tok-for-test\r\n", encoding="utf-8")
        self.env = {"SC_LINK": "1", "SC_TOKEN_FILE": str(token), "SC_PORT": str(self.server.server_address[1])}

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def test_sends_ctx(self):
        sl.report(SAMPLE, 42, self.env)
        self.assertEqual(Recorder.requests, [
            ("/hook", "tok-for-test", {"hook_event_name": "StatusLine", "session_id": "abc123", "ctx": 42})])

    def test_nothing_without_opt_in_or_ctx(self):
        sl.report(SAMPLE, 42, dict(self.env, SC_LINK="0"))
        sl.report(SAMPLE, None, self.env)
        sl.report({"session_id": ""}, 10, self.env)
        self.assertEqual(Recorder.requests, [])

    def test_errors_are_ignored(self):
        started = time.monotonic()
        sl.report(SAMPLE, 42, dict(self.env, SC_PORT="1"))  # nothing listens there
        sl.report(SAMPLE, 42, dict(self.env, SC_TOKEN_FILE=str(Path(self.tmp.name) / "missing")))
        self.assertLess(time.monotonic() - started, 2.0)

    def test_main_prints_only_the_line(self):
        script = Path(__file__).with_name("sc_statusline.py")
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", **self.env)
        for stdin, expected in ((json.dumps(SAMPLE), "Opus 5.5 · ctx 42% · $1.23\n"), ("not json", "\n")):
            with self.subTest(stdin=stdin):
                proc = subprocess.run([sys.executable, str(script)], input=stdin.encode(), capture_output=True,
                                      env=env, timeout=10)
                self.assertEqual((proc.returncode, proc.stdout.decode("utf-8"), proc.stderr), (0, expected, b""))
        self.assertEqual([r[2]["ctx"] for r in Recorder.requests], [42])


if __name__ == "__main__":
    unittest.main()

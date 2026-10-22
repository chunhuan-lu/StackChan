"""Tests for the shell scripts and the settings helper (sc-hook.sh, sc-run.sh, hooks-settings.json, merge_hooks.py).

Stdlib only; the scripts are inspected, not run (run them by hand, see README).
"""

import sys

sys.dont_write_bytecode = True

import json  # noqa: E402
import os  # noqa: E402
import re  # noqa: E402
import subprocess  # noqa: E402
import tempfile  # noqa: E402
import unittest  # noqa: E402
from pathlib import Path  # noqa: E402

import merge_hooks  # noqa: E402

HERE = Path(__file__).parent
BLOCK = re.compile(r"# --- sc-agent connection:.*?# --- end of the shared block ---\n", re.S)


class ShellScriptTest(unittest.TestCase):
    def test_shared_connection_block_is_identical(self):
        blocks = [BLOCK.findall((HERE / name).read_text(encoding="utf-8")) for name in ("sc-hook.sh", "sc-run.sh")]
        self.assertEqual([len(b) for b in blocks], [1, 1])
        self.assertEqual(blocks[0][0], blocks[1][0])
        self.assertIn("WSLInterop", blocks[0][0])

    def test_bash_scripts_use_lf(self):
        for name in ("sc-hook.sh", "sc-run.sh"):
            data = (HERE / name).read_bytes()
            with self.subTest(name=name):
                self.assertTrue(data.startswith(b"#!/usr/bin/env bash\n"))
                self.assertNotIn(b"\r", data)


class HookSettingsTest(unittest.TestCase):
    def test_events(self):
        hooks = json.loads((HERE / "hooks-settings.json").read_text(encoding="utf-8"))["hooks"]
        for event in ("SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PostToolUseFailure",
                      "Notification", "SubagentStart", "SubagentStop", "Stop", "StopFailure", "PreCompact"):
            with self.subTest(event=event):
                self.assertEqual(hooks[event], [{"hooks": [
                    {"type": "command", "command": "~/.claude/sc-hook.sh", "async": True}]}])
        self.assertEqual(hooks["SessionEnd"][0]["hooks"][0]["timeout"], 3)


class StatusLineSettingTest(unittest.TestCase):
    def test_added_only_when_missing(self):
        settings = {"model": "x"}
        self.assertTrue(merge_hooks.add_statusline(settings, "python3 ~/.claude/sc_statusline.py"))
        self.assertEqual(settings["statusLine"], {"type": "command", "command": "python3 ~/.claude/sc_statusline.py"})
        own = {"statusLine": {"type": "command", "command": "~/my-line.sh"}}
        self.assertFalse(merge_hooks.add_statusline(own, "python3 ~/.claude/sc_statusline.py"))
        self.assertEqual(own, {"statusLine": {"type": "command", "command": "~/my-line.sh"}})

    def test_command(self):
        self.assertEqual(merge_hooks.statusline_command("python3"), "python3 ~/.claude/sc_statusline.py")
        command = merge_hooks.statusline_command()
        if sys.platform == "win32":  # the interpreter that runs merge_hooks.py, quoted, forward slashes
            self.assertEqual(command, '"{}" ~/.claude/sc_statusline.py'.format(Path(sys.executable).as_posix()))
        else:
            self.assertEqual(command, "python3 ~/.claude/sc_statusline.py")

    def test_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "settings.json"
            target.write_text(json.dumps({"model": "x"}), encoding="utf-8")
            script = str(HERE / "merge_hooks.py")
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
            first = subprocess.run([sys.executable, script, "--statusline", str(target)], capture_output=True,
                                   text=True, env=env, timeout=30)
            self.assertEqual(first.returncode, 0, first.stderr)
            settings = json.loads(target.read_text(encoding="utf-8"))
            self.assertIn("PreToolUse", settings["hooks"])
            self.assertTrue(settings["statusLine"]["command"].endswith("~/.claude/sc_statusline.py"))
            self.assertTrue((Path(tmp) / "settings.json.bak").exists())
            again = subprocess.run([sys.executable, script, "--statusline", str(target)], capture_output=True,
                                   text=True, env=env, timeout=30)
            self.assertIn("already configured", again.stdout)
            self.assertEqual(json.loads(target.read_text(encoding="utf-8")), settings)


if __name__ == "__main__":
    unittest.main()

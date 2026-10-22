"""Merge hooks-settings.json into a Claude Code settings.json (idempotent, keeps a .bak copy).

    python3 merge_hooks.py [--statusline]                       WSL: ~/.claude/settings.json
    .venv\\Scripts\\python.exe merge_hooks.py [--statusline]      Windows: %USERPROFILE%\\.claude\\settings.json
    python3 merge_hooks.py [--statusline] path/to/settings.json  any other file

Existing sc-hook.sh entries are replaced by the current ones from hooks-settings.json (so an older install is
upgraded); all other settings and hooks in the file are kept. With --statusline it also registers
~/.claude/sc_statusline.py as the status line, but only when the file has no statusLine yet.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Any

HOOK_SCRIPT = "sc-hook.sh"
STATUSLINE_SCRIPT = "~/.claude/sc_statusline.py"


def _is_ours(hook: Any) -> bool:
    return isinstance(hook, dict) and HOOK_SCRIPT in str(hook.get("command", ""))


def _without_ours(group: Any) -> Any | None:
    """A matcher group without our hook entries; None when nothing else was in it."""
    if not isinstance(group, dict) or not isinstance(group.get("hooks"), list):
        return group
    kept = [hook for hook in group["hooks"] if not _is_ours(hook)]
    if len(kept) == len(group["hooks"]):
        return group
    return {**group, "hooks": kept} if kept else None


def merge(settings: dict[str, Any], snippet: dict[str, Any]) -> list[str]:
    """Put the snippet's hook entries into settings, replacing older sc-hook.sh entries; returns changed events."""
    hooks = settings.setdefault("hooks", {})
    changed = []
    for event, entries in snippet["hooks"].items():
        current = hooks.get(event, [])
        others = [group for group in (_without_ours(g) for g in current) if group is not None]
        updated = others + entries
        if updated != current:
            hooks[event] = updated
            changed.append(event)
    return changed


def statusline_command(python: str | None = None) -> str:
    """python3 on PATH in WSL/Linux; on Windows the interpreter running this script (Git Bash runs the command)."""
    if python is None:
        python = "python3" if sys.platform != "win32" else '"{}"'.format(Path(sys.executable).as_posix())
    return f"{python} {STATUSLINE_SCRIPT}"


def add_statusline(settings: dict[str, Any], command: str) -> bool:
    """Register the status line unless one is configured already; True when added."""
    if "statusLine" in settings:
        return False
    settings["statusLine"] = {"type": "command", "command": command}
    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Register sc-hook.sh (and optionally sc_statusline.py).")
    parser.add_argument("settings", nargs="?", help="settings file (default ~/.claude/settings.json)")
    parser.add_argument("--statusline", action="store_true", help="also register sc_statusline.py as statusLine")
    args = parser.parse_args(argv[1:])
    target = Path(args.settings).expanduser() if args.settings else Path.home() / ".claude" / "settings.json"
    snippet = json.loads(Path(__file__).with_name("hooks-settings.json").read_text(encoding="utf-8"))
    try:
        settings = json.loads(target.read_text(encoding="utf-8-sig")) if target.exists() else {}
    except ValueError as exc:
        print(f"{target} is not valid JSON ({exc}); nothing changed", file=sys.stderr)
        return 1
    changed = merge(settings, snippet)
    if changed:
        print(f"updated {HOOK_SCRIPT} hooks: {', '.join(changed)}")
    else:
        print(f"{HOOK_SCRIPT} hooks are up to date")
    added = False
    if args.statusline:
        added = add_statusline(settings, statusline_command())
        if added:
            print(f"statusLine: {settings['statusLine']['command']}")
        else:
            print(f"statusLine is already configured ({json.dumps(settings['statusLine'])}); left unchanged.")
            print(f"To use sc_statusline.py, set its command to: {statusline_command()}")
    if not changed and not added:
        return 0
    if target.exists():
        shutil.copy2(target, target.with_name(target.name + ".bak"))
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as file:  # also runs on WSL's older python3
        file.write(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

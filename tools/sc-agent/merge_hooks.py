"""Merge hooks-settings.json into a Claude Code settings.json (idempotent, keeps a .bak copy).

    python3 merge_hooks.py                           WSL: ~/.claude/settings.json
    .venv\\Scripts\\python.exe merge_hooks.py          Windows: %USERPROFILE%\\.claude\\settings.json
    python3 merge_hooks.py path/to/settings.json     any other file

Existing sc-hook.sh entries are replaced by the current ones from hooks-settings.json (so an older install is
upgraded); all other settings and hooks in the file are kept.
"""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path
from typing import Any

HOOK_SCRIPT = "sc-hook.sh"


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


def main(argv: list[str]) -> int:
    target = Path(argv[1]).expanduser() if len(argv) > 1 else Path.home() / ".claude" / "settings.json"
    snippet = json.loads(Path(__file__).with_name("hooks-settings.json").read_text(encoding="utf-8"))
    try:
        settings = json.loads(target.read_text(encoding="utf-8-sig")) if target.exists() else {}
    except ValueError as exc:
        print(f"{target} is not valid JSON ({exc}); nothing changed", file=sys.stderr)
        return 1
    changed = merge(settings, snippet)
    if not changed:
        print(f"{target} already has the current {HOOK_SCRIPT} hooks")
        return 0
    if target.exists():
        shutil.copy2(target, target.with_name(target.name + ".bak"))
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as file:  # also runs on WSL's older python3
        file.write(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")
    print(f"updated {HOOK_SCRIPT} hooks in {target}: {', '.join(changed)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

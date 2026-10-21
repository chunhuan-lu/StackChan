#!/usr/bin/env python3
"""sc_statusline.py - Claude Code status line that also reports the context usage to sc-agent (REQ-003).

Install as ~/.claude/sc_statusline.py and register it as statusLine (merge_hooks.py --statusline). Reads the
status-line JSON from stdin and prints one short line, e.g. "Opus 5.5 · ctx 42% · $1.23". When the session takes
part (SC_LINK=1, or a .claude/stackchan marker in CLAUDE_PROJECT_DIR, workspace.project_dir or cwd; SC_LINK=0 = off)
it also sends {"hook_event_name": "StatusLine", "session_id", "ctx"} to sc-agent on 127.0.0.1 (0.3 s timeout).
Prints nothing else and never fails visibly. Standard library only, Python 3.8 or newer.
Settings: SC_TOKEN_FILE (default ~/.sc-agent/token), SC_PORT (default 47123).
"""

from __future__ import annotations

import json
import os
import sys
import urllib.request
from typing import Any, Optional

REPORT_TIMEOUT_S = 0.3


def context_percent(data: dict) -> Optional[int]:
    """Context window usage 0-100 from context_window.used_percentage, else from current_usage / window size."""
    window = data.get("context_window")
    if not isinstance(window, dict):
        return None
    used = window.get("used_percentage")
    if _is_number(used):
        return _clamp(used)
    usage, size = window.get("current_usage"), window.get("context_window_size")
    if isinstance(usage, dict) and _is_number(size) and size > 0:
        keys = ("input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens")
        tokens = sum(usage[key] for key in keys if _is_number(usage.get(key)))
        return _clamp(100.0 * tokens / size)
    return None


def status_text(data: dict, ctx: Optional[int]) -> str:
    parts = []
    model = data.get("model")
    name = model.get("display_name") if isinstance(model, dict) else None
    if isinstance(name, str) and name:
        parts.append(name)
    if ctx is not None:
        parts.append("ctx {}%".format(ctx))
    cost = data.get("cost")
    usd = cost.get("total_cost_usd") if isinstance(cost, dict) else None
    if _is_number(usd) and usd > 0:
        parts.append("${:.2f}".format(usd))
    return " · ".join(parts)


def opted_in(data: dict, environ: Any = os.environ) -> bool:
    link = environ.get("SC_LINK", "")
    if link == "1":
        return True
    if link:
        return False
    workspace = data.get("workspace") if isinstance(data.get("workspace"), dict) else {}
    for folder in (environ.get("CLAUDE_PROJECT_DIR"), workspace.get("project_dir"), data.get("cwd"),
                   workspace.get("current_dir")):
        if isinstance(folder, str) and folder and os.path.exists(os.path.join(folder, ".claude", "stackchan")):
            return True
    return False


def report(data: dict, ctx: Optional[int], environ: Any = os.environ) -> None:
    """Send the context usage to sc-agent; any error is ignored."""
    session = data.get("session_id")
    if ctx is None or not isinstance(session, str) or not session or not opted_in(data, environ):
        return
    token_file = environ.get("SC_TOKEN_FILE") or os.path.join(os.path.expanduser("~"), ".sc-agent", "token")
    try:
        with open(token_file, encoding="utf-8-sig") as file:
            token = file.read().strip()
        if not token:
            return
        port = int(environ.get("SC_PORT") or 47123)
        body = json.dumps({"hook_event_name": "StatusLine", "session_id": session, "ctx": ctx}).encode("utf-8")
        request = urllib.request.Request(
            "http://127.0.0.1:{}/hook".format(port), data=body, method="POST",
            headers={"X-SC-Token": token, "Content-Type": "application/json"},
        )
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # never through a proxy
        opener.open(request, timeout=REPORT_TIMEOUT_S).close()
    except Exception:
        pass


def main() -> int:
    try:
        sys.stderr = open(os.devnull, "w")  # nothing but the status line may appear
        try:
            data = json.loads(sys.stdin.buffer.read().decode("utf-8", "replace") or "{}")
        except ValueError:
            data = {}
        if not isinstance(data, dict):
            data = {}
        ctx = context_percent(data)
        sys.stdout.buffer.write((status_text(data, ctx) + "\n").encode("utf-8"))
        sys.stdout.flush()
        report(data, ctx)
    except Exception:
        pass
    return 0


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _clamp(value: float) -> int:
    return max(0, min(100, int(round(value))))


if __name__ == "__main__":
    sys.exit(main())

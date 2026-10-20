# REQ-003 Claude Code status robot

**English** | [中文](REQ-003-claude-code-status.zh-CN.md)

Status: **design draft**, not implemented · Backlog: B-010

## Goal

The robot shows what the Claude Code sessions on the maintainer's PC are doing and calls the maintainer back when a
session needs input, without slowing Claude Code down and without depending on the network the robot is on.

## Decisions by the maintainer (2026-10-21)

- The robot travels with the maintainer between home and office; office Wi-Fi is unstable and often not the PC's LAN.
- One PC runs Claude Code, often several sessions at once, mostly inside **WSL**, sometimes natively on Windows.
- A background program may run permanently on the PC; Python is fine (no opaque .exe), started from a .bat / bash script.
- Only selected sessions take part: a marker file in the project **or** an environment variable at start.
- No sounds during working hours.
- The speech bubble may show the project and the event.
- The NAS has a static public IP and the router can forward ports (no domain).

## Architecture

```
Claude Code hooks (WSL or Windows)
   │  sc-hook: opt-in check, then curl.exe → 127.0.0.1 (runs on the Windows side, also from WSL)
   ▼
sc-agent (Windows, Python, starts at logon)
   ├─ session table + debounce, decides what the robot shows
   ├─ USB link (COM port, also writes the rolling serial log) ─────────▶ robot plugged into this PC
   └─ MQTT over TLS to Mosquitto on the NAS (static IP, own CA) ◀──── robot on any Wi-Fi (phase 2)

Voice (phase 3): robot → xiaozhi cloud → MCP endpoint ◀── outbound from sc-agent
```

### Hooks (Claude Code side)

- Configured once in the user settings of each environment (WSL `~/.claude/settings.json`, Windows
  `%USERPROFILE%\.claude\settings.json`) as `async: true` command hooks, so they never block a session.
- One small script `sc-hook` (bash for WSL and Git Bash): exits at once unless `SC_LINK=1` is set or the project has a
  marker file `.claude/stackchan`; otherwise pipes the hook JSON to `curl.exe` with a token header. In WSL, `curl.exe`
  runs on Windows through interop, so `127.0.0.1` reaches sc-agent without WSL networking changes.
- Events used: `SessionStart`, `UserPromptSubmit`, `PostToolUseFailure`, `Notification` (`permission_prompt`,
  `idle_prompt`, `agent_needs_input`, `elicitation_dialog`), `Stop`, `StopFailure`, `SubagentStart` / `SubagentStop`,
  `PreCompact` / `PostCompact`, `SessionEnd`. Optional status-line wrapper for context usage.

### sc-agent (PC side)

- Python 3 in its own venv (`pyserial`; `paho-mqtt` in phase 2); `start-sc-agent.bat` runs it with `pythonw`; optional
  Task Scheduler entry at logon. Listens on `127.0.0.1` only, token in a user file.
- Session table keyed by `session_id`: project (folder name, shortened to 12 characters), state, last event, context %.
  Sessions expire on `SessionEnd` or after 30 min without events.
- Sends the robot an idempotent full **state snapshot** (at most 5 per second, immediately on attention changes) plus
  short **event** messages for one-off animations.
- Transport: the USB link when the robot is plugged into this PC (found by VID:PID 303A:1001), otherwise MQTT
  (phase 2), otherwise nothing.
- Owns the COM port: writes every non-protocol line to a rolling log file (replaces `tools/serial_log.py`) and offers
  release / acquire for flashing through a wrapper script around `idf.py`.
- Quiet hours for sounds: never on weekdays 08:00–18:00 and never during the robot's night (23:00–08:00); configurable.

### Robot (firmware side)

- **USB link** (`SC_USB_LINK`): checksummed JSON lines `@@sc{...}*CRC32` on the native USB port next to the normal log;
  RX through the usb_serial_jtag driver, replies with one `write()` to `/dev/secondary`; task with a PSRAM stack;
  about 1 KB internal RAM. Stop-and-wait, the host retries.
- **Claude layer** in the idle director, as a file-scope singleton (the director is re-created around every
  conversation). While sessions exist it owns LEDs 1–5 / 7–11 (LED 0 / 6 stay with the xiaozhi status) and may hold
  the face; it is the only writer of those LEDs.
- Commands are an allow-list (state, event, ping); text is length-limited; head moves use the director's servo budget
  and the 5–85° pitch limits; nothing is ever routed to MCP, settings or OTA.
- During a conversation only the LEDs follow; faces and bubbles wait until the conversation ends. At night: light only,
  no head move, no sound.
- **Never** a way to approve a Claude Code permission request from the robot.

### Phase 2: relay

- Mosquitto 2 in Docker on the NAS, TLS listener on 8883 forwarded by the router, server certificate from our own CA;
  the robot pins the CA certificate (`cert_pem`) and sc-agent uses the same CA file. Per-client passwords and topic
  ACLs (robot: read `sc/<id>/cmd`, write `sc/<id>/state`; sc-agent the opposite). Only that port is exposed.
- The robot connects only in AI Agent idle (pause during conversations, retained offline state published before
  stopping), keepalive below 60 s. Extra internal RAM about 5–8 KB to be measured; fallback: esp-mqtt task stack in
  PSRAM via a patched component.

### Phase 3: voice status

- sc-agent connects to the xiaozhi MCP endpoint and offers a read-only `claude_status` tool ("3 sessions: StackChan
  working, docs waiting for you"). Only project names and states leave the PC, never prompts, output or code.

## Event mapping

| Claude Code | Robot | Bubble |
|---|---|---|
| prompt submitted / working | thinking face, session LED blue breathing | – |
| tool failed (`PostToolUseFailure`) | sweat 3 s, LED red flash | `<project>: failed` |
| needs input (`Notification`) | looks up at the user, LED amber blinking, optional soft sound outside quiet hours | `<project>: needs you` |
| finished (`Stop`) | nod + happy, LED green | `<project>: done` |
| API error (`StopFailure`) | sad, LED red | `<project>: error` |
| subagents running | LED ring spinner | – |
| context > 80 % / compaction | "full" face / yawn | `<project>: compacting` |

LED layout: up to 5 sessions, one LED per session, mirrored left (1–5) and right (7–11).

## Phases

| Phase | Content |
|---|---|
| P0 | REQ-004 fixes (standby, power, touch) |
| P1 | USB link + Claude layer (firmware), sc-agent with USB, hooks; items: call back, mood, multi-session LEDs, context |
| P2 | Relay on the NAS |
| P3 | Voice status through the xiaozhi MCP endpoint |

## Open points

- Exact working hours for the quiet rule (default weekdays 08:00–18:00).
- Whether the voice status (phase 3) may say project names, since it passes the xiaozhi cloud.
- Prototype checks: USB link frame loss under heavy logging; Windows port release during flashing; WSL interop latency
  for `curl.exe`; RAM of the MQTT client.

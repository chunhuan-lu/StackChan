# REQ-003 Claude Code status robot

**English** | [中文](REQ-003-claude-code-status.zh-CN.md)

Status: phase 1 (USB link, Claude layer, sc-agent, hooks) implemented on `feat/claude-link`, **not yet verified on
the device**; phases 2–3 are design only · Backlog: B-010

## Goal

The robot shows what the Claude Code sessions on the maintainer's PC are doing and calls the maintainer back when a
session needs input, without slowing Claude Code down and without depending on the network the robot is on.

## Decisions by the maintainer (2026-10-21)

- The robot travels with the maintainer between home and office; office Wi-Fi is unstable and often not the PC's LAN.
- One PC runs Claude Code, often several sessions at once, mostly inside **WSL**, sometimes natively on Windows.
- A background program may run permanently on the PC; Python is fine (no opaque .exe), started from a .bat / bash script.
- Only selected sessions take part: a marker file in the project **or** an environment variable at start.
- No sounds during working hours: weekdays 08:00–18:00 (confirmed), and never at the robot's night.
- The speech bubble may show the project and the event.
- The NAS has a static public IP and the router can forward ports (no domain).
- The voice status (phase 3) may say project names (confirmed).

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

## USB link protocol v1 (phase 1)

Transport: the robot's native USB-Serial/JTAG port (Windows: VID 303A, PID 1001), 115200 baud nominal. Open with
DTR and RTS released before `open()` and never toggle them (they reset the chip).

Framing, both directions: one line =

```
@@sc<compact JSON>*<CRC-32 of the JSON bytes, 8 uppercase hex digits>\n
```

- CRC-32 is the standard zlib CRC (`zlib.crc32(json_bytes)`; on the device `esp_rom_crc32_le(0, ...)`).
- JSON is UTF-8, one line, no `\n` inside. Host → device at most 768 bytes of JSON, device → host at most 256.
- Every other line is log output. A receiver searches for the **last** `@@sc` in a line and the last `*` after it;
  log text or ANSI colour codes may precede the frame. Lines from the device end with `\r\n`.
- Invalid frames (bad CRC, bad JSON, too long) are dropped silently.

Messages host → device all carry `"t"` (type) and `"id"` (uint32, strictly increasing within one agent run). The device
answers each with an ack; the host waits up to 500 ms and retries at most twice with the **same** id (the device
re-acknowledges a repeated id without applying it again). One message in flight at a time.

| t | Fields | Meaning |
|---|---|---|
| `hello` | `v` (1), `agent` (string) | Start of a link. Ack carries `v`, `fw`, `boot` (random per boot), `up` (s). A changed `boot` means the robot restarted: resend the state. Also resets the device's id de-duplication. |
| `ping` | – | Keepalive, every 2 s. |
| `state` | `sessions`: list of up to 5 `{slot 0-4, name ≤ 12 chars, st}` | Full snapshot, replaces everything. `st`: `idle`, `work`, `wait`, `done`, `err`. Empty list = Claude layer off. |
| `event` | `slot`, `ev`, `text` (≤ 40 chars), `snd` (bool, optional) | One-off reaction. `ev`: `needs`, `done`, `fail`, `err`, `compact`. `snd` asks for a short sound; the robot still refuses at night and during conversations. |
| `clear` | – | Same as an empty `state`. |

Ack (device → host): `{"t":"ack","id":N,"ok":true,"st":"idle|talk|other","boot":B,"n":K}`; on error
`"ok":false,"err":"..."`. `st` is the robot state: `idle` = AI Agent standby, `talk` = conversation, `other` = launcher
or another app. `boot` (random per robot boot) and `n` (used session slots on the robot) are in **every** ack: a
changed `boot` (a restart that kept the USB port open) or an `n` that differs from the agent's last state makes the
agent resend the state.

Staleness: if the device receives no valid frame for 30 s it drops the sessions (agent stopped or cable unplugged).

### Protocol v1.1 additions (backwards compatible, receivers ignore unknown fields)

- `state.sessions[i].act` (optional): what a working session is doing right now: `think` (prompt received, no tool
  yet), `edit` (Edit/Write/NotebookEdit), `run` (Bash and other tools), `read` (Read/Grep/Glob), `web`
  (WebSearch/WebFetch), `test` (a test or build command, or `sc-run`), `agents` (subagents running). Absent or empty
  when idle, waiting, done or in error.
- `state.sessions[i].ctx` (optional, 0–100): context window usage reported by the status-line script.
- `event.ev` value `pass`: a test or build command succeeded; `text` carries the result, e.g. `StackChan: 42 passed`.
  `fail` is now only sent for test and build commands (other failing commands are ignored); its text carries the
  counts when known, e.g. `StackChan: 3 failed`.

### Protocol v1.2 additions (REQ-006 firmware, backwards compatible)

- Host → device `agent` (no fields): start the AI Agent as a tap on its launcher icon would. The launcher handles it
  in its own update (the view must be closed under the LVGL lock before mooncake is torn down; requesting the xiaozhi
  start directly from the USB task crashed on the first try). Ignored when the AI Agent already runs; a request the
  launcher does not pick up within 30 s (another app open, setup page) is dropped. Older firmware answers
  `"ok":false,"err":"type"`.
- Every ack carries `ai` (bool): the AI Agent was started (requested or running).
- Used by `tools/sc-agent/sc-flash.ps1` through `POST /agent`.

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

## Phase 1 implementation notes (2026-10-21, decided while the maintainer was asleep)

Firmware (`feat/claude-link`):
- `main/hal/sc/usb_link.*`: task "sc_usb" on core 1, priority 2, 6 KB PSRAM stack; driver RX ring 1 KB; replies with one
  `write()` to `/dev/secondary`; started at the end of `Hal::init()` in every mode (the state is only shown in AI Agent
  standby). `SC_USB_LINK` (Kconfig default n since REQ-006; on in `sdkconfig.defaults.sc`).
- `main/stackchan/sc/claude_status.*`: singleton with the 5 slots, an event queue (4 entries, 30 s max age) and the
  30 s staleness rule.
- Idle director: while sessions exist they own LEDs 1–5 / 7–11 (slot i = LEDs 1+i and 7+i); short reaction effects
  (pick-up, petting) still win. Colours: idle dim white, work blue breathing, wait amber blinking, done green (dim after
  10 min), err red; a failed tool flashes the slot red for 3 s. Sleeping: 35 % brightness.
- Reactions: needs → doubt face, bubble 8 s, look up and settle, optional popup sound; done → happy, bubble, small nod;
  fail → sweat, bubble; err → sad, bubble; compact → yawn, bubble. Head moves use the director's servo budget (skipped,
  face and bubble kept, when no token is left). In the night window (23:00–08:00) only the LEDs react, awake or asleep.
  "needs" is never dropped: the sound is requested first (played from the main task, not under the LVGL lock), it wakes
  a sleeping or dozing robot (the wake-up is the gesture) and keeps it awake for the grace period. Other events while
  asleep: LEDs only. Busy (held, tilted, head touched, another reaction): bubble only.
- LED colours respect the PY32 driver's RGB565 steps (dim non-zero channels are lifted to the first step).
- Deviation: during a conversation the director does not exist, so the session LEDs are off; events wait (≤ 30 s) and
  are shown afterwards.
- Protocol v1.1 (2026-10-21, same day): `act` drives the faces of REQ-005 (work, think, curious for web), subagents
  colour the session LED cyan and pulse faster, `ctx` >= 80 % makes the "full" face likely among the occasional faces,
  `pass` shows the "done" face with the result. Claude reactions now use FaceArt faces instead of emotions.

PC side (`tools/sc-agent/`, see its README):
- `idle_prompt` notifications are ignored (Claude Code sends one about 60 s after every finished answer, which would
  repeat the call-back each time); `PostToolUse` turns "wait" back into "work" after a permission prompt was answered.
- Any mapped event re-creates an unknown session (survives an agent restart or the 30 min expiry); ended sessions are
  ignored for 60 s against late async hooks.
- Ping only after 2 s without traffic; a missing ack after retries counts as a lost link (hello, then the state again).
- `/release` auto-reacquires after 30 min (`release_timeout_s`, 0 = wait for `/acquire`).
- `merge_hooks.py` merges the hook snippet into a settings.json (writes a `.bak`); `--statusline` installs
  `sc_statusline.py` when no status line is configured (prints model, ctx %, cost and reports ctx).
- v1.1: PreToolUse / SubagentStart / SubagentStop hooks set `act`; `results.py` recognises test and build commands and
  parses pytest, unittest, ctest, jest/vitest, cargo, go and idf.py results (`pass` / `fail` with counts); other failing
  commands no longer react; `sc-run <command>` reports any command run outside Claude Code.

## Open points

- Prototype checks: USB link frame loss under heavy logging; Windows port release during flashing; WSL interop latency
  for `curl.exe`; RAM of the MQTT client.

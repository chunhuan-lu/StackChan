# sc-agent: Claude Code status on StackChan

**English** | [中文](README.zh-CN.md)

sc-agent shows what the Claude Code sessions on this PC are doing on the StackChan robot plugged in over USB, and
calls you back when a session needs input. It implements the PC side of
[REQ-003](../../docs/requirements/REQ-003-claude-code-status.md), phase 1 (USB link).

```
Claude Code (WSL or Windows) ── async hook ── sc-hook.sh ── curl.exe ──▶ 127.0.0.1:47123
                                                                           sc-agent (Windows, Python)
                                                       USB link (COM port) ◀──┘   └──▶ serial log
                                                                  StackChan
```

The robot firmware must contain the USB link (`SC_USB_LINK`, see REQ-003). A port that does not answer `hello` is
closed again after five tries and left alone for 60 s (see "Finding the robot"), unless it is known as the robot.

## Files

| File | Purpose |
|---|---|
| `sc_agent.py` | The agent: HTTP endpoint for hooks, session table, USB link, serial log, port release for flashing |
| `sessions.py` | Pure logic: hook events to robot sessions/events, slots, expiry, quiet hours |
| `sc_link.py` | Pure functions: frame encoding/parsing of the USB link protocol v1 |
| `sc-hook.sh` | Claude Code hook (bash, WSL and Git Bash): opt-in check, sends the hook JSON to the agent |
| `hooks-settings.json` | Snippet for `~/.claude/settings.json` that registers the hook |
| `merge_hooks.py` | Merges `hooks-settings.json` into a settings file (idempotent, keeps a `.bak`) |
| `setup.bat` | Creates (or repairs) `.venv` and installs `requirements.txt` (pyserial) |
| `start-sc-agent.bat` | Starts the agent in the background (`pythonw`), or with `--console` in the foreground |
| `sc-idf.ps1` | Runs `idf.py` with the COM port released by the agent |
| `test_*.py` | Unit tests (stdlib only, no serial port needed) |

## Install

Requirements: Windows 10/11, Python 3.10 or newer (python.org installer or the `py` launcher), `curl.exe` (part of
Windows). `<repo>` stands for the checkout, e.g. `E:\workbench\M5\src\StackChan` (`/mnt/e/workbench/M5/src/StackChan`
in WSL).

1. Create the virtual environment once (again after a Python upgrade):
   ```bat
   <repo>\tools\sc-agent\setup.bat
   ```
   It uses `py -3` or `python` from PATH; set `SC_PYTHON` to the full path of `python.exe` to pick another one.
   An existing `.venv` that no longer starts or was made with another Python version is rebuilt (stop the agent
   first, it runs from that `.venv`).
2. Start the agent: `start-sc-agent.bat` (background) or `start-sc-agent.bat --console` (log in the window, Ctrl+C
   stops). The first start creates the token `%USERPROFILE%\.sc-agent\token`.
3. Install the hook for **WSL** (inside WSL; replace `<you>` with the Windows user name):
   ```bash
   mkdir -p ~/.sc-agent ~/.claude
   cp /mnt/c/Users/<you>/.sc-agent/token ~/.sc-agent/token && chmod 600 ~/.sc-agent/token
   cp <repo>/tools/sc-agent/sc-hook.sh ~/.claude/sc-hook.sh && chmod +x ~/.claude/sc-hook.sh
   ```
   Register the hook in `~/.claude/settings.json` (adds the entries of `hooks-settings.json`, replaces older
   `sc-hook.sh` entries, keeps your other settings and hooks, writes `settings.json.bak`; running it again changes
   nothing, so it also upgrades an older install):
   ```bash
   python3 <repo>/tools/sc-agent/merge_hooks.py
   ```
4. Install the hook for **Windows** (Claude Code runs hooks with Git Bash there; Git Bash's `~` is
   `%USERPROFILE%`, so the token needs no copy):
   ```powershell
   Copy-Item <repo>\tools\sc-agent\sc-hook.sh $env:USERPROFILE\.claude\sc-hook.sh
   <repo>\tools\sc-agent\.venv\Scripts\python.exe <repo>\tools\sc-agent\merge_hooks.py
   ```
   The second line registers the hook in `%USERPROFILE%\.claude\settings.json`. Instead of `merge_hooks.py` you can
   also copy the entries of `hooks-settings.json` by hand.

   Running Claude Code sessions pick up new hooks only after a restart (or after reviewing them in `/hooks`).

The hook must keep LF line endings; the `.gitattributes` in this directory ensures that for checkouts. The hook
prints nothing, always exits 0 and gives up after 2 s. All hooks are registered as `async`, so they never slow a
session down, except `SessionEnd`: it runs synchronously with a 3 s timeout, because async hooks that are still
running when Claude Code exits are killed, and the robot would keep showing the ended session until it expires.

## Opt-in per session

Only selected sessions talk to the robot:

- marker file: `mkdir -p .claude && touch .claude/stackchan` in the project root (`$CLAUDE_PROJECT_DIR`, or the
  current directory when that is unset). Add it to `.gitignore` if you do not want to share it.
- or start Claude Code with `SC_LINK=1 claude`. `SC_LINK=0` switches the robot off even with a marker file.

## What the robot shows

| Claude Code hook | Session state | Robot event / bubble |
|---|---|---|
| `SessionStart` | `idle` (new session; an existing one only from `done`/`err` or after 5 s without events) | – |
| `UserPromptSubmit` | `work` | – |
| `PostToolUse` | `wait` → `work` (a permission prompt was answered); no new session; subagent tools ignored | – |
| `PostToolUseFailure` | `work` (a waiting session stays `wait`) | `fail`, `<project>: failed` |
| `Notification` (`permission_prompt`, `agent_needs_input`, `elicitation_dialog`, `elicitation_url_dialog`) | `wait` | `needs`, `<project>: needs you`, optional sound |
| `Notification` (`idle_prompt`) | `work`/`wait` → `idle`, others unchanged; no new session | – |
| `Stop` | `done` | `done`, `<project>: done` |
| `StopFailure` | `err` | `err`, `<project>: error` |
| `PreCompact` | unchanged | `compact`, `<project>: compacting` |
| `SessionEnd` | removed | – |

- `idle_prompt` (sent about 60 s after Claude Code started waiting for a prompt) is no call-back, that would repeat
  "needs you" after every answer. It only corrects a session left in `work`/`wait`, e.g. after you interrupted
  Claude (then no `Stop` arrives). `PostToolUse` runs for every tool call (asynchronously, it never blocks Claude);
  tool calls of subagents (`agent_id` set) do not end the main session's `wait`.
- Async hooks can arrive out of order, so a `SessionStart` right after other events of a working session does not
  reset it; `SessionStart` after a compaction (`source: compact`) never does.
- `<project>` is the project folder name, at most 12 characters. Up to 5 sessions (slots 0–4); a sixth concurrent
  session is ignored until a slot is free. Sessions without events for 30 minutes are dropped.
- Sound only for "needs you", and never on weekdays 08:00–18:00 or at night 23:00–08:00 (configurable). The robot
  itself also stays quiet at night and during conversations.
- The hook passes the hook JSON (which may contain your prompt) to the agent over the loopback interface only. The
  agent never logs or forwards it; the robot receives only slot, project name, state and the short bubble text.
- The robot can never approve a Claude Code permission request: the protocol has no such message.

## Configuration

Optional `%USERPROFILE%\.sc-agent\config.json` (restart the agent after changes; keys starting with `_` are
comments). All keys with their defaults:

```json
{
  "http_port": 47123,
  "usb_vid": "303A",
  "usb_pid": "1001",
  "serial_port": null,
  "serial_number": null,
  "quiet_days": ["mon", "tue", "wed", "thu", "fri"],
  "quiet_work": "08:00-18:00",
  "quiet_night": "23:00-08:00",
  "serial_log_days": 30,
  "release_timeout_s": 1800
}
```

- `http_port`: 1–65535.
- `serial_port`: fixed port such as `"COM12"` instead of the search by VID:PID. It is held even when the robot does
  not answer.
- `serial_number`: USB serial number of the robot (shown as `serial` in `/status`, the chip's MAC address). That
  port is preferred and held even when it does not answer (useful for firmware without the link, to keep the
  serial log). Without it the agent remembers the serial number of the last port that answered.
- `quiet_work` applies on `quiet_days`, `quiet_night` on every day; `null` or `""` switches a rule off.
- `serial_log_days`: serial logs older than this are deleted; `0` keeps them.
- `release_timeout_s`: a port released for flashing is taken back after this long even without `/acquire` (for a
  killed flash script); `0` waits for `/acquire`.
- With another `http_port`, set `SC_PORT` for the hooks too, e.g. `"env": {"SC_PORT": "47124"}` in the Claude Code
  settings, and for `sc-idf.ps1`.

## Autostart at logon (Task Scheduler)

In PowerShell (adjust `$dir`):

```powershell
$dir = "E:\workbench\M5\src\StackChan\tools\sc-agent"
$action = New-ScheduledTaskAction -Execute "$dir\.venv\Scripts\pythonw.exe" -Argument "`"$dir\sc_agent.py`"" -WorkingDirectory $dir
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName "sc-agent" -Action $action -Trigger $trigger -Settings $settings -Description "StackChan Claude Code status agent"
```

`-ExecutionTimeLimit ([TimeSpan]::Zero)` removes the default 72-hour limit. If registration is denied, run the
commands in an elevated PowerShell. Remove it with `Unregister-ScheduledTask sc-agent`.

Stop the agent cleanly (it closes the port and sends `clear`, so the robot drops the sessions at once):

```powershell
curl.exe -s --noproxy "*" -X POST -H "X-SC-Token: $(Get-Content $env:USERPROFILE\.sc-agent\token)" http://127.0.0.1:47123/shutdown
```

`Stop-ScheduledTask sc-agent`, Task Manager or `Stop-Process` end the background (`pythonw`) agent without that
cleanup: the robot then keeps the sessions until its 30 s staleness timeout. In console mode Ctrl+C also stops
cleanly. Start it again with `Start-ScheduledTask sc-agent` or `start-sc-agent.bat`. Last resort:

```powershell
Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" | Where-Object CommandLine -like '*sc_agent.py*' | ForEach-Object { Stop-Process -Id $_.ProcessId }
```

Only one agent can run: a second one cannot bind the HTTP port, logs that and exits.

## Finding the robot

The agent looks for ports with VID:PID 303A:1001, which every ESP32-S3 with native USB has. It tries the robot's
serial number first (`serial_number`, or the last port that answered `hello`), then ports that answered in this
run, then the others by name. A port that never answered in this run is closed after five unanswered `hello`
(about 20 s) and skipped for 60 s, so another ESP32-S3 board stays usable; `agent.log` says so, with a warning when
it is the only matching port. If the robot's own port is busy (e.g. `idf.py monitor`), the agent waits for it
instead of taking another board. Known robot ports, `serial_port` and `serial_number` are never skipped.

## Status

```powershell
curl.exe -s --noproxy "*" -H "X-SC-Token: $(Get-Content $env:USERPROFILE\.sc-agent\token)" http://127.0.0.1:47123/status
```

`link` is `searching` (no usable port), `connecting` (port open, no answer to `hello`), `linked` or `released`.
The answer also contains the port and its USB serial number, the robot state from the last ack (`idle`, `talk`,
`other`), its boot id and firmware, the sessions and link counters (`retries`, `timeouts`, `bad_frames` help to
judge frame loss).

## Flashing with sc-idf.ps1

The agent holds the COM port, so `idf.py flash` alone fails with "port busy". In an ESP-IDF PowerShell, from
`<repo>\firmware`:

```powershell
..\tools\sc-agent\sc-idf.ps1 flash               # port reported by the agent
..\tools\sc-agent\sc-idf.ps1 -p COM12 flash monitor
```

For targets that use the serial port (`flash`, `app-flash`, `bootloader-flash`, `partition-table-flash`,
`encrypted-flash`, `encrypted-app-flash`, `monitor`, `erase-flash`, `erase-otadata`, `read-otadata`, `efuse-*`) the
script releases the port (`POST /release`), runs `idf.py -p <port> <arguments>` and hands the port back
(`POST /acquire`) even when `idf.py` fails or is stopped with Ctrl+C. Other targets (`build`, `menuconfig`, ...) run
`idf.py` directly. Without a running agent and without `-p`, `idf.py` runs without `-p` and finds the port itself.
If PowerShell refuses to run scripts, use `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` once, or
`powershell -ExecutionPolicy Bypass -File ..\tools\sc-agent\sc-idf.ps1 flash`. Project rule: `flash` only, never
`erase-flash`.

## Logs

- `%USERPROFILE%\.sc-agent\agent.log`: agent log (1 MB, 3 old files kept). Never contains the token or prompts.
- `%USERPROFILE%\.sc-agent\logs\serial-YYYYMMDD.log`: everything the robot prints (protocol lines removed, ANSI
  colours stripped), one line each with local time. This replaces `serial_log.py` for daily use; only one program
  can hold the COM port at a time.

## Troubleshooting

- **Robot shows nothing**: check `/status` and `agent.log`.
  - `searching`: robot not plugged in, another VID:PID (set `serial_port`), or the only matching port did not
    answer `hello` and is skipped for 60 s (see "Finding the robot").
  - `cannot open COMx: ... Access is denied`: another program holds the port (`idf.py monitor`, `serial_log.py`,
    a serial terminal). Close it, or flash with `sc-idf.ps1`. The agent retries every second.
  - `connecting` with "robot does not answer hello": firmware without the USB link.
- **Robot stays dark after flashing without `sc-idf.ps1`'s `/acquire`** (script window closed): the agent takes the
  port back by itself `release_timeout_s` (30 min) after the `/release`, counted from the release, not from the
  end of flashing. `POST /acquire` (same `curl.exe` line as for `/shutdown`) or an agent restart takes it back at
  once.
- **Hook does nothing**: check the opt-in (marker file or `SC_LINK=1`). `agent.log` shows
  `rejected POST /hook: missing or wrong token` when the token differs: copy it again. Manual test from WSL:
  ```bash
  curl.exe -s --noproxy '*' -H "X-SC-Token: $(cat ~/.sc-agent/token)" \
    --data-binary '{"hook_event_name":"SessionStart","session_id":"test","cwd":"/tmp/demo"}' http://127.0.0.1:47123/hook
  ```
  Expected `{"ok": true}`; clean up with the same command and `"hook_event_name":"SessionEnd"`.
- **No `curl.exe` in WSL**: `command -v curl.exe` finds nothing when `/etc/wsl.conf` has
  `[interop] appendWindowsPath=false`. The hook then tries `/mnt/c/Windows/System32/curl.exe` directly.
- **Interop disabled** (`[interop] enabled=false`): WSL cannot start Windows programs. Enable it again and run
  `wsl --shutdown`, or switch WSL to mirrored networking (`networkingMode=mirrored` under `[wsl2]` in
  `%USERPROFILE%\.wslconfig`): then the Linux `curl` reaches 127.0.0.1 on Windows and the hook uses it.
- **`curl.exe: cannot execute binary file: Exec format error` in WSL**: interop is lost, typically with
  `systemd=true` in `/etc/wsl.conf` on Ubuntu 20.04 (the `WSLInterop` entry in `/proc/sys/fs/binfmt_misc/` is
  missing). The hook notices the missing entry and uses the Linux `curl`, which works with mirrored networking. To
  repair interop itself: `echo ':WSLInterop:M::MZ::/init:PF' | sudo tee /etc/binfmt.d/WSLInterop.conf`, then
  `sudo systemctl restart systemd-binfmt` (or `wsl --shutdown` from Windows).
- **HTTP port taken**: change `http_port` (see Configuration).

## Protocol and tests

The USB link protocol v1 is specified in REQ-003: frames `@@sc<json>*<CRC32>\n`, stop-and-wait with 500 ms ack
timeout and 2 retries with the same id, `hello` on connect (state resent after every hello), `ping` when nothing was
sent for 2 s, state snapshots at most 5 per second, events right after the pending state, `clear` on a clean
shutdown. Every ack of the current firmware carries the robot's boot id and its number of used session slots
(`n`): a new boot id (restart without USB re-enumeration) or an `n` that does not match the last acknowledged state
(the robot dropped its sessions) makes the agent resend the state. Acks without these fields (older firmware) are
accepted.

```bat
python -m unittest discover -s <repo>\tools\sc-agent -v
```

The tests use only the standard library, emulate the robot and never open a serial port.

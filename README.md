# StackChan — AI Agent fork

**English** | [中文](README.zh-CN.md)

A personal fork of [m5stack/StackChan](https://github.com/m5stack/StackChan): the robot firmware, based on the
official **v1.6.0** release (xiaozhi-esp32 v2.2.4, ESP-IDF v5.5.4), reworked around the **AI Agent** (xiaozhi)
mode. It is not an official M5Stack release. The upstream README (hardware, app, server, credits) is kept as
[README.upstream.md](README.upstream.md); the app, remote and server folders are unchanged.

Fork features are behind `SC_*` Kconfig switches whose defaults keep the upstream behaviour; this fork turns them
on in [`firmware/sdkconfig.defaults.sc`](firmware/sdkconfig.defaults.sc).

## Read this before flashing

These changes affect compatibility with the official firmware, flashing, or other features.

| Change | What it affects | What to do |
|---|---|---|
| **Partition table** [`firmware/partitions_sc.csv`](firmware/partitions_sc.csv): `assets` grows from 4 MB to 6080 KB (0xA00000-0xFF0000) for the MultiNet7 wake word model; `coredump` moves from 0xE00000 to the last 64 KB (0xFF0000). `nvs`, `otadata`, `phy_init`, `ota_0`, `ota_1` keep their offsets and sizes. | NVS (Wi-Fi, binding, settings) and both app slots stay where they are. An app-only update (OTA, or writing only the app image) does not change the partition table or the assets: the fork's app on the official layout has no MultiNet7 model (no wake word); the official app on the fork's layout looks for the official wake word model and does not find it. | Flash with `idf.py -p COMx flash` (bootloader, partition table, app and assets; NVS is kept). To go back, flash the official release completely (see [Back to the official firmware](#back-to-the-official-firmware)). |
| **App partitions about 95 % full** (0x4F0000 = 4.94 MB each; the build with face tracking is about 4.7 MB). | Face tracking (off by default) accounts for about 400 KB of it; there is little room for more. | New features need a partition decision first (e.g. one app slot instead of two). |
| **Wake word**: Chinese "起来干活" (*qi lai gan huo*) via MultiNet7 replaces "Hi, Stack Chan" (WakeNet); threshold 20. | Uses about 75 % of core 0 by itself. Other wake words need the sdkconfig changed. | `CONFIG_CUSTOM_WAKE_WORD*` in `sdkconfig.defaults.sc` ([REQ-001](docs/requirements/REQ-001-custom-wake-word.md)). |
| **OTA**: automatic firmware updates and server-pushed firmware / assets are off (`SC_DISABLE_AUTO_OTA`, `SC_DISABLE_REMOTE_OTA`). | The xiaozhi server cannot replace this firmware or its assets. Settings > About still updates manually, which installs the official firmware and removes this fork. | Do not use the manual update unless you want to leave the fork. |
| **Offline first** (`SC_OFFLINE_FIRST`): the AI Agent reaches standby without a network; the version check and the server connection run in the background. | No reboot into App setup when Wi-Fi is missing; Wi-Fi is set up from the launcher's App setup (or [`tools/sc-wifi`](tools/sc-wifi)). No "activating" or version screen. For the first 20 s after boot (or until the network start is done) the wake word is not active yet. A conversation without a network shows "No network. Please connect to the internet first." | [REQ-009](docs/requirements/REQ-009-offline-first.md). |
| **Default timezone**: Melbourne (`AEST-10AEDT,M10.1.0,M4.1.0/3`). | The day / night schedule (sleep 23:00-08:00) follows it. | Choose yours in Settings > System, or change `CONFIG_SC_DEFAULT_TIMEZONE`. |
| **AI Agent standby** is run by the idle director instead of upstream's random head motion. | The robot sleeps at night and in the dark, a tap wakes it (instead of starting a chat while asleep), mood light on the LEDs, its own head movements. | Settings panel (gear next to Home); [REQ-002](docs/requirements/REQ-002-idle-behaviour.md). |
| **USB power**: no power-save sleep or auto power-off while USB power is present. | The screen stays on while plugged in. | Setting "No standby on USB"; [REQ-004](docs/requirements/REQ-004-standby-power-touch.md). |
| **USB-Serial/JTAG** also carries the Claude Code link (framed JSON lines next to the log). | When `sc-agent` runs on the PC it holds the COM port: a plain `idf.py flash` reports the port as busy. | Flash through [`tools/sc-agent/sc-idf.ps1`](tools/sc-agent/README.md) or `sc-flash.ps1`. |
| **Microphone path**: the CoreS3 codec reads through a 1 s PSRAM buffer (`SC_MIC_BUFFER_MS`). | No audio is lost when other work slows the wake word down; it may answer up to about 0.3 s later. | [REQ-008](docs/requirements/REQ-008-face-tracking.md). |
| **Internal RAM is tight**: lowest about 6.6 KB (while the wake word model loads). | Code that allocates internal RAM can fail. | Allocate in PSRAM; measure with the logs in the dev log. |
| **Components**: esp-dl 3.2.4 and human_face_detect 0.4.0 added; esp-dl's TIE728 conv2d kernels are renamed at build time ([`firmware/CMakeLists.txt`](firmware/CMakeLists.txt)) so they do not collide with esp-sr's old copies. `dependencies.lock` moved esp_codec_dev 1.5.4 → 1.5.11, esp_sccb_intf 0.0.8 → 1.0.0, dl_fft 0.3.1 → 0.8.0, i2c_bus, uart-uhci and iot_usbh_cdc. | Audio codec and camera drivers are newer than upstream 1.6.0 (tested on the device). | Keep the lock file when building. |
| **xiaozhi changes** live in an extra patch, [`firmware/patches/xiaozhi-esp32-fork.patch`](firmware/patches/xiaozhi-esp32-fork.patch) (application, device state machine, backlight, MCP tools, assets build script), applied after upstream's patch by `fetch_repos.py`. | A newer xiaozhi needs the patch rebased. | `python firmware/tools/xiaozhi_fork_patch.py` regenerates it from `firmware/xiaozhi-esp32`. |
| **Settings** are stored in NVS namespace `sc_cfg` (31 settings) and offered to the voice assistant (`self.robot.get_settings`, `self.robot.set_setting`). | The settings' change mask is 32 bits: one setting left before it has to grow. | [REQ-006](docs/requirements/REQ-006-settings.md). |

## Features

| Feature | Docs |
|---|---|
| Custom wake word "起来干活" (MultiNet7) | [REQ-001](docs/requirements/REQ-001-custom-wake-word.md) |
| Idle behaviour director: day / night schedule, sleep in the dark, head touch, pick-up and tilt reactions, mood light | [REQ-002](docs/requirements/REQ-002-idle-behaviour.md) |
| Claude Code status on the robot over USB: session LEDs, faces, event reactions; PC agent and hooks | [REQ-003](docs/requirements/REQ-003-claude-code-status.md) |
| Standby, power and touch fixes | [REQ-004](docs/requirements/REQ-004-standby-power-touch.md) |
| Drawn faces (soft smile, special idle faces, Claude Code faces) | [REQ-005](docs/requirements/REQ-005-faces.md) |
| Settings panel (gear next to Home) and voice settings | [REQ-006](docs/requirements/REQ-006-settings.md) |
| Pixel buddy suit while Claude Code works, with transitions and head moves | [REQ-007](docs/requirements/REQ-007-claude-suit.md) |
| Face tracking: eyes and head follow you, greeting, search when lost (off by default) | [REQ-008](docs/requirements/REQ-008-face-tracking.md) |
| Offline first: the network only for a conversation | [REQ-009](docs/requirements/REQ-009-offline-first.md) |
| Full USB control: flash, restart, start the AI Agent, change settings, move the head | [REQ-003](docs/requirements/REQ-003-claude-code-status.md), [sc-agent](tools/sc-agent/README.md) |

## Build and flash

```bash
cd firmware
python fetch_repos.py            # upstream repos, then upstream's and this fork's xiaozhi patches
idf.py build                     # ESP-IDF v5.5.4
idf.py -p COMx flash             # keeps NVS: Wi-Fi, binding and settings survive
```

- `sdkconfig.defaults` (upstream) is applied first, then `sdkconfig.defaults.sc` (this fork), then
  `sdkconfig.defaults.local` if it exists (git-ignored, for your own overrides).
- Defaults only apply when `sdkconfig` is generated: after changing a defaults file or `main/Kconfig.projbuild`,
  run `idf.py reconfigure` (or delete `firmware/sdkconfig`).
- Face tracking: add `CONFIG_SC_FACE_PROBE=y` to `sdkconfig.defaults.local` (about 400 KB more; see the partition
  note above).
- Never commit `sdkconfig` or `sdkconfig.defaults.local`. Do not `erase-flash` without a full backup: NVS holds the
  Wi-Fi settings and the device binding.

## Kconfig switches

Menu "StackChan fork options" in `idf.py menuconfig`. "Upstream" is the Kconfig default, "fork" the value in
`sdkconfig.defaults.sc`.

| Switch | Upstream | Fork | What |
|---|---|---|---|
| `SC_OFFLINE_FIRST` | n | y | AI Agent without a network ([REQ-009](docs/requirements/REQ-009-offline-first.md)) |
| `SC_DISABLE_AUTO_OTA` | n | y | No automatic firmware update |
| `SC_DISABLE_REMOTE_OTA` | n | y | No server-pushed firmware / assets |
| `SC_DEFAULT_TIMEZONE` | GMT0 | Melbourne | Until one is chosen in Settings > System |
| `SC_IDLE_DIRECTOR` | n | y | Idle behaviour director ([REQ-002](docs/requirements/REQ-002-idle-behaviour.md)) |
| `SC_IDLE_FACES` | y | y | Drawn faces ([REQ-005](docs/requirements/REQ-005-faces.md)) |
| `SC_EXT_POWER_NO_STANDBY` | n | y | No standby on USB power ([REQ-004](docs/requirements/REQ-004-standby-power-touch.md)) |
| `SC_TOUCH_FIXES` | n | y | AI Agent touch fixes |
| `SC_USB_LINK` | n | y | Claude Code link over USB ([REQ-003](docs/requirements/REQ-003-claude-code-status.md)) |
| `SC_CLAUDE_SUIT` | y | y | Pixel buddy suit ([REQ-007](docs/requirements/REQ-007-claude-suit.md)) |
| `SC_SETTINGS_PANEL` | n | y | Settings panel and voice settings ([REQ-006](docs/requirements/REQ-006-settings.md)) |
| `SC_MIC_BUFFER_MS` | 0 | 1000 | Microphone buffer |
| `SC_FACE_PROBE` | n | n | Face tracking ([REQ-008](docs/requirements/REQ-008-face-tracking.md)) |

Schedule, thresholds, colours and speeds (`SC_IDLE_*`, `SC_CLAUDE_SUIT_*`) are defaults for the settings panel; the
list with ranges is in [REQ-006](docs/requirements/REQ-006-settings.md).

## Tools

- [`tools/sc-agent`](tools/sc-agent/README.md): PC agent (Windows, Python). Claude Code hooks → robot over USB
  (session lights, faces, the suit), the status line, `sc-flash.ps1` (flash, restart, start the AI Agent in one
  step), `sc-idf.ps1` (idf.py with the port released), and an HTTP API on 127.0.0.1 (`/status`, `/agent`,
  `/setting`, `/head`, ...).
- [`tools/sc-wifi`](tools/sc-wifi): add a Wi-Fi network over BLE (self-built firmware cannot pass the app's pairing
  handshake).

## Docs

- [Requirements](docs/requirements) REQ-001 to REQ-009, each in English and Chinese.
- [Backlog](docs/backlog.md) and the [dev log](docs/devlog) (what was tested on the device, and how).
- [Conventions](docs/CONVENTIONS.md): branches, commits, Kconfig, docs.

## Back to the official firmware

Flash an official release completely (for example with M5Burner), so that its partition table and assets are
written too; an app-only update keeps this fork's partition table and assets. NVS stays at the same place, so
Wi-Fi and the binding normally survive; the fork's settings (`sc_cfg`) are simply ignored. Make a full flash backup
first.

## License and credits

The firmware is MIT licensed (see [firmware/LICENSE](firmware/LICENSE), © M5Stack); the fork's changes are under the
same license. Thanks to M5Stack, the StackChan community and the xiaozhi-esp32 project; see
[README.upstream.md](README.upstream.md).

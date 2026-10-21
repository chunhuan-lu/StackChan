# Backlog

**English** | [中文](backlog.zh-CN.md)

Status: `todo` · `decide` (needs a decision) · `doing` · `done` · `dropped`. Last updated: 2026-10-21.

## Done

| ID | Item | Notes |
|---|---|---|
| B-000 | Toolchain, backup and repository setup | ESP-IDF v5.5.4; unmodified v1.5.1 and v1.6.0 both build; full 16 MB flash backup of the official 1.6.0 device; fork + git workflow. See [devlog 2026-10-20](devlog/2026-10-20.md). |

## Next

| ID | Item | Status | Notes |
|---|---|---|---|
| B-001 | Custom wake word "起来干活" (qi lai gan huo) | doing | Decided: MultiNet `mn7_cn` + assets partition enlarged to 6080 KB. Implemented on `feat/wake-word`; first on-device test works (wake-ups at prob 0.32 / 0.34, threshold 0.2). Hit-rate, negative and false-wake tests pending. See [REQ-001](requirements/REQ-001-custom-wake-word.md). |
| B-002 | Disable xiaozhi automatic and server-pushed OTA | doing | Fork patch, both on in `sdkconfig.defaults.sc` (Kconfig default n = upstream behaviour): `SC_DISABLE_AUTO_OTA` (a newer version is only logged) and `SC_DISABLE_REMOTE_OTA` (MCP tools `self.upgrade_firmware` / `self.assets.set_download_url` not registered, a pushed assets URL is ignored). Manual update from Settings > About still works. The automatic path is not exercised yet (no newer release). |
| B-003 | `fetch_repos.py`: multiple patches per repository | done | `extra_patches` in `repos.json`, idempotent re-runs, errors instead of silent skips; `tools/xiaozhi_fork_patch.py`. Tested on the host (already applied / dirty tree / clean tree). |
| B-004 | Wi-Fi throughput and latency | todo | Disable modem sleep during downloads and LAN control; move Wi-Fi/lwIP buffers to PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) and enlarge RX buffers / TCP window / UDP mailbox. |
| B-005 | Do not hold the LVGL lock while sleeping | todo | `_stackchan_update_task` holds the lock during `vTaskDelay(100)` while xiaozhi is busy (`hal.cpp`). |
| B-006 | Restore hotspot Wi-Fi provisioning | todo | 1.6 only allows App (BLE) setup, which self-built firmware cannot pass. Fallback today: BLE `setWifi` via nRF Connect. |
| B-007 | Audio scheduling review | todo | Only if choppy speech returns: `opus_codec` (prio 2) runs below the LVGL and stackchan tasks (prio 3). |
| B-023 | Claude Code pixel buddy suit | doing | [REQ-007](requirements/REQ-007-claude-suit.md) on `feat/claude-suit`: while Claude Code works the robot wears an orange pixel buddy costume (9 poses, sweat while working), put on with a drop-and-wriggle transition and taken off by unzipping itself with a long outlined arm; head nods, leans back and shakes along (settings `claude_suit`, `suit_off_min`, `suit_head`, `suit_head_spd`). Flashed; transitions to be watched on the device. |
| B-022 | Full USB control: flash, restart, start the AI Agent | doing | `tools/sc-agent/sc-flash.ps1`: download mode through esptool over USB-Serial/JTAG, flash, wait for the new boot id (watchdog reset if the robot hangs in the ROM loader), USB link command `agent` (protocol v1.2) so the launcher opens the AI Agent. Verified on the robot 2026-10-21 (AI Agent ready 15 s after the reset). See [REQ-003](requirements/REQ-003-claude-code-status.md). |
| B-021 | Settings panel and voice settings | doing | [REQ-006](requirements/REQ-006-settings.md) on `feat/settings`: gear next to the Home button opens a panel with 15 settings (activity, schedule, lux thresholds, sleep screen, light, face interval, faces / tap wake / Claude / USB standby switches), stored in NVS `sc_cfg`, applied live; MCP tools `self.robot.get_settings` / `set_setting`. Kconfig defaults now follow the upstream behaviour, the fork's choices are in `sdkconfig.defaults.sc`. Waiting for on-device verification. |
| B-020 | Expressive faces | doing | [REQ-005](requirements/REQ-005-faces.md): FaceArt draws 16 vector faces (soft smile as the base, occasional special faces every 4-10 min, Claude Code faces from session state and events). Waiting for on-device verification. |
| B-019 | Standby, power and touch fixes | doing | [REQ-004](requirements/REQ-004-standby-power-touch.md) on `fix/standby-power`: lit/dark sleep backlight (5 / 45) with fade, screen tap wakes the sleeping face, swipe filter, Home feedback, VBUS-based external power with power log (INA226 cell current), no power-save on USB, reset reason, core dumps. Waiting for on-device verification. |
| B-018 | Upstream bug: network test, then AI Agent → reboot into App setup | todo | Seen 2026-10-21 ([devlog](devlog/2026-10-21.md)). Settings > Network test starts and connects Wi-Fi through `WifiManager` and leaves it on. Opening AI Agent without a reboot: `WifiBoard::TryWifiConnect()` waits for a `Connected` event that already happened → 60 s timeout → `StartWifiConfigMode()` → `requestAppSetup()` → reboot into the APP SETUP page. Nothing is lost (binding, `is_configed`, Wi-Fi credentials); a reset recovers. Fix options: treat an already connected station as connected in `TryWifiConnect()` (fork patch), or stop the station when the test page closes. Workaround: reboot after a network test. Candidate for an upstream issue. Same timeout, second trigger (2026-10-21 16:06): a network whose DHCP takes minutes (associated after 8 s, no IP by 60 s; earlier the same AP needed 3 min for an IP) also ends in the App setup page; option: wait longer once associated. |

## Later

| ID | Item | Status | Notes |
|---|---|---|---|
| B-010 | Claude Code status robot | doing | Phase 1 (USB link, Claude layer, `tools/sc-agent`, hooks) implemented on `feat/claude-link`, waiting for on-device verification. [REQ-003](requirements/REQ-003-claude-code-status.md): Claude Code hooks → sc-agent (Python on Windows, also for WSL sessions) → USB link to the robot; phase 2 relay through Mosquitto on the NAS (static IP, own CA); phase 3 voice status through the xiaozhi MCP endpoint. Opt-in per project or session; never approves permissions. |
| B-011 | Idle behaviour, batch 1 | doing | Mood-driven director: schedule 08–23, sleep in the dark, touch/pick-up/tilt reactions, screen-only micro animations, mood light; silent and quiet. Implemented on `feat/idle-behavior`; first on-device test 2026-10-20, then tuned (touch quiet period, lux thresholds, Melbourne timezone). Remaining acceptance tests pending. See [REQ-002](requirements/REQ-002-idle-behaviour.md). |
| B-017 | Idle behaviour, batch 2 | todo | Knock detection (tap the table), head-bob to music (needs mic level from xiaozhi), head levelling on tilt (IMU axes calibrated: gravity +Y, pitch about X), persist the inner state. |
| B-012 | On-device face detection and tracking | doing | [REQ-008](requirements/REQ-008-face-tracking.md) on `feat/face-track`: ESP-DL `human_face_detect` 0.4 on core 1 while idle (esp-dl kernels renamed against esp-sr's copies); microphone buffer so detection no longer costs the wake word audio; eyes and head follow the face, greeting, wrong-way guard; settings `face_track`, `face_head`, `face_box`, `face_probe`, plus `sleep`; USB link `set` / `head` (protocol v1.3). Detection verified on the device, tracking flashed, to be watched with a face. |
| B-013 | Desk object recognition | todo | NAS: YOLO (OpenVINO) + reaction rules; small VLM only for new or unknown objects. |
| B-014 | Extra ways to start and end a conversation | todo | Head touch, tap/shake (IMU), voice "exit" tool, offline command words (depends on B-001). |
| B-015 | Evaluate xiaozhi v2.5.x | todo | Reference: artrobin-cn branch `public/work-secretary-v2.5`; it disables AEC to keep the wake word working, which must be solved first. |
| B-016 | Self-hosted xiaozhi server | todo | Optional; `xinnan-tech/xiaozhi-esp32-server` on the NAS (Docker). |

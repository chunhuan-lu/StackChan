# Backlog

**English** | [中文](backlog.zh-CN.md)

Status: `todo` · `decide` (needs a decision) · `doing` · `done` · `dropped`. Last updated: 2026-10-20.

## Done

| ID | Item | Notes |
|---|---|---|
| B-000 | Toolchain, backup and repository setup | ESP-IDF v5.5.4; unmodified v1.5.1 and v1.6.0 both build; full 16 MB flash backup of the official 1.6.0 device; fork + git workflow. See [devlog 2026-10-20](devlog/2026-10-20.md). |

## Next

| ID | Item | Status | Notes |
|---|---|---|---|
| B-001 | Custom wake word "起来干活" (qi lai gan huo) | doing | Decided: MultiNet `mn7_cn` + assets partition enlarged to 6080 KB. Implemented on `feat/wake-word`, builds; waiting for on-device verification. See [REQ-001](requirements/REQ-001-custom-wake-word.md). |
| B-002 | Disable xiaozhi automatic OTA | doing | `SC_DISABLE_AUTO_OTA` (default y) in the fork patch; implemented with B-001, not yet verified on the device. |
| B-003 | `fetch_repos.py`: multiple patches per repository | done | `extra_patches` in `repos.json`, idempotent re-runs, errors instead of silent skips; `tools/xiaozhi_fork_patch.py`. Tested on the host (already applied / dirty tree / clean tree). |
| B-004 | Wi-Fi throughput and latency | todo | Disable modem sleep during downloads and LAN control; move Wi-Fi/lwIP buffers to PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) and enlarge RX buffers / TCP window / UDP mailbox. |
| B-005 | Do not hold the LVGL lock while sleeping | todo | `_stackchan_update_task` holds the lock during `vTaskDelay(100)` while xiaozhi is busy (`hal.cpp`). |
| B-006 | Restore hotspot Wi-Fi provisioning | todo | 1.6 only allows App (BLE) setup, which self-built firmware cannot pass. Fallback today: BLE `setWifi` via nRF Connect. |
| B-007 | Audio scheduling review | todo | Only if choppy speech returns: `opus_codec` (prio 2) runs below the LVGL and stackchan tasks (prio 3). |

## Later

| ID | Item | Status | Notes |
|---|---|---|---|
| B-010 | LAN bridge on NAS + MCP tools for Claude Code | todo | Speak / emote / gesture / status; enabled only in selected projects or sessions; role-based tokens; remote access through Tailscale only. |
| B-011 | Idle behaviour, batch 1 | doing | Mood-driven director: schedule 08–23, sleep in the dark, touch/pick-up/tilt reactions, screen-only micro animations, mood light; silent and quiet. Implemented on `feat/idle-behavior`, builds; waiting for on-device verification. See [REQ-002](requirements/REQ-002-idle-behaviour.md). |
| B-017 | Idle behaviour, batch 2 | todo | Knock detection (tap the table), head-bob to music (needs mic level from xiaozhi), head levelling on tilt (after IMU axis calibration), persist the inner state. |
| B-012 | On-device face detection and tracking | todo | ESP-DL `human_face_detect`; buffers and stacks in PSRAM; pause during conversations; prototype memory and frame rate first. |
| B-013 | Desk object recognition | todo | NAS: YOLO (OpenVINO) + reaction rules; small VLM only for new or unknown objects. |
| B-014 | Extra ways to start and end a conversation | todo | Head touch, tap/shake (IMU), voice "exit" tool, offline command words (depends on B-001). |
| B-015 | Evaluate xiaozhi v2.5.x | todo | Reference: artrobin-cn branch `public/work-secretary-v2.5`; it disables AEC to keep the wake word working, which must be solved first. |
| B-016 | Self-hosted xiaozhi server | todo | Optional; `xinnan-tech/xiaozhi-esp32-server` on the NAS (Docker). |

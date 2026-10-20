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
| B-001 | Custom wake word "起来干活" (qi lai gan huo) | decide | Not an available WakeNet phrase. Options: (a) existing WakeNet phrase, no partition change; (b) MultiNet `mn7_cn` (2.6 MB) + enlarge the 4 MB assets partition into the ~1.9 MB free at the end of flash, which also enables offline command words; (c) paid custom WakeNet from Espressif. |
| B-002 | Disable xiaozhi automatic OTA | todo | `Application::CheckNewVersion()` installs new versions automatically and would overwrite self-built firmware. Notify only. |
| B-003 | `fetch_repos.py`: multiple patches per repository | todo | Needed for `xiaozhi-esp32-fork.patch` (conventions §4). |
| B-004 | Wi-Fi throughput and latency | todo | Disable modem sleep during downloads and LAN control; move Wi-Fi/lwIP buffers to PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) and enlarge RX buffers / TCP window / UDP mailbox. |
| B-005 | Do not hold the LVGL lock while sleeping | todo | `_stackchan_update_task` holds the lock during `vTaskDelay(100)` while xiaozhi is busy (`hal.cpp`). |
| B-006 | Restore hotspot Wi-Fi provisioning | todo | 1.6 only allows App (BLE) setup, which self-built firmware cannot pass. Fallback today: BLE `setWifi` via nRF Connect. |
| B-007 | Audio scheduling review | todo | Only if choppy speech returns: `opus_codec` (prio 2) runs below the LVGL and stackchan tasks (prio 3). |

## Later

| ID | Item | Status | Notes |
|---|---|---|---|
| B-010 | LAN bridge on NAS + MCP tools for Claude Code | todo | Speak / emote / gesture / status; enabled only in selected projects or sessions; role-based tokens; remote access through Tailscale only. |
| B-011 | Idle behaviour v2 | todo | Observation points, eyes move before the head, new expressions (surprised, curious), sleep when the room is dark. |
| B-012 | On-device face detection and tracking | todo | ESP-DL `human_face_detect`; buffers and stacks in PSRAM; pause during conversations; prototype memory and frame rate first. |
| B-013 | Desk object recognition | todo | NAS: YOLO (OpenVINO) + reaction rules; small VLM only for new or unknown objects. |
| B-014 | Extra ways to start and end a conversation | todo | Head touch, tap/shake (IMU), voice "exit" tool, offline command words (depends on B-001). |
| B-015 | Evaluate xiaozhi v2.5.x | todo | Reference: artrobin-cn branch `public/work-secretary-v2.5`; it disables AEC to keep the wake word working, which must be solved first. |
| B-016 | Self-hosted xiaozhi server | todo | Optional; `xinnan-tech/xiaozhi-esp32-server` on the NAS (Docker). |

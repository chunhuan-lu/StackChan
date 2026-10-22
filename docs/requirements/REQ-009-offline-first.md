# REQ-009 Offline first

**English** | [中文](REQ-009-offline-first.zh-CN.md)

Status: implemented on `feat/face-track`, flashed 2026-10-22; the online start is verified on the device, the
behaviour without a network still has to be tried · Backlog: B-024

## Goal

The maintainer uses the robot almost only for what works without the internet (faces, face tracking, Claude Code
status, settings) and rarely calls the voice assistant. The AI Agent mode should not depend on the network: only a
conversation needs it, and then a short English notice says so ("No network. Please connect to the internet
first."). Nothing else (no hint about the App, no Wi-Fi setup flow): the maintainer knows how to set Wi-Fi up.

## Before

- Everything of the fork (idle director, faces, face tracking, settings gear, Claude Code status on the face) starts
  only when xiaozhi reaches standby, and that needed Wi-Fi, the OTA version check and the server connection first.
- No Wi-Fi saved: App setup (a reboot) 1.5 s after opening the AI Agent. Saved Wi-Fi not found: App setup after 60 s.
  Wi-Fi without internet: the "Activating" error face with retries for up to about 85 minutes, no way out but a
  reboot.
- In standby, a lost server connection left the error face on screen until the server came back.
- The day / night schedule trusted only network time, although the RTC keeps the time.

## Behaviour (`CONFIG_SC_OFFLINE_FIRST`, Kconfig default off = upstream, fork on)

- **Standby right away**: the assets (fonts and the wake word model) are applied and xiaozhi goes from Starting to
  Idle at once (about 5 s after boot, with the ready sound), without waiting for Wi-Fi.
- **Network in the background**: when Wi-Fi connects, the version check and the server connection run in an
  activation task that changes no state, shows nothing and plays no sound. A failed version check is tried twice
  (5 s apart), then the server connection uses the settings saved by the last successful check. A server binding
  request still opens App setup, as upstream.
- **No App setup reboot**: saved networks are looked for again after the 60 s timeout (the station rescans at a
  growing interval); with none saved the robot stays offline. Wi-Fi is set up from the launcher's App setup.
- **Errors in standby** (a reconnect, the background start) are only logged.
- **A conversation** (wake word, screen tap, start listening): without Wi-Fi the notice shows for 5 s with the
  exclamation sound and the robot stays in standby; with Wi-Fi but no server connection yet, it connects first and
  opens the conversation when that is done. A conversation that cannot reach the server shows the same notice
  instead of the error screen.
- **Time**: the director trusts the system time when it is 2025 or later, which the RTC provides at boot (it was set
  from network time before and is kept by its battery); the log says "RTC" or "synced". SNTP is restarted when
  Wi-Fi connects.

## Internal RAM

The network start needs a lot of internal RAM at once: the HTTPS version check about 27 KB at its peak, the MQTT task
a 6 KB stack (esp-mqtt cannot put it in PSRAM). Upstream does it before standby; with standby first, the wake word
model (about 12 KB, 27 KB while it loads) and the face detection model were loaded already, and the first device
test failed: `mqtt_client: Error create mqtt task`, minimum free internal RAM 1.3 KB. Now:

- `hal_bridge::network_busy()` is set from boot until the first network start is done (or 20 s without Wi-Fi), and
  during every later background start. Face detection waits meanwhile and unloads its model.
- The first wake word start waits for it too (the model load peak), and runs before face detection loads its model.
- The background start releases the OTA object (about 9 KB) before the server connection starts.

Measured on the device (free internal RAM, minimum since boot):

| | Upstream order | Standby first, first version | Now |
|---|---|---|---|
| During the network start | 32 KB | 1.3 KB (MQTT task not created) | 23 KB |
| Lowest overall | about 6.2 KB (wake word model load) | 1.3 KB | 6.6 KB (wake word model load) |

## Files

| File | Change |
|---|---|
| `patches/xiaozhi-esp32-fork.patch` | `application.*`: standby at start, background activation, conversation gate, quiet errors, deferred wake word, network busy; `device_state_machine.cc`: Starting → Idle |
| `main/hal/board/stackchan.cc` | No App setup reboot from the Wi-Fi timeout or without saved Wi-Fi |
| `main/hal/board/hal_bridge.h`, `stackchan_display.cc` | `show_notice()` (a speech bubble that goes away), `set_network_busy()` / `network_busy()` |
| `main/hal/sc/face_probe.cpp` | Waits (and unloads the model) while the network starts |
| `main/stackchan/sc/idle_director.cpp` | Trusts RTC time |
| `main/Kconfig.projbuild`, `sdkconfig.defaults.sc` | `SC_OFFLINE_FIRST` (menu "Network, OTA and time"), on in the fork |

## Test on the device

1. With Wi-Fi: the face comes up about 5 s after the AI Agent starts; the log shows "Activation done" and "MQTT:
   Connected to endpoint" about 10 s later; the wake word works.
2. Router off (or out of range): the robot still comes to standby, faces and face tracking work, no reboot into App
   setup; "起来干活" or a screen tap shows "No network. Please connect to the internet first." for 5 s.
3. Router on again: within a few minutes the robot reconnects by itself; the next conversation works.

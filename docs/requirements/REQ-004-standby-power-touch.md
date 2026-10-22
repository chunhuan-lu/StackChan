# REQ-004 Standby, power and touch fixes

**English** | [中文](REQ-004-standby-power-touch.zh-CN.md)

Status: implemented on `fix/standby-power` (branched from `feat/idle-behavior`), **not yet verified on the device**
· Backlog: B-019

## Background

Reported by the maintainer on 2026-10-21, after a night with the REQ-001/REQ-002 build:

1. The screen is too dark while the robot sleeps.
2. On USB power the robot should not go into standby, once it is confirmed that it is not running from the battery.
3. After a long idle the device restarted; the log showed Wi-Fi reconnect and TLS errors, so it looked like a crash.

## Findings (source review and schematics, verified by a second review)

- **Restart**: not a crash. `[HAL] warm reboot request to app index: 0` comes only from the AI Agent Home button
  (swipe up from the bottom edge, then tap Home). Leaving AI Agent always restarts the device because xiaozhi cannot be
  stopped in place; `esp_restart()` stops Wi-Fi first, which produces the reconnect and TLS/MQTT errors. No panic
  banner, reset reason 0xc (software). The maintainer confirmed pressing Home.
  Side finding: the idle director ignores screen touches, so a tap on the dim sleeping face starts a conversation, and
  a swipe from the bottom edge can toggle the conversation too (LVGL sends CLICKED after a drag when nothing scrolls).
- **Backlight**: the dim screen is the director's night sleep. `CONFIG_SC_IDLE_SLEEP_BACKLIGHT=5` is already the
  lowest visible level: the AXP2101 feeds the backlight from DLDO1 in 9 steps of 100 mV, and every value from 1 to 12
  gives 2.5 V. Raising 5 to 10 would change nothing.
- **Power**:
  - On USB the upstream power-save timer (sleepy face after 5 min, power-off after 10 min) is already off, as long as
    the PMIC does not report the battery as discharging. What the maintainer saw as "standby" was the night sleep.
  - The firmware's "external power" flag really means "battery not discharging"; VBUS is never read.
  - Hardware (schematics in `docs/schematics`): **the servos always run from the battery cell** through a boost
    converter in the body, also on USB. USB powers the CoreS3 and recharges the cell (up to 700 mA). "Not running from
    the battery" therefore cannot be fully guaranteed.
  - The body has an INA226 (0x41) on a 10 mOhm shunt in the cell path, which sees the real cell current including
    servo load. The firmware did not use it.

## Decisions by the maintainer (2026-10-21)

- On USB: **keep the night schedule** (sleep 23:00–08:00 and in the dark), but never power-save or power off.
- Sleep backlight with the lights on: **45** (2.8 V). Dark room: unchanged (5, the dimmest step).
- Home: **no hold-to-confirm**; show feedback before the restart.

## Changes

| Area | Change |
|---|---|
| Sleep backlight | Two levels: `SC_IDLE_SLEEP_BACKLIGHT` (dark room, 5) and new `SC_IDLE_SLEEP_BACKLIGHT_LIT` (45) while the ambient light is above `SC_IDLE_BRIGHT_LUX`; switches to dark after 10 s below `SC_IDLE_DARK_LUX`. Never brighter than the normal brightness. Fades one 100 mV step per `SC_IDLE_BACKLIGHT_FADE_MS` (300 ms); restore on wake-up stays instant. Logs `sleep backlight N (x lux)`. |
| Screen tap while asleep | Wakes the robot instead of starting a conversation (at night it dozes off again after the 60 s grace). The wake word is unaffected. |
| Swipes | A press that starts in the bottom 20 px or moves more than 30 px no longer toggles the conversation (`Screen swipe ignored`). Screen taps are logged. |
| Home button | Clicks while the button slides in or out are ignored. When going home restarts the device (AI Agent and most apps; Settings only after changes that need it), a full-screen "Going home..." is drawn by the LVGL task and home follows 300 ms later. The overlay belongs to the home indicator and is deleted with it. Gesture and click are logged. |
| Touch controller | FT6336 samples with more than 2 points or coordinates outside 320x240 are dropped like a read error (the last state is kept); a release is reported only after 5 bad samples in a row. Rate-limited warning. |
| Reset reason | Logged at boot (`reset reason: N`: 3 software, 4 panic, 5–7 watchdog, 9 brownout). |
| Core dumps | Written to the existing 64 KB `coredump` partition; read with `idf.py coredump-info`. Coredump log strings off (they would cost ~5 KB internal RAM). |
| External power | `SC_EXT_POWER_NO_STANDBY`: external power = AXP2101 VBUS-good (REG00[5]), debounced (2 s to USB, 5 s to battery). The power-save timer stays off while USB power is present even if the battery helps out. "Allow auto power off while charging" is still honoured. |
| Power log | At boot, on every change and every 10 min: `Power: USB/battery (VBUS mV), PMIC battery charging/idle/discharging, %, cell mV mA -> power save on/off`. Warning once a minute if the PMIC reports discharging while USB is present. Cell values from the INA226 (sign convention still to be confirmed on the device). |
| Status bar | Battery icon shows the bolt while USB power is present; green bar only while charging. |
| Power-save exit | On battery, the upstream sleepy face keeps the sleep backlight instead of jumping back to full brightness; when power save ends (e.g. USB plugged in) the idle director is restarted instead of waiting for the next conversation. |
| Backlight (xiaozhi fork patch) | `Backlight::SetBrightness` retargets a pending transition: a second call from the same task (restore, then dim) used to be ignored. |

## Configuration (`StackChan fork options`)

| Option | Default |
|---|---|
| `SC_IDLE_SLEEP_BACKLIGHT` | 5 (dark room) |
| `SC_IDLE_SLEEP_BACKLIGHT_LIT` | 45 (lights on) |
| `SC_IDLE_BACKLIGHT_FADE_MS` | 300 |
| `SC_EXT_POWER_NO_STANDBY` | y |
| `SC_TOUCH_FIXES` | y |

## Files

| File | Change |
|---|---|
| `main/stackchan/sc/idle_director.*` | lit/dark sleep backlight with fade, screen-tap wake |
| `main/hal/board/stackchan_display.*` | swipe filter, tap logging, screen-tap wake hook, `SetPowerSaveMode` override |
| `main/apps/common/home_indicator/home_indicator.*`, `main/apps/app_setup/app_setup.cpp` | logging, slide guard, "Going home..." only when going home restarts |
| `patches/xiaozhi-esp32-fork.patch` | `boards/common/backlight.cc`: retarget pending transitions |
| `main/hal/board/stackchan.cc` | VBUS read, debounced external power, power log, FT6336 sample check |
| `main/hal/sc/ina226.*`, `main/hal/sc/hal_power.cpp` | new: INA226 reader, `Hal::isOnExternalPower()` |
| `main/hal/hal.h`, `main/hal/board/hal_bridge.h` | `isOnExternalPower()` |
| `main/apps/common/status_bar/status_bar.cpp` | bolt on USB power |
| `main/main.cpp` | reset reason log |
| `main/Kconfig.projbuild`, `sdkconfig.defaults.sc` | options above, core dumps |

## Known limitations

- The servos draw from the cell on USB too; repeated servo moves cycle the battery near full charge.
- The INA226 current sign is not confirmed yet; the log prints the raw signed value.
- Light from the backlight may leak into the ambient light sensor behind the front glass. If it keeps the lux above
  `SC_IDLE_DARK_LUX` at backlight 45, a sleeping robot would not dim when the room goes dark (check in test 1).
- Core dumps reserve a ~1.8 KB internal-RAM stack; check free internal RAM after a conversation.

## Acceptance criteria (on the device)

1. Night, room light on: the sleeping face fades to 45 (`sleep backlight 45`); light off → dims to 5 within about
   10–20 s; light on again → back to 45.
2. Tap on the sleeping face → it wakes (`screen tap while asleep`), no conversation starts; at night it dozes off again
   after about 60 s.
3. Tap on the awake face still starts / ends a conversation; a swipe up from the bottom edge only shows Home
   (`Screen swipe ignored`).
4. Home → "Going home..." is visible, then the restart; the next boot logs `reset reason: 3`.
5. Boot log shows `Power: USB (VBUS ~5000 mV)` on the CoreS3 USB-C and on the body USB-C, from a PC port and from a
   charger; unplugging gives `Power: battery` within about 5 s, replugging `Power: USB` within about 2 s; the status
   bar shows the bolt on USB.
6. On battery: after 5 min idle the sleepy face stays dim; plugging USB in brings the idle director back
   (`Power save ended: restart idle director`).
7. Record the INA226 cell current while charging at rest and while a servo moves (sign convention).
8. No regression: wake word, conversations, idle behaviours, free internal RAM after a conversation.

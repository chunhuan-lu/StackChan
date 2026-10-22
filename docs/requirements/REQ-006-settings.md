# REQ-006 Settings panel and voice settings

**English** | [中文](REQ-006-settings.zh-CN.md)

Status: implemented on `feat/settings`, **not yet verified on the device** · Backlog: B-021

## Goal

The fork features (idle director, faces, sleep screen, Claude Code link, standby on USB) had their values fixed at
build time. The maintainer wants them as settings with sensible defaults and ranges, changeable on the robot, and
friendly to people who fork the branch or open a PR:

1. Entry point: the existing swipe up from the bottom edge (the Home popup) plus a gear button. No new gestures.
2. Keep every candidate setting.
3. Settings can also be changed by voice.
4. Kconfig defaults follow the official behaviour; this fork's choices live in `sdkconfig.defaults.sc`.

## Configuration model

| Layer | Holds | Who changes it |
|---|---|---|
| Kconfig (`main/Kconfig.projbuild`, menu "StackChan fork options") | Feature switches (`SC_*`, default `n` = upstream behaviour), default values and ranges | Whoever builds the firmware (`idf.py menuconfig`, `sdkconfig.defaults*`) |
| `firmware/sdkconfig.defaults.sc` | This fork's choices: all features on, Melbourne timezone, sleep screen 5 / 45 | The fork maintainer |
| NVS namespace `sc_cfg` | Values changed on the device (panel or voice), always clamped to the range | The user; "Reset to defaults" goes back to the Kconfig values |

"Activity" is the upstream idle movement level and keeps its upstream storage (xiaozhi config), so the Settings app
and the panel always show the same value; it is listed with the idle director only (the upstream idle motion reads the
level once, when AI Agent starts), and it is re-read when AI Agent starts. Without `SC_SETTINGS_PANEL` the Kconfig
values are used and values left in NVS are ignored.

## Settings

Only settings whose feature is compiled in are listed.

| Key | Panel label | Range (step) | Default (Kconfig) | Needs |
|---|---|---|---|---|
| `activity` | Activity | Off / Low / Medium / High | upstream setting | `SC_IDLE_DIRECTOR` |
| `head_moves` | Head moves / hour | 0–120 (5) | `SC_IDLE_HEAD_MOVES_PER_HOUR` 20 | `SC_IDLE_DIRECTOR` |
| `sleep` | Sleeping | on / off | on | `SC_IDLE_DIRECTOR` (off: never sleeps, REQ-008) |
| `wake_hour` | Wake up at | 0–23 :00 | `SC_IDLE_WAKE_HOUR` 8 | `SC_IDLE_DIRECTOR` |
| `sleep_hour` | Go to sleep at | 0–23 :00 | `SC_IDLE_SLEEP_HOUR` 23 | `SC_IDLE_DIRECTOR` |
| `dark_lux` | Dark below | 0–100 lux | `SC_IDLE_DARK_LUX` 3 | `SC_IDLE_DIRECTOR` |
| `bright_lux` | Light above | 1–200 lux | `SC_IDLE_BRIGHT_LUX` 8 | `SC_IDLE_DIRECTOR` |
| `sleep_bl` | Sleep screen, dark | 0–100 % (5) | `SC_IDLE_SLEEP_BACKLIGHT` 5 | `SC_IDLE_DIRECTOR` |
| `sleep_bl_lit` | Sleep screen, lit | 0–100 % (5) | `SC_IDLE_SLEEP_BACKLIGHT_LIT` 45 | `SC_IDLE_DIRECTOR` |
| `led_max` | Light brightness | 0–255 (4) | `SC_IDLE_LED_MAX` 24 | `SC_IDLE_DIRECTOR` |
| `face_min` | Special face, from | 1–60 min | `SC_IDLE_FACE_MIN_MINUTES` 4 | `SC_IDLE_DIRECTOR` |
| `face_max` | Special face, to | 1–60 min | `SC_IDLE_FACE_MAX_MINUTES` 10 | `SC_IDLE_DIRECTOR` |
| `faces` | Expressive faces | on / off | `SC_IDLE_FACES` y | `SC_IDLE_DIRECTOR` |
| `tap_wake` | Tap wakes me up | on / off | on | `SC_IDLE_DIRECTOR` + `SC_TOUCH_FIXES` |
| `claude` | Show Claude Code | on / off | on | `SC_IDLE_DIRECTOR` + `SC_USB_LINK` |
| `claude_suit` | Pixel buddy suit | on / off | `SC_CLAUDE_SUIT` y | `SC_IDLE_DIRECTOR` + `SC_USB_LINK` (REQ-007) |
| `suit_color` | Suit colour | Clay / Orange / Bright / Custom | `SC_CLAUDE_SUIT_COLOR` 1 | same |
| `suit_r` / `suit_g` / `suit_b` | Custom red / green / blue | 0–255 (5) | `SC_CLAUDE_SUIT_CUSTOM_R/G/B` 217 / 119 / 87 | same |
| `suit_off_min` | Suit off after | 1–30 min | `SC_CLAUDE_SUIT_OFF_MINUTES` 2 | same |
| `suit_head` | Head moves along | on / off | `SC_CLAUDE_SUIT_HEAD` y | same |
| `suit_head_spd` | Head speed | 100–1000 (50) | `SC_CLAUDE_SUIT_HEAD_SPEED` 600 | same |
| `usb_awake` | No standby on USB | on / off | on | `SC_EXT_POWER_NO_STANDBY` |
| `face_track` | Look at faces | on / off | on | `SC_FACE_PROBE` ([REQ-008](REQ-008-face-tracking.md)) |
| `face_head` | Turn head to faces | on / off | on | same |
| `face_dead` | Head turns beyond | 2–20 deg | 8 | same |
| `face_speed` | Head speed | 100–1000 (50) | 500 | same |
| `face_steady` | Steadiness | Low / Medium / High | Medium | same |
| `face_box` | Show face box | on / off | off | same |
| `face_probe` | Face checks / s | 0–4 | 1 | same |

Pairs stay consistent: "light above" is kept above "dark below", and "special face, to" is never below "from" (also
when stored values and new Kconfig defaults disagree at boot).
Turning `faces` off brings back the dot eyes and the old eye animations (side-eye, silent hum, content squint);
turning `claude` off stops the session lights, faces and event reactions (the link keeps running); `usb_awake` off
restores the upstream power-save rule. All changes apply at once, without a restart.

## Panel

- AI Agent standby: swipe up from the bottom edge → the Home button slides in with a gear on its right → tap the gear.
- Full-screen list grouped by Idle / Sleep / Faces / Claude Code / Power: − / + buttons (hold to repeat) for numbers
  and choices, switches for on / off. Changes apply at once; they are written to NVS 1 s after the last change and when
  the panel closes (holding + / − would otherwise commit NVS ten times a second).
- "Reset to defaults" needs a second tap within 3 s. ✕ closes; the panel also closes after 60 s without a touch.
- Changes made by voice while the panel is open show up within 0.5 s.
- While the panel is open the robot stays awake; opening it on a sleeping face wakes the robot up.
- No gear when no setting is compiled in.

## Voice (MCP tools)

- `self.robot.get_settings`: list with key, label, value, shown text, min, max, default, help (and choices / on_off).
- `self.robot.set_setting(key, value)`: clamps, stores and returns the value now in use; unknown keys return an error
  text that points to `get_settings`.

Example: "睡觉的时候屏幕再暗一点" → the model calls `get_settings`, then `set_setting("sleep_bl_lit", 30)`.

## Implementation

| File | Change |
|---|---|
| `main/stackchan/sc/sc_settings.*` | Settings table, atomic values, NVS storage, clamping, pair rules, version counter |
| `main/stackchan/sc/settings_panel.*` | LVGL panel on `lv_layer_top()` |
| `main/hal/sc/settings_mcp.*` | The two MCP tools |
| `main/apps/common/home_indicator/*` | Gear button, `view::set_home_indicator_settings()` (inert unless called) |
| `main/hal/hal.cpp`, `main/hal/hal_mcp.cpp` | Hooks under `SC_SETTINGS_PANEL` |
| `main/stackchan/sc/idle_director.cpp` | Reads the settings at runtime instead of `CONFIG_SC_IDLE_*` |
| `main/hal/board/stackchan.cc`, `stackchan_display.cc` | `usb_awake` and the dark sleep backlight at runtime |
| `main/Kconfig.projbuild` | Submenus; defaults = upstream behaviour; new `SC_IDLE_FACES`, `SC_IDLE_FACE_MIN/MAX_MINUTES`, `SC_SETTINGS_PANEL` |
| `sdkconfig.defaults.sc` | Turns the fork features and the panel on |

Note for existing build trees: `sdkconfig` keeps its values; `idf.py reconfigure` adds the new symbols from
`sdkconfig.defaults.sc`.

## Acceptance criteria (on the device)

1. Swipe up in AI Agent standby: Home and gear slide in together; the gear opens the panel; Home still goes home.
2. Every row changes its value within the range; the value survives a restart.
3. `activity` changed in the panel is shown by Settings > AI Agent, and the other way round (after the restart that
   leaving Settings causes).
4. Live effect: `led_max` changes the mood light at once, `faces` off shows the dot eyes, `claude` off clears the
   session lights, `sleep_bl_lit` changes the sleeping screen.
5. "Reset to defaults" (two taps) restores the Kconfig values.
6. Voice: asking to change a setting calls `set_setting` and the panel / behaviour follow.
7. Building with all `SC_*` options off gives the upstream behaviour (no gear, no tools).

# REQ-002 Idle behaviour, batch 1

**English** | [中文](REQ-002-idle-behaviour.zh-CN.md)

Status: implemented on `feat/idle-behavior` (branched from `feat/wake-word`); first on-device test 2026-10-20 passed
for the parts tried, then tuned (see [dev log](../devlog/2026-10-20.md)); remaining acceptance tests pending
· Backlog: B-011

## Goal

Make the AI Agent standby state feel alive instead of "random head turns", while staying quiet.

## Decisions by the maintainer (2026-10-20)

- Implement the whole first batch, then try it out.
- **Silent**: no sounds at all while idle.
- Awake **08:00–23:00** local time.
- **Quiet**: few head movements; most "looking" is done by the on-screen eyes.

## Design

### Inner state (`IdleMood`, survives the director being re-created after each conversation)

| Value | Range | Changes |
|---|---|---|
| energy | 0–1 | follows the clock (morning 0.9, post-lunch dip 0.5, after 22:00 0.35) |
| boredom | 0–1 | +1 per 45 min without interaction; touch / pick-up / conversation lower it; decays while asleep |
| valence (mood) | 0–1 | petting +, conversation +, taps / shaking −; relaxes to 0.5 over ~30 min |
| affection | 0–1 | slowly grows with petting (runtime only, not persisted yet) |
| head tokens | 0–cap | servo budget, refilled at `SC_IDLE_HEAD_MOVES_PER_HOUR` × activity scale |

### Director (`IdleDirectorModifier`)

Replaces upstream `IdleMotionModifier` + `IdleExpressionModifier` in the idle slot of `StackChanAvatarDisplay`
(only while `CONFIG_SC_IDLE_DIRECTOR=y`). Every few seconds it picks behaviours by weighted random choice from the inner
state, with cooldowns and no immediate repetition.

| Area | Behaviour |
|---|---|
| Eye glances | every ~6–15 s (more often when bored), screen only |
| Spontaneous actions | every ~45–120 s at activity level 2: head look (servo, budgeted), yawn (tired), side-eye at the LEDs, sigh (bored), bug chase (bored, eyes follow a dot), sneeze / hiccup (rare), silent hum (happy), afternoon nod-off, eyes-only look-around, or nothing |
| Schedule | outside 08:00–23:00: dozes off and sleeps; at 08:00 a morning stretch (only once the room is light) |
| Darkness | ambient light < `SC_IDLE_DARK_LUX` for 1 min → sleep; light > `SC_IDLE_BRIGHT_LUX` for 5 s → wake up (at night: a grumpy one-eyed peek, keeps sleeping) |
| Sleep pose | eyes almost closed, "Zzz" bubble now and then, rare mumbling ("..."), head lowered, backlight `SC_IDLE_SLEEP_BACKLIGHT`, mood light off |
| Boredom | look-around and bug chase become more likely; at boredom ≥ 0.92 it takes a 10 min nap |
| Head touch | strokes (existing hearts reaction) + afterglow: contented squint and pink heartbeat light; 3 quick taps: annoyed; 6 taps: dizzy; long press 2.5 s: lulled into a 10 min nap; touching while asleep: peek (night/dark) or wake (nap) |
| Picked up / set down | startled wide eyes, "?!", eyes looking around while held; set down: relief (sweat, happy). Detection pauses while the head is touched and for 1.5 s afterwards (taps jolt the IMU) |
| Tilted > 22° | doubt + sweat until level again |
| Mood light | LEDs 1–5 and 7–11 (LED 0/6 stay with the status indicator); colour from mood (blue grumpy / teal calm / amber happy), brightness from energy, slow breathing; max channel `SC_IDLE_LED_MAX` |

### Time

- The schedule is only active after SNTP has synchronised the clock in this boot (`Hal::isTimeSynced()`), because
  xiaozhi's OTA check shifts the system clock by the timezone offset before that.
- Default timezone `SC_DEFAULT_TIMEZONE` = Melbourne `AEST-10AEDT,M10.1.0,M4.1.0/3` (upstream `GMT0`); a timezone
  chosen in Settings > System wins. "Melbourne (AEST/AEDT)" was added to that list (upstream only has fixed offsets).

## Configuration (`StackChan fork options`)

| Option | Default |
|---|---|
| `SC_DEFAULT_TIMEZONE` | `AEST-10AEDT,M10.1.0,M4.1.0/3` |
| `SC_IDLE_DIRECTOR` | y |
| `SC_IDLE_WAKE_HOUR` / `SC_IDLE_SLEEP_HOUR` | 8 / 23 |
| `SC_IDLE_HEAD_MOVES_PER_HOUR` | 20 (activity level 0/1/2/3 from the AI Agent settings: ×0 / ×0.5 / ×1 / ×2) |
| `SC_IDLE_DARK_LUX` / `SC_IDLE_BRIGHT_LUX` | 3 / 8 (sensor behind the front glass: desk lamp 11–16 lux) |
| `SC_IDLE_SLEEP_BACKLIGHT` | 5 |
| `SC_IDLE_LED_MAX` | 24 |

After changing Kconfig files run `idf.py reconfigure` (an incremental build does not pick up new options).

## Changes

| File | Change |
|---|---|
| `main/stackchan/sc/idle_director.*` | New: director, inner state, schedule, reactions, mood light, servo budget |
| `main/stackchan/sc/face_animation.*` | New: keyframe face animations (relative position offsets, coexist with BreathModifier), bug chase |
| `main/hal/sc/ltr553.*`, `hal_ambient_light.cpp` | New: LTR-553ALS driver (ALS only, gain 8x, 100 ms), `Hal::getAmbientLux()` with lazy init |
| `main/hal/hal.h` | `isTimeSynced()`, `getImuAccel()`, `getAmbientLux()` |
| `main/hal/hal_network.cpp` | SNTP sync flag |
| `main/hal/hal_imu.cpp` | keep the latest accelerometer sample |
| `main/hal/hal_rtc.cpp` | default timezone from Kconfig |
| `main/hal/board/stackchan_display.cc` | use the director in the idle slot |
| `main/apps/app_setup/workers/system.cpp` | timezone entry "Melbourne (AEST/AEDT)" |
| `main/Kconfig.projbuild`, `sdkconfig.defaults.sc` | options above |

## Deviations from the plan

- **Head levelling on tilt is deferred**: the IMU sits in the head and its axis/sign relative to the pitch servo was not
  known. Batch 1 reacts with an expression only. The first device test calibrated the axes (gravity on +Y, head pitch
  about X, pitch ≈ atan2(gz, gy)), so levelling moves to batch 2 (B-017).

## Known limitations

- On battery, upstream's power-save timer still takes over after 5 min idle ("sleepy" + Zzz) and the device powers off
  after the idle shutdown time (default 10 min). The director is meant for USB power.
- Lux thresholds were tuned once from the first test; IMU thresholds are still first guesses (lux is logged every 5 min).
- Inner state is not persisted across reboots.

## Acceptance criteria (on the device, USB power)

1. Standby shows eye glances and occasional actions; head moves stay rare (log `behaviour: head-look`, about
   ≤ 20 per hour); no sound is ever played.
2. Mood light visible on LEDs 1–5 / 7–11, LED 0 still shows listening / speaking status during conversations.
3. Covering the light sensor (or switching the light off) for > 1 min → dozes off; light back → wakes up.
4. Between 23:00 and 08:00 it sleeps (after SNTP sync); a touch gives a one-eyed peek; at 08:00 (room light on) it wakes
   with a stretch.
5. Taps ×3 → annoyed, ×6 → dizzy; long press → nap; two or more strokes → afterglow.
6. Picking it up → startled, set down → relief; tilting > 22° → doubt/sweat.
7. Saying the wake word at any time (also while asleep) starts a normal conversation; afterwards the face, backlight and
   lights are restored.

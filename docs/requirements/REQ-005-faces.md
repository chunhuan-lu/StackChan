# REQ-005 Expressive faces

**English** | [中文](REQ-005-faces.zh-CN.md)

Status: implemented on `feat/claude-link`, **not yet verified on the device** · Backlog: B-020

## Goal

The idle face looked bored and low-energy: two small dots and a flat 90×6 px line. The maintainer picked all 16 faces
from a mock-up page ("StackChan 表情候选", drawn at the real 320×240 geometry) and asked for them to be built in.
Changes should be rare: mostly one friendly base face, a special face now and then.

## How faces are drawn

The upstream skin draws the eyes as a white circle with a sliding black eyelid and the mouth as a rounded rectangle;
the six emotions only change eyelid weight and eye rotation. New `FaceArt` (`main/stackchan/sc/face_art.*`) draws
vector faces with LVGL objects (circles, rings, polylines, clipped half-discs; star and drops as small ARGB bitmaps
rasterised at runtime into PSRAM) on a layer inside the avatar panel, above the eyes and mouth, below the speech
bubble, never clickable. Faces that keep the round eyes (soft smile, sparkle, curious, peek, work, think and the open
eye of wink) leave blinking and looking to the existing modifiers and add glints that follow the eyes; the others
hide the default eyes and draw their own. The default mouth is always replaced while a face is shown.

## Faces and when they appear

| Face | When |
|---|---|
| Soft smile (round eyes with a glint, smile) | Base face whenever the robot is awake in AI Agent standby |
| Sparkle, joy, blush, curious, wink, hum, peek | Every 4–10 min one of them for 5–8 s, weighted by mood (energy, valence, affection, boredom); peek and hum also replace the old side-eye and silent-hum behaviours; blush is the stroke afterglow |
| Full (flat eyes, puffed cheeks) | Among the occasional faces, strongly preferred when a Claude Code session's context is above 80 % |
| Work (eyes down, determined brows) | A Claude Code session is working (edit, run, read, test) |
| Think (eyes up, "…" bubble) | Prompt received or subagents running |
| Curious | Session searching the web |
| Needs (ring eyes, "!") | A session waits for the user; never delayed |
| Done (gold stars, open smile) | `done` / `pass` events, and 20 s after a session finished |
| Fail (> <, zigzag mouth, sweat) | A test or build failed |
| Err (T T with tears) | API error, for 60 s |
| Compact (spinning spirals) | Context compaction |

Rules:
- Each face stays at least 3 s (no flicker when Claude Code switches tools quickly), except "needs".
- Faces are cleared while the robot sleeps, during reactions (pick-up, taps, shake, wake-up), during animations that
  move the mouth (yawn, sneeze, …) and while an emotion override is active; eye-only glances run on top of the soft
  smile.
- At night (23:00–08:00) Claude Code changes the LEDs only, as before.
- Conversations use the upstream face (the speaking mouth); the director clears its face when it is destroyed.

## Protocol (REQ-003 v1.1)

`act` and `ctx` per session and the `pass` event drive the Claude faces; see REQ-003.

## Known limitations

- The head cannot roll, so "curious" has no tilt; asymmetric eyes carry the expression.
- Faces are drawn at fixed eye positions when the default eyes are hidden (no glances on those faces).

## Acceptance criteria (on the device)

1. Awake standby shows the soft smile; it still blinks and glances, the glints follow the eyes.
2. About every 4–10 min a special face appears for a few seconds, then the soft smile returns.
3. With a linked Claude Code session: work / think / needs / done / fail / err / compact faces appear as listed;
   no flicker while tools change.
4. Screen taps, pick-up, taps on the head and conversations behave as before; after a conversation the soft smile
   returns.
5. Sleep pose and sleep backlight (REQ-004) unchanged.

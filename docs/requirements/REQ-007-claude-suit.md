# REQ-007 Claude Code pixel buddy suit

**English** | [中文](REQ-007-claude-suit.zh-CN.md)

Status: implemented on `feat/claude-suit`, flashed 2026-10-21; transitions not yet watched on the device · Backlog: B-023

## Goal

While Claude Code works, the robot should look like a "Claude robot", with a fun transition from the normal face
into it and back. The maintainer picked direction C (an orange pixel buddy, after the pixel creature in the Claude
Code terminal) from four mock-ups. Requests made along the way:

- sweat while working;
- the transition in: StackChan gets a Claude costume put on;
- the transition out: the buddy unzips itself, using its own hand on a long arm (not a flying block), with an
  outline so hands and arm stand out against the body;
- head movements (servos) during the transitions, switchable and with adjustable speed in the settings panel.

Design canvas (private): StackChan Claude 表情, artboards "C 像素小伙伴", "穿上 Claude 皮套", "脱下 Claude 皮套".

## When the suit is worn

- **On** when a linked session works or waits for the user, the robot is awake, it is not night and the settings
  `claude_suit`, `claude` and `faces` are on. Right after a conversation (the director is re-created) it comes back
  without the transition.
- **Off** `suit_off_min` minutes (default 2) after the last moment a session worked or waited, so it does not change
  clothes between two prompts; at once with the transition out when it becomes night or a setting is turned off.
- While it is worn the robot does not doze off, and the idle behaviours and special faces pause (they animate the
  hidden default features). Conversations use the upstream face as before.

## Poses

| Pose | When | Look |
|---|---|---|
| Idle | No session active, before the suit comes off | Pixel eyes, blinking and glancing now and then |
| Think | Prompt received, subagents, web search | Eyes up, three dots |
| Work | Edit, run, read, test | Eyes down, claws tapping, sweat flying off and running down |
| Needs | A session waits for the user | Big eyes, right claw waving, "!" |
| Done | `done` / `pass`, 20 s after a session finished | "^ ^" eyes, claws up, confetti |
| Fail | A test or build failed | "> <" eyes, drooping claws, a sweat drop |
| Err | API error | "T T" eyes with tears |
| Compact | Context compaction | One block circling in each eye |
| Full | Context above 80 % | Flat eyes, pink cheeks |

## Transitions

| Transition | Steps | Head (servos) |
|---|---|---|
| Suit on, 2.8 s | The suit drops from above and dangles, StackChan looks up ("o" mouth); thud, squash, dust; it wriggles in, its white eyes peek through the eye holes; blink into pixel eyes; claws and legs pop out, ta-da | Nod on the landing, lift for the ta-da |
| Suit off, 3.6 s | Sweaty buddy; a long outlined arm reaches over the head, grabs the zipper pull and pulls it down while the eyes follow; steam puffs out; the halves fall away; a flushed, sweaty StackChan shakes the sweat off, opens its eyes, smiles and puffs | Lean back at the steam, shake (yaw ±15°) with the sweat |

Pitch stays within 13–30° (limits 5–85°). Speed: `suit_head_spd` for the moves, a third of it for the return.

## Settings (REQ-006 panel and voice)

| Key | Panel label | Range | Default (Kconfig) |
|---|---|---|---|
| `claude_suit` | Pixel buddy suit | on / off | `SC_CLAUDE_SUIT` y |
| `suit_off_min` | Suit off after | 1–30 min | `SC_CLAUDE_SUIT_OFF_MINUTES` 2 |
| `suit_head` | Head moves along | on / off | `SC_CLAUDE_SUIT_HEAD` y |
| `suit_head_spd` | Head speed | 100–1000 (50) | `SC_CLAUDE_SUIT_HEAD_SPEED` 600 |

All need `SC_IDLE_DIRECTOR` + `SC_USB_LINK`.

## Implementation

- `main/stackchan/sc/pixel_buddy.*`: the buddy and both transitions, drawn with about 50 plain LVGL rectangles (and
  three white lines for StackChan's own face) on a layer of the avatar panel, above the default features and below
  the speech bubble. Each frame (40 ms) sets positions and sizes from the time in the transition; only changed
  objects are touched. The eye holes clip StackChan's peeking eyes; the long arm is an upper arm along the body's
  side plus a forearm to the hand, recomputed from the hand position. Transitions report head cues (dip, lift,
  recoil, shake).
- `main/stackchan/sc/idle_director.*`: when to wear it, the poses from the Claude session state and events (the same
  priority as the faces), and the cues turned into servo moves (`suit_head`, `suit_head_spd`).
- Without the suit (`claude_suit` off) the REQ-005 Claude faces are used as before.

## Acceptance criteria (on the device)

1. A prompt in a linked session: the suit-on transition plays once, with the nod and the lift.
2. Work shows sweat and tapping claws; needs / done / fail / err / compact faces appear as listed.
3. Two minutes after the session stops: the suit-off transition with the arm, the zipper, steam, the falling halves,
   the lean back and the head shake, then the soft smile.
4. No transition flapping between prompts; a conversation in between brings the suit back without a transition.
5. `suit_head` off: transitions without head movements; `suit_head_spd` changes how fast they are.
6. No crash, no lingering objects after many on/off cycles (heap log stable).

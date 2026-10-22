# REQ-008 Face tracking

**English** | [中文](REQ-008-face-tracking.zh-CN.md)

Status: implemented on `feat/face-track`. Detection verified on the device 2026-10-21 (the box is accurate and
follows the user); the tracking behaviour was flashed 2026-10-22 but not yet watched with a face (the room was dark)
· Backlog: B-012

## Goal

While the AI Agent is idle, the robot notices the person in front of it and looks at them: the eyes follow the
face, the head turns so the face stays in the middle of the camera view, and someone coming back after a while gets
a glad face. The maintainer chose this ("eye contact") from the camera ideas, asked for a measured prototype first,
then for the behaviour, with switches in the settings panel.

## Design

### Detection (`main/hal/sc/face_probe.*`)

- A task on core 1 at priority 1 (stack in PSRAM) runs ESP-DL `human_face_detect` 0.4 (two stages, MSR + MNP,
  esp-dl 3.2) while the AI Agent is idle **and** the idle director asked for detections in the last 2 s (it does so
  while awake with `face_track` or `face_box` on). Asleep, in a conversation or in another app the camera is left
  alone.
- Frames: GC0308 over DVP, 320 x 240 YUYV. Converted to 160 x 120 RGB888 (the first stage's input size, so the
  detector does not resize). The luma byte lane is checked on the first frame that is not flat; the default is the
  even bytes, as measured on this camera.
- The DVP driver has one buffer, so the frame waiting in it was taken right after the previous grab. When the last
  grab is older than 100 ms that frame is dropped and the next one taken; the frame time is the grab end minus 50 ms.
- Flat frames (both byte lanes vary less than 30: a dark room gives a uniform black level, not 0) and frames darker
  than a mean luma of 40 (the black level is about 28) are not run through the detector (no face to find, and detection costs the wake word time,
  see below).
- Rate: at most `face_probe` checks per second (default 1) while looking for a face; the director can ask for the
  next check at once when the head has settled after a turn, but never closer than 300 ms to the previous one.
- Every analysed frame publishes one `Sighting`: sequence number, frame time, kind, centre in 0..1 of the frame
  (camera view, not mirrored), size and score. Kinds, best first: `Face` (the detector's biggest, i.e. closest,
  face), `Track` (that face's patch found again), `Motion`, `None`. A debug overlay draws the box mirrored on the
  screen (`face_box`): green for a detected face, yellow for its tracked patch.
- Every 30 s one log line: detections, dark frames, faces, detection time, CPU of the wake word task and of both idle
  tasks, microphone audio lost, the largest microphone backlog and how often detection waited for the wake word.

### Between detections (`main/hal/sc/frame_cues.*`)

Detections are slow (about 200 ms) and rare (they wait for the wake word), so every frame also gets an 80 x 60 luma
copy (4 x 4 blocks from a quarter of the samples) for two cheap cues:

- **Patch tracking**: a detected face's patch (at least 6 x 6, at most the central 24 x 24 small pixels) is looked
  for every 250 ms near where it should be: the last position, shifted by the head turn since then (head angles
  from the director), +/-6 small pixels around it, zero-mean sum of absolute differences (brightness changes do not
  count). A match below 16 levels per pixel is accepted; one below 10 refreshes a quarter of the template, so a
  face turning away slowly stays found. Each detection renews the template; it is dropped 4 s after the last one or
  after two misses. No search while the head turns (blurred frames, lagging angles).
- **Motion**: while the director searches, a frame every 400 ms; the difference to the previous small frame (both
  taken with the head still at the same angles, means taken out) above 22 levels counts as moved. Fewer than 12
  moved pixels is noise, more than 35 % of the frame a change of the whole picture (exposure).
- Cue frames wait for the wake word too, but only above 160 ms of backlog.

### esp-dl and esp-sr in one image

esp-sr's prebuilt `libdl_lib.a` carries an old esp-dl whose 30 `dl_tie728_*` conv2d kernels have the same names as
esp-dl 3.x's but other arguments; the linker used esp-sr's copies for esp-dl too, so detection returned garbage and
took 25 times as long. `firmware/CMakeLists.txt` renames esp-dl's kernels while compiling it (`espdl3_` prefix), so
each library keeps its own, and builds esp-dl with `-Os` (the app partition is 95 % full; the hot kernels are
assembly anyway).

### The wake word and the microphone buffer

The custom wake word ("起来干活", MultiNet7) takes about 75 % of core 0 by itself. Detection runs on core 1, but both
use PSRAM and the shared cache: during a detection (about 200 ms) the wake word nearly stops. The six I2S DMA buffers
(60 ms) then overflowed and the wake word lost a part of what was said, so it hardly ever woke.

- `CONFIG_SC_MIC_BUFFER_MS` (Kconfig default 0 = upstream behaviour, fork 1000): a pump task on core 1 at priority 9
  drains the DMA buffers into a PSRAM stream buffer of that length; the codec's `Read()` takes from it. Nothing is
  lost any more; the wake word answers up to about 0.3 s later while it catches up.
- Detection starts only when less than 80 ms of audio wait in that buffer (back-pressure), so at 2 checks per second
  it automatically runs fewer.

### Behaviour (`main/stackchan/sc/idle_director_face.cpp`)

The idle director owns it, so it never fights the other idle behaviours for the head or the eyes.

- **Eyes**: the default eyes move towards the face (offsets of up to 70 / 40 position units at the edge of the
  view, eased at 0.25 units per ms). The screen faces the user, so a face on the robot's right moves the eyes to the
  screen's left. Drawn faces that bring their own eyes, the buddy suit and sleep get the eyes back centred. The
  offsets are relative, like the breathing and the glance animations; positions are clamped to +/-100, so the gaze
  only takes steps both eyes can follow, and it is dropped at once before an animation or a drawn face moves the
  eyes by its own offsets (their sum could hit the limit and the part cut off would never be undone) and when the
  director goes (conversation).
- **Directions in the room**: the director reads both servo angles every 100 ms and keeps the last 2.4 s. Each
  sighting becomes a direction: the head angle at the frame time (interpolated) plus the angle in the view. So frames
  taken while the head turns count as well, and errors do not add up from step to step.
- **Head** (`face_head`): follows that direction continuously, 0.35 s ahead of someone moving faster than 4 deg/s
  (speed from the last placements, smoothed), when the head points more than 4 degrees away; the servo speed grows
  with the distance (180-500). Yaw within +/-70 degrees, pitch within 15-75 degrees (inside the 5-85 degree safety
  range). A tracked patch keeps following going but does not start it (a face has to be detected first).
- **Direction**: positive yaw turns to the robot's right. Measured with the gyro on 2026-10-22: a +20 degree yaw
  move turned clockwise seen from above (peak -45.8 deg/s), the way back +42.6 deg/s. The upstream comment on
  `Motion::lookAtPoint` says the opposite; the voice tool ("-128 as your left") agrees with the measurement, and so
  did the first test with a face.
- **Lost** (no face or patch for 1.2 s): moving faster than 8 deg/s or last seen in the outer 40 % of the view, the
  head turns 15-30 degrees further that way, then back to the last place, then 15 degrees to the other side, looking
  1.5 s at each stop (with a detection once settled). Lost in the middle (turned away, covered): it waits there 4 s.
  Motion during the search (0.3-30 % of the view) turns the head towards it, at most every 2.5 s.
- **Someone here**: for 2 minutes after the last face or patch the robot does no head-look or nod-off behaviour, and
  when something else turned the head away (a reaction, a conversation) it turns back to where they were, at most
  every 5 s. The place is kept in the idle mood, so it survives a conversation.
- The earlier wrong-way guard is gone: with directions in the room an error does not compound, and the direction is
  verified. (On the first morning test it had paused head tracking for 30 minutes after two frames without a face.)
- Sightings older than 1.5 s (from before a conversation or a doze) are skipped.
- While following, idle glances wait 6 s and head behaviours 20 s after each placement.
- **Greeting**: a face found more than 2 minutes after the last face or interaction gets the Joy face for 2.5 s (the
  happy emotion when `faces` is off). Not while the buddy suit is on, an animation plays or the head is touched.
- Not while asleep, held, tilted, touched, during suit transitions, nod-off or the morning stretch.

### Sleep switch

`sleep` (panel "Sleeping", default on): off keeps the robot awake, no night, darkness or boredom sleep; a sleeping
robot wakes up when it is switched off. The xiaozhi power save is a separate thing (`usb_awake`).

## Settings (panel section "Camera", and "Sleep")

| Key | Label | Default | Meaning |
|---|---|---|---|
| `face_track` | Look at faces | on | Detection while idle, eyes follow the face |
| `face_head` | Turn head to faces | on | The head turns too |
| `face_box` | Show face box | off | Debug overlay with box, score and detection time |
| `face_probe` | Face checks / s | 1 | 0-4 while looking for a face; 0 = camera off |
| `sleep` | Sleeping | on | Off: never sleeps |

The camera settings exist only with `CONFIG_SC_FACE_PROBE` (Kconfig "Camera" → "Face tracking", default off; set in
the local sdkconfig for now, not in `sdkconfig.defaults.sc`, as it adds about 400 KB to a nearly full partition).

## Remote control over USB (protocol v1.3)

For testing without touching the robot, and for later automation (see [REQ-003](REQ-003-claude-code-status.md)):

- `set` `{k, v}`: change a settings panel setting (saved like a panel change); without `v` it reads it. The ack
  carries the value in `val`. sc-agent: `POST /setting {"key": "sleep", "value": 0}`, values in `/status` as
  `robot_settings`.
- `head` `{yaw, pitch, spd}`: move the head (degrees), and the IMU logs the strongest turn about the vertical during
  the next 4 s. sc-agent: `POST /head {"yaw": 20, "speed": 150}`.

## Measurements (2026-10-21/22)

| | |
|---|---|
| Detection, 160 x 120 | 140-200 ms (about 165 average); about 40 ms in Espressif's figures, which use octal PSRAM and `-O3` |
| Internal RAM free | 9 KB right after loading the model, 18-27 KB steady (minimum seen 3.5 KB on an earlier build) |
| App size | 0x4ad6b0 of 0x4f0000 (5 % free) |

Microphone audio lost per 10 s (10 ms chunks) at a fixed detection rate, and the wake word task's CPU:

| Checks / s | Before the buffer | With the buffer | audio_input CPU | core 0 idle |
|---|---|---|---|---|
| 0 | 0-10 | 0 | 74-78 % | 20-24 % |
| 1 | 62-112 (about 8 %) | 0 | 81 % → 89 % | 16 % → 8 % |
| 2 | 177-252 (about 20 %) | 0 (only ~1.4 run) | 83-86 % → 93-95 % | 12 % → 3-5 % |
| 4 | 289-327 (about 30 %) | 0 (only ~1.5 run) | 92 % | 5 % |

With the buffer the wake word task processes all the audio, hence the higher CPU; the backlog peaked at 170-270 ms.
The wake word was recognised with probability 0.39 right after the fix (0.20, the threshold, with detection on before;
0.70 without detection earlier that day).

## Privacy

Frames stay in RAM, are overwritten by the next one and are never stored, logged or sent. The prototype's frame
dumps (2026-10-21) are gone from the code, the serial log and the temporary folder. Any future capture that keeps a
frame must be announced on the screen first.

## Not done yet

- Watching the behaviour with a real face: direction of the eyes, the head step size and the greeting.
- During conversations the camera is off (the AFE and the cloud audio would compete with detection the same way).
- The model and esp-dl take about 400 KB of a nearly full app partition. The maintainer decided on 2026-10-22 to
  keep the feature off by default for now (enabled in the local sdkconfig only); a fork default would need a
  partition decision first.
- esp-dl issue #337 (same hardware): rare crashes in TIE728 depthwise kernels; not seen here so far.

## Test on the device

1. Sit in front of the robot in a lit room: the eyes should turn towards you within about a second; with
   `face_box` on, the box shows what the camera sees.
2. Move to the side, slowly and faster: the head should follow without stopping; turn your face away or look down:
   the yellow box should take over for a few seconds; walk out of the view on one side: the head should go after
   you; leave it alone: it should keep facing where you were.
3. Leave for more than 2 minutes and come back: Joy face.
4. Say "起来干活" while it watches you: it should wake every time (perhaps a fraction of a second later).
5. Switch `sleep` off at night: the robot wakes and stays awake; switch it on again.

## Files

| File | Change |
|---|---|
| `main/hal/sc/face_probe.*` | Detection task, `Sighting`, `keep_running()`, `request_check()`, `note_head()` |
| `main/hal/sc/frame_cues.*` | Small frames, patch tracking, motion |
| `main/stackchan/sc/idle_director_face.cpp`, `idle_director.*` | Tracking behaviour, gaze, wrong-way guard, sleep switch |
| `main/hal/board/cores3_audio_codec.*` | Microphone buffer and pump, lost-audio counter |
| `main/hal/hal_imu.cpp`, `hal.h` | `watchImuTurn()` |
| `main/hal/sc/usb_link.cpp` | `set` and `head` commands |
| `main/stackchan/sc/sc_settings.*` | `sleep`, `face_track`, `face_head`, `face_box`, `face_probe` |
| `firmware/CMakeLists.txt`, `main/idf_component.yml` | esp-dl kernel renames, `-Os`; `human_face_detect` 0.4.0 |
| `main/Kconfig.projbuild`, `sdkconfig.defaults.sc` | `SC_FACE_PROBE`, `SC_MIC_BUFFER_MS` (fork 1000) |
| `tools/sc-agent/sc_agent.py` | `POST /setting`, `POST /head`, `robot_settings` |

/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

namespace sc::face_probe {

/**
 * @brief On-device face detection for face tracking (REQ-008).
 *
 * A low-priority task on core 1 (the wake word runs on core 0) takes camera frames while the AI Agent is idle and
 * the idle director wants them. It runs ESP-DL's two-stage MSR+MNP face detector at the "face_probe" rate and, in
 * between, cheaper cues on an 80 x 60 copy of the frames (frame_cues.h): the last face's patch found again a few
 * times a second, and what moved while the head stood still. Every frame publishes one Sighting. It logs timing, CPU
 * use of the wake word task and lost microphone audio every 30 s. Frames are never stored and never leave the device.
 */
void start();

// Camera on the head (GC0308, about 66 degrees diagonal, 4:3): half the field of view in degrees.
constexpr float kHalfFovX = 27.0f;
constexpr float kHalfFovY = 20.5f;
// Faces the detector scores below this are weak (turned aside, looking down): taken only near the person.
constexpr float kStrongScore = 0.5f;

enum class SightKind : uint8_t {
    None,    // nothing found in this frame
    Face,    // the detector found a face
    Track,   // the last face's patch, found again (a face turned away or half out of view still is)
    Motion,  // no face, but something moved while the head stood still
};

struct Sighting {
    uint32_t seq   = 0;  // frames analysed so far; a new value means a new result
    uint32_t at    = 0;  // GetHAL().millis() when the frame was taken
    SightKind kind = SightKind::None;
    float cx       = 0;  // centre, 0..1 of the frame as the camera sees it (not mirrored):
    float cy       = 0;  // cx > 0.5 is the robot's right, cy > 0.5 is below the camera axis
    float size     = 0;  // face width (0..1 of the frame width), or the moving part of the frame
    float score    = 0;  // detector score, or track quality 0..1
};

// The latest result; false before the first frame.
bool latest(Sighting& out);

// The idle director calls this every update while it is awake and face tracking or the face box is on; frames are
// taken only while someone asked in the last two seconds. search: someone was here a moment ago and is not seen
// now, so look for motion between detections.
void keep_running(bool search = false);

// Run the next detection as soon as the wake word allows, instead of waiting for the rate (the head has settled).
void request_check();

// Servo angles (0.1 degree) and whether the head moves, from the director's polls: shift the tracked patch when the
// head turned, and tell motion from the picture moving with the head.
void note_head(uint32_t at, int yaw, int pitch, bool moving);

}  // namespace sc::face_probe

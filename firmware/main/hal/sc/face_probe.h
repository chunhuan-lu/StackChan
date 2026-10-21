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
 * the idle director wants them, runs ESP-DL's two-stage MSR+MNP face detector and publishes the biggest face. It
 * logs timing, CPU use of the wake word task and lost microphone audio every 30 s. Frames stay on the device and are
 * never stored.
 */
void start();

struct Sighting {
    uint32_t seq   = 0;  // detections so far; a new value means a new result, with or without a face
    uint32_t at    = 0;  // GetHAL().millis() when the frame was taken
    bool found     = false;
    float cx       = 0;  // centre of the biggest face, 0..1 of the frame as the camera sees it (not mirrored):
    float cy       = 0;  // cx > 0.5 is the robot's right, cy > 0.5 is below the camera axis
    float size     = 0;  // face width, 0..1 of the frame width
    float score    = 0;
};

// The latest result; false before the first detection.
bool latest(Sighting& out);

// Detection runs only while someone asked for it in the last two seconds (the idle director, every update while it
// is awake and face tracking or the face box is on).
void keep_running();

// Run the next detection as soon as the wake word allows, instead of waiting for the rate (the head has settled).
void request_check();

}  // namespace sc::face_probe

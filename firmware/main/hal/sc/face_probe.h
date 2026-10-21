/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace sc::face_probe {

/**
 * @brief Face detection prototype (REQ-008, step 1): measures what on-device face detection costs before any
 *        behaviour is built on it.
 *
 * A low-priority task on core 1 (the wake word runs on core 0) takes camera frames while the AI Agent is idle,
 * runs ESP-DL's two-stage MSR+MNP face detector, draws the face box on the screen and logs timing (grab,
 * convert, detect), the detection rate and memory every 10 s. Frames stay on the device.
 */
void start();

}  // namespace sc::face_probe

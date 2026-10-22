/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

namespace sc::face_probe {

/**
 * @brief Cheap cues between face detections (REQ-008): an 80 x 60 luma copy of every frame, the last face's patch
 *        found again in later frames (template tracking), and what moved since the previous frame while the head
 *        stood still (frame difference).
 *
 * Positions are fractions of the frame (0..1, camera view). Head angles (0.1 degree; yaw > 0 is the robot's right,
 * pitch > 0 up) shift the predicted patch position when the head turned between two frames.
 */
class FrameCues {
public:
    static constexpr int kW = 80;
    static constexpr int kH = 60;

    bool begin();

    // A new frame (YUYV, luma in the even bytes when luma_even); head angles when it was taken, and whether the head
    // stood still. Keeps the previous small frame for motion().
    void add_frame(const uint8_t* yuv, int w, int h, bool luma_even, uint32_t at, int yaw, int pitch, bool still);

    // What moved between the previous frame and this one, if both were taken with the head still and at the same
    // angles: centre and the moving part of the frame (0..1). False if nothing (or everything: exposure) moved.
    bool motion(float& cx, float& cy, float& area) const;

    // Remember the face in the box (fractions of the frame) of the current frame.
    void set_template(float x1, float y1, float x2, float y2, uint32_t at);
    // Find it in the current frame near where it should be; false (and the template dropped after two misses) if
    // not found. quality 0..1.
    bool track(float& cx, float& cy, float& quality);
    // A template younger than hold_ms (since the last set_template) is there.
    bool tracking(uint32_t now, uint32_t hold_ms) const;
    void drop_template();

private:
    uint8_t* _cur  = nullptr;
    uint8_t* _prev = nullptr;
    bool _have_prev = false;
    uint32_t _cur_at = 0, _prev_at = 0;
    int _cur_yaw = 0, _cur_pitch = 0, _prev_yaw = 0, _prev_pitch = 0;
    bool _cur_still = false, _prev_still = false;
    int _cur_mean = 0, _prev_mean = 0;

    static constexpr int kTplMax = 24;
    uint8_t _tpl[kTplMax * kTplMax];
    int _tw = 0, _th = 0;      // 0: no template
    int _tpl_mean = 0;
    float _tx = 0, _ty = 0;    // top left of the patch where it was last found
    int _t_yaw = 0, _t_pitch = 0;
    uint32_t _t_set_at = 0;
    int _misses = 0;
};

}  // namespace sc::face_probe

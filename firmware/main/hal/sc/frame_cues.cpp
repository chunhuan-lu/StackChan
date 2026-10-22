/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "frame_cues.h"
#include "face_probe.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sc::face_probe {

static constexpr int kN = FrameCues::kW * FrameCues::kH;

// Motion: a pixel moved when it changed by more than this (after both frames' means are taken out); fewer moved
// pixels than kMotionMin is noise, more than kMotionMax a change of the whole picture (exposure, light).
static constexpr int kMotionThreshold = 22;
static constexpr int kMotionMin       = 12;
static constexpr int kMotionMax       = kN * 35 / 100;
static constexpr int kSameAngle       = 5;  // 0.1 degree: the head did not turn between the two frames

// Tracking: search this far (small pixels) around the predicted position; accept a mean absolute difference up to
// kMaxSad, and let a clearly good match (below kBlendSad) refresh a quarter of the template, so a face that turns
// away slowly stays found.
static constexpr int kSearch    = 6;
static constexpr int kMaxSad    = 16;
static constexpr int kBlendSad  = 10;
static constexpr int kMaxMisses = 2;

bool FrameCues::begin()
{
    _cur  = static_cast<uint8_t*>(heap_caps_malloc(kN, MALLOC_CAP_SPIRAM));
    _prev = static_cast<uint8_t*>(heap_caps_malloc(kN, MALLOC_CAP_SPIRAM));
    return _cur && _prev;
}

void FrameCues::add_frame(const uint8_t* yuv, int w, int h, bool luma_even, uint32_t at, int yaw, int pitch, bool still)
{
    std::swap(_cur, _prev);
    _have_prev  = _cur_at != 0;
    _prev_at    = _cur_at;
    _prev_yaw   = _cur_yaw;
    _prev_pitch = _cur_pitch;
    _prev_still = _cur_still;
    _prev_mean  = _cur_mean;

    // 4 x 4 blocks, from 4 of their 16 luma samples (every other row and column): a quarter of the frame is read.
    const int bx = w / kW, by = h / kH;
    const int lane = luma_even ? 0 : 1;
    uint32_t sum   = 0;
    for (int y = 0; y < kH; ++y) {
        const uint8_t* row0 = yuv + static_cast<size_t>(y * by) * w * 2 + lane;
        const uint8_t* row1 = row0 + static_cast<size_t>(by / 2) * w * 2;
        uint8_t* out        = _cur + y * kW;
        for (int x = 0; x < kW; ++x) {
            const int i = x * bx * 2, j = i + (bx / 2) * 2;
            out[x]      = static_cast<uint8_t>((row0[i] + row0[j] + row1[i] + row1[j] + 2) >> 2);
            sum += out[x];
        }
    }
    _cur_mean  = static_cast<int>(sum / kN);
    _cur_at    = at;
    _cur_yaw   = yaw;
    _cur_pitch = pitch;
    _cur_still = still;
}

bool FrameCues::motion(float& cx, float& cy, float& area) const
{
    if (!_have_prev || !_cur_still || !_prev_still || _cur_at - _prev_at > 1500 ||
        std::abs(_cur_yaw - _prev_yaw) > kSameAngle || std::abs(_cur_pitch - _prev_pitch) > kSameAngle) {
        return false;
    }
    const int shift = _cur_mean - _prev_mean;
    int count = 0, sx = 0, sy = 0;
    for (int y = 0; y < kH; ++y) {
        const uint8_t* a = _cur + y * kW;
        const uint8_t* b = _prev + y * kW;
        for (int x = 0; x < kW; ++x) {
            if (std::abs(a[x] - b[x] - shift) > kMotionThreshold) {
                ++count;
                sx += x;
                sy += y;
            }
        }
    }
    if (count < kMotionMin || count > kMotionMax) {
        return false;
    }
    cx   = (sx / static_cast<float>(count) + 0.5f) / kW;
    cy   = (sy / static_cast<float>(count) + 0.5f) / kH;
    area = static_cast<float>(count) / kN;
    return true;
}

void FrameCues::set_template(float x1, float y1, float x2, float y2, uint32_t at)
{
    // The face box in small pixels, at least 6 x 6; a large face keeps its central kTplMax x kTplMax.
    float fx1 = x1 * kW, fx2 = x2 * kW, fy1 = y1 * kH, fy2 = y2 * kH;
    const float cx = (fx1 + fx2) * 0.5f, cy = (fy1 + fy2) * 0.5f;
    const int tw = std::clamp(static_cast<int>(std::lround(fx2 - fx1)), 6, kTplMax);
    const int th = std::clamp(static_cast<int>(std::lround(fy2 - fy1)), 6, kTplMax);
    const int left = std::clamp(static_cast<int>(std::lround(cx - tw * 0.5f)), 0, kW - tw);
    const int top  = std::clamp(static_cast<int>(std::lround(cy - th * 0.5f)), 0, kH - th);
    int sum        = 0;
    for (int y = 0; y < th; ++y) {
        std::memcpy(_tpl + y * tw, _cur + (top + y) * kW + left, tw);
        for (int x = 0; x < tw; ++x) {
            sum += _tpl[y * tw + x];
        }
    }
    _tw       = tw;
    _th       = th;
    _tpl_mean = sum / (tw * th);
    _tx       = static_cast<float>(left);
    _ty       = static_cast<float>(top);
    _t_yaw    = _cur_yaw;
    _t_pitch  = _cur_pitch;
    _t_set_at = at | 1;
    _misses   = 0;
}

bool FrameCues::tracking(uint32_t now, uint32_t hold_ms) const
{
    return _tw > 0 && static_cast<int32_t>(now - _t_set_at) < static_cast<int32_t>(hold_ms);
}

bool FrameCues::tracked_centre(float& cx, float& cy) const
{
    if (_tw == 0) {
        return false;
    }
    cx = (_tx + _tw * 0.5f) / kW;
    cy = (_ty + _th * 0.5f) / kH;
    return true;
}

void FrameCues::drop_template()
{
    _tw = _th = 0;
}

bool FrameCues::track(float& cx, float& cy, float& quality)
{
    if (_tw == 0) {
        return false;
    }
    // The head turned since the patch was found: the picture moved the other way (turning right moves things left,
    // looking up moves them down).
    const float px = _tx - (_cur_yaw - _t_yaw) / 10.0f / (2 * kHalfFovX) * kW;
    const float py = _ty + (_cur_pitch - _t_pitch) / 10.0f / (2 * kHalfFovY) * kH;
    const int x0 = static_cast<int>(std::lround(px)), y0 = static_cast<int>(std::lround(py));
    const int n  = _tw * _th;

    int best = INT32_MAX, bx = 0, by = 0;
    for (int y = std::max(0, y0 - kSearch); y <= std::min(kH - _th, y0 + kSearch); ++y) {
        for (int x = std::max(0, x0 - kSearch); x <= std::min(kW - _tw, x0 + kSearch); ++x) {
            int sum = 0;
            for (int ty = 0; ty < _th; ++ty) {
                const uint8_t* w = _cur + (y + ty) * kW + x;
                for (int tx = 0; tx < _tw; ++tx) {
                    sum += w[tx];
                }
            }
            const int shift = sum / n - _tpl_mean;  // zero-mean: brightness changes do not count
            int sad         = 0;
            for (int ty = 0; ty < _th && sad < best; ++ty) {
                const uint8_t* w = _cur + (y + ty) * kW + x;
                const uint8_t* t = _tpl + ty * _tw;
                for (int tx = 0; tx < _tw; ++tx) {
                    sad += std::abs(w[tx] - t[tx] - shift);
                }
            }
            if (sad < best) {
                best = sad;
                bx   = x;
                by   = y;
            }
        }
    }
    if (best == INT32_MAX || best > kMaxSad * n) {
        if (++_misses >= kMaxMisses) {
            drop_template();
        }
        return false;
    }
    _misses  = 0;
    _tx      = static_cast<float>(bx);
    _ty      = static_cast<float>(by);
    _t_yaw   = _cur_yaw;
    _t_pitch = _cur_pitch;
    if (best <= kBlendSad * n) {
        int sum = 0;
        for (int ty = 0; ty < _th; ++ty) {
            const uint8_t* w = _cur + (by + ty) * kW + bx;
            uint8_t* t       = _tpl + ty * _tw;
            for (int tx = 0; tx < _tw; ++tx) {
                t[tx] = static_cast<uint8_t>((3 * t[tx] + w[tx] + 2) >> 2);
                sum += t[tx];
            }
        }
        _tpl_mean = sum / n;
    }
    cx      = (bx + _tw * 0.5f) / kW;
    cy      = (by + _th * 0.5f) / kH;
    quality = 1.0f - static_cast<float>(best) / (kMaxSad * n);
    return true;
}

}  // namespace sc::face_probe

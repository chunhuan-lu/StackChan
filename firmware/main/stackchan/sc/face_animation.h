/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../avatar/avatar.h"
#include <lvgl.h>
#include <cstdint>
#include <vector>

namespace stackchan::sc {

/**
 * @brief A screen-only face animation (no servo movement, no sound).
 *
 * The caller locks the avatar while an animation runs so that BlinkModifier and friends pause.
 * Positions are applied as relative offsets because BreathModifier moves the features by deltas.
 */
class FaceAnimation {
public:
    virtual ~FaceAnimation() = default;

    virtual void begin(avatar::Avatar& avatar, uint32_t now) = 0;
    // Returns false once the animation has finished (the face is back to its base state).
    virtual bool step(avatar::Avatar& avatar, uint32_t now) = 0;
    // Abort early and restore the base state.
    virtual void end(avatar::Avatar& avatar) = 0;
};

/**
 * @brief One keyframe. Absolute channels use kBase for "value captured at start";
 *        look and bob are offsets in avatar position units (-100..100 maps to +-16 px).
 */
struct FaceKey {
    static constexpr int16_t kBase = -1000;

    uint32_t t       = 0;  // ms since start
    int16_t eyeL     = kBase;
    int16_t eyeR     = kBase;
    int16_t eyeSize  = kBase;
    int16_t mouth    = kBase;
    int16_t mouthRot = kBase;  // 0.1 degree, signed
    int16_t lookX    = 0;
    int16_t lookY    = 0;
    int16_t bobY     = 0;

    FaceKey& eyes(int16_t weight)
    {
        eyeL = eyeR = weight;
        return *this;
    }
    FaceKey& eyesLR(int16_t left, int16_t right)
    {
        eyeL = left;
        eyeR = right;
        return *this;
    }
    FaceKey& size(int16_t value)
    {
        eyeSize = value;
        return *this;
    }
    FaceKey& mouthOpen(int16_t weight)
    {
        mouth = weight;
        return *this;
    }
    FaceKey& mouthTilt(int16_t rotation)
    {
        mouthRot = rotation;
        return *this;
    }
    FaceKey& look(int16_t x, int16_t y)
    {
        lookX = x;
        lookY = y;
        return *this;
    }
    FaceKey& bob(int16_t y)
    {
        bobY = y;
        return *this;
    }
};

inline FaceKey key(uint32_t t)
{
    FaceKey k;
    k.t = t;
    return k;
}

/**
 * @brief Linear interpolation between keyframes; the last keyframe should return to base.
 */
class TimelineAnimation : public FaceAnimation {
public:
    explicit TimelineAnimation(std::vector<FaceKey> keys) : _keys(std::move(keys))
    {
    }

    void begin(avatar::Avatar& avatar, uint32_t now) override;
    bool step(avatar::Avatar& avatar, uint32_t now) override;
    void end(avatar::Avatar& avatar) override;

private:
    struct Base {
        int eyeL, eyeR, eyeSize, mouth, mouthRot;
    };

    void apply(avatar::Avatar& avatar, const FaceKey& a, const FaceKey& b, float f);
    void apply_offsets(avatar::Avatar& avatar, int look_x, int look_y, int bob_y);

    std::vector<FaceKey> _keys;
    Base _base{};
    uint32_t _start = 0;
    int _look_x     = 0;
    int _look_y     = 0;
    int _bob_y      = 0;
};

/**
 * @brief A small white dot wanders over the face and the eyes follow it.
 */
class BugChaseAnimation : public FaceAnimation {
public:
    explicit BugChaseAnimation(uint32_t durationMs = 5000) : _duration(durationMs)
    {
    }
    ~BugChaseAnimation() override;

    void begin(avatar::Avatar& avatar, uint32_t now) override;
    bool step(avatar::Avatar& avatar, uint32_t now) override;
    void end(avatar::Avatar& avatar) override;

private:
    void set_look(avatar::Avatar& avatar, int x, int y);

    uint32_t _duration;
    uint32_t _start = 0;
    lv_obj_t* _dot  = nullptr;
    float _phase[4] = {0, 0, 0, 0};
    int _look_x     = 0;
    int _look_y     = 0;
};

namespace anim {

std::vector<FaceKey> glance(int16_t x, int16_t y, uint32_t holdMs);
std::vector<FaceKey> yawn();
std::vector<FaceKey> sneeze();
std::vector<FaceKey> hiccup();
std::vector<FaceKey> sideEyeAtLight(bool left);
std::vector<FaceKey> sigh();
std::vector<FaceKey> silentHum();
std::vector<FaceKey> contentSquint();
std::vector<FaceKey> dozeOff();   // ends with the eyes almost closed, caller then applies the sleep pose
std::vector<FaceKey> wakeUp();    // starts from closed eyes
std::vector<FaceKey> sleepPeek();  // one eye opens briefly
std::vector<FaceKey> startled();
std::vector<FaceKey> lookAround(uint32_t durationMs);

}  // namespace anim

}  // namespace stackchan::sc

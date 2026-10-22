/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_animation.h"
#include "../utils/random.h"
#include <algorithm>
#include <cmath>

namespace stackchan::sc {

static int to_signed_rotation(int raw)
{
    return raw > 1800 ? raw - 3600 : raw;
}

static int to_raw_rotation(int value)
{
    return value < 0 ? 3600 + value : value;
}

static void move_feature(avatar::Feature& feature, int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }
    auto pos = feature.getPosition();
    pos.x += dx;
    pos.y += dy;
    feature.setPosition(pos);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                        TimelineAnimation                                       */
/* ---------------------------------------------------------------------------------------------- */

void TimelineAnimation::begin(avatar::Avatar& avatar, uint32_t now)
{
    _start         = now;
    _base.eyeL     = avatar.leftEye().getWeight();
    _base.eyeR     = avatar.rightEye().getWeight();
    _base.eyeSize  = avatar.leftEye().getSize();
    _base.mouth    = avatar.mouth().getWeight();
    _base.mouthRot = to_signed_rotation(avatar.mouth().getRotation());
    _look_x = _look_y = _bob_y = 0;
}

bool TimelineAnimation::step(avatar::Avatar& avatar, uint32_t now)
{
    if (_keys.empty()) {
        return false;
    }

    const uint32_t elapsed = now - _start;
    if (elapsed >= _keys.back().t) {
        apply(avatar, _keys.back(), _keys.back(), 0.0f);
        return false;
    }

    size_t i = 0;
    while (i + 1 < _keys.size() && _keys[i + 1].t <= elapsed) {
        ++i;
    }
    const FaceKey& a = _keys[i];
    const FaceKey& b = _keys[std::min(i + 1, _keys.size() - 1)];
    const float span = static_cast<float>(b.t - a.t);
    const float f    = span > 0 ? static_cast<float>(elapsed - a.t) / span : 1.0f;
    apply(avatar, a, b, f);
    return true;
}

void TimelineAnimation::end(avatar::Avatar& avatar)
{
    FaceKey base;
    apply(avatar, base, base, 0.0f);
}

void TimelineAnimation::apply(avatar::Avatar& avatar, const FaceKey& a, const FaceKey& b, float f)
{
    auto lerp = [f](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * f)); };
    auto pick = [](int16_t value, int base) { return value == FaceKey::kBase ? base : value; };

    avatar.leftEye().setWeight(lerp(pick(a.eyeL, _base.eyeL), pick(b.eyeL, _base.eyeL)));
    avatar.rightEye().setWeight(lerp(pick(a.eyeR, _base.eyeR), pick(b.eyeR, _base.eyeR)));

    const int size = lerp(pick(a.eyeSize, _base.eyeSize), pick(b.eyeSize, _base.eyeSize));
    if (avatar.leftEye().getSize() != size) {
        avatar.leftEye().setSize(size);
        avatar.rightEye().setSize(size);
    }

    avatar.mouth().setWeight(lerp(pick(a.mouth, _base.mouth), pick(b.mouth, _base.mouth)));
    const int rot = lerp(pick(a.mouthRot, _base.mouthRot), pick(b.mouthRot, _base.mouthRot));
    if (to_signed_rotation(avatar.mouth().getRotation()) != rot) {
        avatar.mouth().setRotation(to_raw_rotation(rot));
    }

    apply_offsets(avatar, lerp(a.lookX, b.lookX), lerp(a.lookY, b.lookY), lerp(a.bobY, b.bobY));
}

void TimelineAnimation::apply_offsets(avatar::Avatar& avatar, int look_x, int look_y, int bob_y)
{
    const int dx      = look_x - _look_x;
    const int dy_eyes = (look_y - _look_y) + (bob_y - _bob_y);
    const int dy_bob  = bob_y - _bob_y;

    move_feature(avatar.leftEye(), dx, dy_eyes);
    move_feature(avatar.rightEye(), dx, dy_eyes);
    move_feature(avatar.mouth(), 0, dy_bob);

    _look_x = look_x;
    _look_y = look_y;
    _bob_y  = bob_y;
}

/* ---------------------------------------------------------------------------------------------- */
/*                                        BugChaseAnimation                                       */
/* ---------------------------------------------------------------------------------------------- */

BugChaseAnimation::~BugChaseAnimation()
{
    if (_dot) {
        lv_obj_delete(_dot);
    }
}

void BugChaseAnimation::begin(avatar::Avatar& avatar, uint32_t now)
{
    _start = now;
    for (auto& p : _phase) {
        p = Random::getInstance().getFloat(0.0f, 6.28f);
    }

    _dot = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(_dot, 6, 6);
    lv_obj_set_style_radius(_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_dot, 0, 0);
    lv_obj_set_style_pad_all(_dot, 0, 0);
    lv_obj_move_foreground(_dot);
    step(avatar, now);
}

bool BugChaseAnimation::step(avatar::Avatar& avatar, uint32_t now)
{
    const uint32_t elapsed = now - _start;
    if (elapsed >= _duration) {
        end(avatar);
        return false;
    }

    // Wobbly figure-of-eight around the upper half of the face.
    const float t = elapsed / 1000.0f;
    const float x = 110.0f * std::sin(1.3f * t + _phase[0]) + 20.0f * std::sin(4.1f * t + _phase[1]);
    const float y = -40.0f + 45.0f * std::sin(1.9f * t + _phase[2]) + 12.0f * std::sin(5.3f * t + _phase[3]);
    if (_dot) {
        lv_obj_align(_dot, LV_ALIGN_CENTER, static_cast<int>(x), static_cast<int>(y));
    }

    // Eyes follow with a small lag: map screen pixels to avatar position units.
    const int target_x = std::clamp(static_cast<int>(x * 0.7f), -80, 80);
    const int target_y = std::clamp(static_cast<int>((y + 16.0f) * 0.9f), -70, 70);
    set_look(avatar, _look_x + (target_x - _look_x) / 3, _look_y + (target_y - _look_y) / 3);
    return true;
}

void BugChaseAnimation::end(avatar::Avatar& avatar)
{
    set_look(avatar, 0, 0);
    if (_dot) {
        lv_obj_delete(_dot);
        _dot = nullptr;
    }
}

void BugChaseAnimation::set_look(avatar::Avatar& avatar, int x, int y)
{
    move_feature(avatar.leftEye(), x - _look_x, y - _look_y);
    move_feature(avatar.rightEye(), x - _look_x, y - _look_y);
    _look_x = x;
    _look_y = y;
}

/* ---------------------------------------------------------------------------------------------- */
/*                                           Animations                                           */
/* ---------------------------------------------------------------------------------------------- */

namespace anim {

std::vector<FaceKey> glance(int16_t x, int16_t y, uint32_t holdMs)
{
    return {key(0), key(250).look(x, y), key(250 + holdMs).look(x, y), key(500 + holdMs)};
}

std::vector<FaceKey> yawn()
{
    return {
        key(0),
        key(500).eyes(55).mouthOpen(35),
        key(1100).eyes(25).mouthOpen(85).bob(-6),
        key(2000).eyes(20).mouthOpen(90).bob(-6),
        key(2600).eyes(60).mouthOpen(20),
        key(3000),
    };
}

std::vector<FaceKey> sneeze()
{
    return {
        key(0),
        key(300).eyes(45).mouthOpen(25).bob(-5),
        key(600).eyes(70).mouthOpen(10),
        key(900).eyes(35).mouthOpen(35).bob(-8),
        key(1050).eyes(10).mouthOpen(70).bob(12),
        key(1250).eyes(10).mouthOpen(40).bob(6),
        key(1700).eyes(80).mouthOpen(0),
        key(2000),
    };
}

std::vector<FaceKey> hiccup()
{
    std::vector<FaceKey> keys{key(0)};
    uint32_t t = 0;
    for (int i = 0; i < 3; ++i) {
        t += 600;
        keys.push_back(key(t).bob(0));
        keys.push_back(key(t + 90).bob(-12).size(15).mouthOpen(20));
        keys.push_back(key(t + 220).bob(0));
    }
    keys.push_back(key(t + 600));
    return keys;
}

std::vector<FaceKey> sideEyeAtLight(bool left)
{
    const int16_t x = left ? -75 : 75;
    return {
        key(0),
        key(400).look(x, 70).eyes(80),
        key(1800).look(x, 70).eyes(80),
        key(1950).look(x, 70).eyes(25),
        key(2100).look(x, 70).eyes(80),
        key(2600).look(x, 70).eyes(80),
        key(3100),
    };
}

std::vector<FaceKey> sigh()
{
    return {
        key(0),
        key(700).eyes(60).mouthOpen(15).bob(-6),
        key(1700).eyes(45).mouthOpen(5).bob(6),
        key(2600).eyes(70),
        key(3000),
    };
}

std::vector<FaceKey> silentHum()
{
    std::vector<FaceKey> keys{key(0), key(300).eyes(72)};
    uint32_t t = 300;
    for (int i = 0; i < 6; ++i) {
        t += 350;
        keys.push_back(key(t).eyes(72).mouthOpen(i % 2 ? 8 : 25).look(i % 2 ? -10 : 10, 0));
    }
    keys.push_back(key(t + 400));
    return keys;
}

std::vector<FaceKey> contentSquint()
{
    return {key(0), key(600).eyes(55).size(10), key(6000).eyes(55).size(10), key(7000)};
}

std::vector<FaceKey> dozeOff()
{
    return {
        key(0),
        key(800).eyes(55).mouthOpen(30),
        key(1600).eyes(30).mouthOpen(70).bob(-5),
        key(2400).eyes(35).mouthOpen(10),
        key(4000).eyes(8).mouthOpen(0),  // offsets back to 0: the sleep pose keeps no position offset
    };
}

std::vector<FaceKey> wakeUp()
{
    return {
        key(0).eyes(6),
        key(700).eyes(45),
        key(1000).eyes(10),
        key(1600).eyes(60).mouthOpen(40),
        key(2400).eyes(35).mouthOpen(80).bob(-6),
        key(3200).eyes(85).mouthOpen(10),
        key(3500).eyes(30),
        key(3700),
    };
}

std::vector<FaceKey> sleepPeek()
{
    return {key(0), key(300).eyesLR(55, FaceKey::kBase), key(1700).eyesLR(55, FaceKey::kBase), key(2100)};
}

std::vector<FaceKey> startled()
{
    return {
        key(0),
        key(120).eyes(100).size(70).mouthOpen(45).bob(-10),
        key(900).eyes(100).size(60).mouthOpen(30).look(-40, 0),
        key(1500).eyes(100).size(60).mouthOpen(30).look(40, 0),
        key(2100).eyes(100).size(55).mouthOpen(25).look(0, -20),
        key(2700),
    };
}

std::vector<FaceKey> lookAround(uint32_t durationMs)
{
    std::vector<FaceKey> keys{key(0)};
    uint32_t t = 0;
    while (t + 900 < durationMs) {
        t += Random::getInstance().getInt(500, 900);
        keys.push_back(
            key(t).look(Random::getInstance().getInt(-70, 70), Random::getInstance().getInt(-40, 40)).size(30));
    }
    keys.push_back(key(durationMs));
    return keys;
}

}  // namespace anim

}  // namespace stackchan::sc

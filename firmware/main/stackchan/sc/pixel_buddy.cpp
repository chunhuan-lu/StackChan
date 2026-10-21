/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "pixel_buddy.h"
#include "../utils/random.h"
#include <algorithm>
#include <cmath>

namespace stackchan::sc {

// Colours (the suit's own colour comes from setColors())
static constexpr uint32_t kInk     = 0x141413;
static constexpr uint32_t kIvory   = 0xFAF9F5;
static constexpr uint32_t kBlue    = 0x7FC8FF;
static constexpr uint32_t kGold    = 0xFFD166;
static constexpr uint32_t kPink    = 0xFF8FB1;
static constexpr uint32_t kWhite   = 0xFFFFFF;
static constexpr uint32_t kBlack   = 0x000000;

// Geometry in panel coordinates (320 x 240); StackChan's eyes are at (90|230, 104)
static constexpr int kBottom    = 190;
static constexpr int kBodyH     = 140;
static constexpr int kHalfW     = 100;
static constexpr int kHalfX[2]  = {60, 160};
static constexpr int kEyeX[2]   = {90, 230};
static constexpr int kEyeY      = 104;
static constexpr int kClawX[2]  = {30, 260};
static constexpr int kClawY     = 100;
static constexpr int kClaw      = 30;
static constexpr int kLegX[4]   = {80, 120, 180, 220};
static constexpr int kLeg       = 20;
static constexpr int kShoulderY = 115;
static constexpr int kArmX      = 262;
static constexpr int kArmW      = 12;
static constexpr int kPullX     = 155;
static constexpr int kPullY     = 46;
static constexpr int kGapX      = 157;
static constexpr int kLayerIndex = 3;  // like FaceArt: above the eyes and mouth, below the speech bubble

// Timing
static constexpr uint32_t kFrameMs = 40;
static constexpr uint32_t kOnMs    = 2800;
static constexpr uint32_t kOffMs   = 3600;

const char* buddy_pose_name(BuddyPose pose)
{
    static constexpr const char* kNames[] = {"idle", "think", "work", "needs", "done",
                                             "fail", "err",   "compact", "full"};
    const auto i = static_cast<size_t>(pose);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

static int rnd(int a, int b)
{
    return Random::getInstance().getInt(a, b);
}

// 0..1 between a and b
static float seg(uint32_t t, uint32_t a, uint32_t b)
{
    if (t <= a) return 0.0f;
    if (t >= b) return 1.0f;
    return static_cast<float>(t - a) / static_cast<float>(b - a);
}

static int lerp(int a, int b, float f)
{
    return a + static_cast<int>(std::lround((b - a) * f));
}

static float ease_in(float f)
{
    return f * f;
}

static float ease_out(float f)
{
    return 1.0f - (1.0f - f) * (1.0f - f);
}

// Piecewise linear through (time, value) keys; before the first key its value.
static int keys(uint32_t t, std::initializer_list<std::pair<uint32_t, int>> ks)
{
    auto prev = *ks.begin();
    if (t <= prev.first) {
        return prev.second;
    }
    for (const auto& k : ks) {
        if (t <= k.first) {
            return lerp(prev.second, k.second, seg(t, prev.first, k.first));
        }
        prev = k;
    }
    return prev.second;
}

// Stepped 0..1 progress through a period, like the design's steps() animations.
static float steps(uint32_t phase, uint32_t period, int count)
{
    return static_cast<float>(phase * count / period) / count;
}

/* ------------------------------------------------------------------------------------------------ */
/*                                              Objects                                             */
/* ------------------------------------------------------------------------------------------------ */

PixelBuddy::~PixelBuddy() = default;  // the owner calls hide() while the avatar exists (see FaceArt)

lv_obj_t* PixelBuddy::make(lv_obj_t* parent, uint32_t rgb, int radius)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

// A quadratic Bezier as a line object placed at its bounding box, so that moving it redraws little.
lv_obj_t* PixelBuddy::make_curve(std::array<lv_point_precise_t, 13>& pts, lv_point_t& origin, float x0, float y0,
                                 float cx, float cy, float x1, float y1, int width)
{
    float min_x = x0, min_y = y0;
    float xs[13], ys[13];
    for (int i = 0; i < 13; ++i) {
        const float t = i / 12.0f, u = 1.0f - t;
        xs[i]         = u * u * x0 + 2 * u * t * cx + t * t * x1;
        ys[i]         = u * u * y0 + 2 * u * t * cy + t * t * y1;
        min_x         = std::min(min_x, xs[i]);
        min_y         = std::min(min_y, ys[i]);
    }
    origin.x = static_cast<int32_t>(min_x) - width;
    origin.y = static_cast<int32_t>(min_y) - width;
    for (int i = 0; i < 13; ++i) {
        pts[i] = {static_cast<lv_value_precise_t>(xs[i] - origin.x), static_cast<lv_value_precise_t>(ys[i] - origin.y)};
    }
    lv_obj_t* o = lv_line_create(_layer);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(o, pts.data(), static_cast<uint32_t>(pts.size()));
    lv_obj_set_style_line_width(o, width, 0);
    lv_obj_set_style_line_color(o, lv_color_hex(kWhite), 0);
    lv_obj_set_style_line_rounded(o, true, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

void PixelBuddy::place(Box& b, int x, int y, int w, int h)
{
    if (!b.obj) {
        return;
    }
    if (!b.shown) {
        lv_obj_remove_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
        b.shown = true;
    }
    if (x != b.x || y != b.y) {
        lv_obj_set_pos(b.obj, x, y);
        b.x = x;
        b.y = y;
    }
    if (w >= 0 && (w != b.w || h != b.h)) {
        lv_obj_set_size(b.obj, w, h);
        b.w = w;
        b.h = h;
    }
}

void PixelBuddy::paint(Box& b, uint32_t rgb)
{
    if (b.obj && rgb != b.rgb) {
        lv_obj_set_style_bg_color(b.obj, lv_color_hex(rgb), 0);
        b.rgb = rgb;
    }
}

void PixelBuddy::fade(Box& b, lv_opa_t opa)
{
    if (b.obj && opa != b.opa) {
        lv_obj_set_style_bg_opa(b.obj, opa, 0);
        b.opa = opa;
    }
}

void PixelBuddy::off(Box& b)
{
    if (b.obj && b.shown) {
        lv_obj_add_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
        b.shown = false;
    }
}

void PixelBuddy::build(avatar::Avatar& av, lv_obj_t* panel)
{
    if (!_layer) {
        _layer = lv_obj_create(panel);
        lv_obj_remove_style_all(_layer);
        lv_obj_remove_flag(_layer, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(_layer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(_layer, 320, 240);
        lv_obj_align(_layer, LV_ALIGN_CENTER, 0, 0);  // centred: the panel has the theme's padding
        lv_obj_move_to_index(_layer, std::min<int32_t>(kLayerIndex, lv_obj_get_child_count(panel) - 1));
        lv_obj_add_event_cb(_layer, on_layer_deleted, LV_EVENT_DELETE, this);

        auto box = [&](Box& b, lv_obj_t* parent, uint32_t rgb, int radius) {
            b     = Box{};
            b.obj = make(parent, rgb, radius);
            b.rgb = rgb;
        };
        auto outline = [](Box& b, uint32_t rgb, int width) {
            lv_obj_set_style_border_color(b.obj, lv_color_hex(rgb), 0);
            lv_obj_set_style_border_width(b.obj, width, 0);
            lv_obj_set_style_border_opa(b.obj, LV_OPA_COVER, 0);
        };

        // StackChan's own face, back to front
        for (int i = 0; i < 2; ++i) {
            box(_sc_blush[i], _layer, kPink, LV_RADIUS_CIRCLE);
        }
        for (int i = 0; i < 2; ++i) {
            _sc_closed[i]     = Box{};
            _sc_closed[i].obj = make_curve(_closed_pts[i], _closed_org[i], kEyeX[i] - 14, 100, kEyeX[i], 114,
                                           kEyeX[i] + 14, 100, 6);
        }
        for (int i = 0; i < 2; ++i) {
            box(_sc_eye[i], _layer, kWhite, LV_RADIUS_CIRCLE);
            box(_sc_glint[i], _sc_eye[i].obj, kBlack, LV_RADIUS_CIRCLE);
            place(_sc_glint[i], 13, 5, 7, 7);
        }
        _sc_smile     = Box{};
        _sc_smile.obj = make_curve(_smile_pts, _smile_org, 140, 142, 160, 156, 180, 142, 5);
        box(_sc_o, _layer, kWhite, LV_RADIUS_CIRCLE);
        fade(_sc_o, LV_OPA_TRANSP);
        outline(_sc_o, kWhite, 4);

        // The suit
        if (_body == 0) {
            setColors(0xF2711C, 0x8C3A06);  // orange until the owner sets a colour
        }
        for (auto& b : _legs) box(b, _layer, _body, 0);
        for (auto& b : _claw) {
            box(b, _layer, _body, 0);
            outline(b, _outline, 2);
        }
        for (auto& b : _half) box(b, _layer, _body, 0);
        for (int i = 0; i < 2; ++i) {
            box(_hole[i], _layer, kBlack, 0);  // children are clipped to it: StackChan's eye peeks through
            box(_peek[i], _hole[i].obj, kWhite, LV_RADIUS_CIRCLE);
            box(_peek_glint[i], _peek[i].obj, kBlack, LV_RADIUS_CIRCLE);
            place(_peek_glint[i], 13, 5, 7, 7);
        }
        for (auto& eye : _eye_block) {
            for (auto& b : eye) box(b, _layer, kInk, 0);
        }
        box(_gap, _layer, kBlack, 0);
        for (Box* b : {&_upper_arm, &_forearm, &_hand}) {
            box(*b, _layer, _body, 0);
            outline(*b, _outline, 2);
        }
        box(_pull, _layer, kIvory, 0);
        for (auto& b : _parts) box(b, _layer, kIvory, 0);
        _parts_used = 0;
    }
    av.leftEye().setVisible(false);
    av.rightEye().setVisible(false);
    av.mouth().setVisible(false);
}

void PixelBuddy::forget()
{
    _layer = nullptr;
    for (int i = 0; i < 2; ++i) {
        _sc_eye[i] = _sc_glint[i] = _sc_closed[i] = _sc_blush[i] = Box{};
        _claw[i] = _half[i] = _hole[i] = _peek[i] = _peek_glint[i] = Box{};
        for (auto& b : _eye_block[i]) b = Box{};
    }
    _sc_smile = _sc_o = _gap = _upper_arm = _forearm = _hand = _pull = Box{};
    for (auto& b : _legs) b = Box{};
    for (auto& b : _parts) b = Box{};
    _parts_used = 0;
}

// The panel (and with it the layer) can be deleted by others, e.g. when the avatar is replaced.
void PixelBuddy::on_layer_deleted(lv_event_t* e)
{
    auto* self = static_cast<PixelBuddy*>(lv_event_get_user_data(e));
    if (self && self->_layer == lv_event_get_target(e)) {
        self->forget();
        self->_clip = Clip::None;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/*                                            Public API                                            */
/* ------------------------------------------------------------------------------------------------ */

void PixelBuddy::suitOn(avatar::Avatar& av, lv_obj_t* panel, uint32_t now)
{
    if (!panel) {
        return;
    }
    build(av, panel);
    _clip       = Clip::On;
    _clip_start = now;
    _cues_fired = 0;
    _cue        = BuddyCue::None;
    _next_frame = 0;
}

void PixelBuddy::suitOff(uint32_t now)
{
    if (!_layer || _clip == Clip::Off) {
        return;
    }
    _clip       = Clip::Off;
    _clip_start = now;
    _cues_fired = 0;
    _cue        = BuddyCue::None;
    _next_frame = 0;
}

void PixelBuddy::show(avatar::Avatar& av, lv_obj_t* panel, uint32_t now)
{
    if (!panel) {
        return;
    }
    build(av, panel);
    _clip       = Clip::None;
    _pose_start = now;
    _next_frame = 0;
}

void PixelBuddy::hide(avatar::Avatar& av)
{
    if (_layer) {
        lv_obj_t* layer = _layer;
        forget();
        lv_obj_delete(layer);  // the delete callback finds _layer already reset
    }
    _clip = Clip::None;
    av.leftEye().setVisible(true);
    av.rightEye().setVisible(true);
    av.mouth().setVisible(true);
    av.setEmotion(av.getEmotion());  // re-sync the eyelids
}

void PixelBuddy::setColors(uint32_t body, uint32_t outline)
{
    if (body == _body && outline == _outline) {
        return;
    }
    _body    = body;
    _outline = outline;
    if (!_layer) {
        return;  // build() uses the new colours
    }
    for (Box* b : {&_legs[0], &_legs[1], &_legs[2], &_legs[3], &_half[0], &_half[1]}) {
        paint(*b, body);
    }
    for (Box* b : {&_claw[0], &_claw[1], &_upper_arm, &_forearm, &_hand}) {
        paint(*b, body);
        if (b->obj) {
            lv_obj_set_style_border_color(b->obj, lv_color_hex(outline), 0);
        }
    }
}

void PixelBuddy::setPose(BuddyPose pose, uint32_t now)
{
    if (pose != _pose) {
        _pose       = pose;
        _pose_start = now;
    }
}

BuddyCue PixelBuddy::takeCue()
{
    const BuddyCue cue = _cue;
    _cue               = BuddyCue::None;
    return cue;
}

void PixelBuddy::fire(BuddyCue cue)
{
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(cue));
    if (!(_cues_fired & bit)) {
        _cues_fired |= bit;
        _cue = cue;
    }
}

void PixelBuddy::update(avatar::Avatar& av, uint32_t now)
{
    if (!_layer || now < _next_frame) {
        return;
    }
    _next_frame      = now + kFrameMs;
    const uint32_t t = now - _clip_start;
    switch (_clip) {
        case Clip::On:
            if (t >= kOnMs) {
                _clip       = Clip::None;
                _pose_start = now;
                draw_pose(now);
            } else {
                draw_on(t);
            }
            break;
        case Clip::Off:
            if (t >= kOffMs) {
                hide(av);
            } else {
                draw_off(t);
            }
            break;
        case Clip::None:
            draw_pose(now);
            break;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/*                                              Drawing                                             */
/* ------------------------------------------------------------------------------------------------ */

void PixelBuddy::draw_face(const FaceGeom& f)
{
    for (int i = 0; i < 2; ++i) {
        if (f.shown && f.open) {
            place(_sc_eye[i], kEyeX[i] - 12 + f.dx, kEyeY - 12 + f.look, 24, 24);
        } else {
            off(_sc_eye[i]);
        }
        if (f.shown && !f.open) {
            place(_sc_closed[i], _closed_org[i].x + f.dx, _closed_org[i].y);
        } else {
            off(_sc_closed[i]);
        }
        if (f.shown && f.blush > LV_OPA_TRANSP) {
            place(_sc_blush[i], (i ? 234 : 62) + f.dx, 124, 24, 12);
            fade(_sc_blush[i], f.blush);
        } else {
            off(_sc_blush[i]);
        }
    }
    if (f.shown && f.smile) {
        place(_sc_smile, _smile_org.x + f.dx, _smile_org.y);
    } else {
        off(_sc_smile);
    }
    if (f.shown && f.o) {
        place(_sc_o, 152 + f.dx, 140, 16, 16);
    } else {
        off(_sc_o);
    }
}

void PixelBuddy::draw_suit(const SuitGeom& g)
{
    if (!g.shown) {
        for (int i = 0; i < 2; ++i) {
            off(_half[i]);
            off(_hole[i]);
            off(_claw[i]);
        }
        for (auto& b : _legs) off(b);
        return;
    }
    const int bottom = kBottom + g.dy;
    const int h      = std::max(2, static_cast<int>(std::lround(kBodyH * g.sy)));
    for (int i = 0; i < 2; ++i) {
        const int ox = g.dx + (i ? g.split : -g.split);
        const int oy = g.drop;
        place(_half[i], kHalfX[i] + ox, bottom - h + oy, kHalfW, h);
        if (g.holes) {
            const int eh = std::max(2, static_cast<int>(std::lround(g.eyeH * g.sy)));
            const int cy = bottom - static_cast<int>(std::lround((kBottom - kEyeY) * g.sy)) + oy + g.eyeY;
            place(_hole[i], kEyeX[i] + ox + g.eyeX - g.eyeW / 2, cy - eh / 2, g.eyeW, eh);
        } else {
            off(_hole[i]);
        }
        if (g.claws && (i == 0 || g.clawR)) {
            place(_claw[i], kClawX[i] + ox + g.clawDx[i], kClawY + g.dy + oy + g.clawDy[i], kClaw, kClaw);
        } else {
            off(_claw[i]);
        }
    }
    for (int j = 0; j < 4; ++j) {
        const int ox = g.dx + (j < 2 ? -g.split : g.split);
        if (g.legs) {
            place(_legs[j], kLegX[j] + ox, kBottom + g.dy + g.drop + g.legsDy, kLeg, kLeg);
        } else {
            off(_legs[j]);
        }
    }
}

// The long right arm: from the shoulder along the body's side, then across to the hand.
void PixelBuddy::draw_arm(int hx, int hy)
{
    place(_hand, hx, hy, kClaw, kClaw);
    const int cy = hy + kClaw / 2;
    const int fx = hx + kClaw - 2;  // under the hand's right edge
    if (fx >= kArmX) {
        off(_forearm);
        off(_upper_arm);
        return;
    }
    place(_forearm, fx, cy - 6, kArmX + kArmW - fx, 12);
    const int top = std::min(cy - 6, kShoulderY - 6), bottom = std::max(cy + 6, kShoulderY + 6);
    place(_upper_arm, kArmX, top, kArmW, bottom - top);
}

void PixelBuddy::draw_eye_shape(EyeShape shape)
{
    for (int i = 0; i < 2; ++i) {
        const int cx = kEyeX[i];
        int n        = 0;
        auto block   = [&](int x, int y, int w, int h) { place(_eye_block[i][n++], x, y, w, h); };
        switch (shape) {
            case EyeShape::Chevron:  // ^ ^
                block(cx - 15, 105, 10, 10);
                block(cx - 5, 95, 10, 10);
                block(cx + 5, 105, 10, 10);
                break;
            case EyeShape::Cross:  // > <
                if (i == 0) {
                    block(cx - 12, 90, 10, 10);
                    block(cx - 2, 100, 10, 10);
                    block(cx - 12, 110, 10, 10);
                } else {
                    block(cx + 2, 90, 10, 10);
                    block(cx - 8, 100, 10, 10);
                    block(cx + 2, 110, 10, 10);
                }
                break;
            case EyeShape::Tee:  // T T
                block(cx - 14, 92, 28, 8);
                block(cx - 4, 100, 8, 20);
                break;
            case EyeShape::Flat:  // - -
                block(cx - 14, 100, 28, 8);
                break;
        }
        while (n < 3) off(_eye_block[i][n++]);
    }
}

void PixelBuddy::hide_eye_shapes()
{
    for (auto& eye : _eye_block) {
        for (auto& b : eye) off(b);
    }
}

void PixelBuddy::particle(int x, int y, int w, int h, uint32_t rgb)
{
    if (_parts_used >= kParticles) {
        return;
    }
    Box& b = _parts[_parts_used++];
    place(b, x, y, w, h);
    paint(b, rgb);
}

void PixelBuddy::drop(int x, int y)
{
    particle(x, y, 6, 6, kBlue);
    particle(x - 2, y + 6, 10, 10, kBlue);
}

void PixelBuddy::end_particles()
{
    for (int i = _parts_used; i < kParticles; ++i) off(_parts[i]);
    _parts_used = 0;
}

// Sweat: drops fly off the head and run down the face, in steps like the pixel design.
void PixelBuddy::sweat(uint32_t t)
{
    float f = steps(t % 1000, 1000, 4);
    drop(50 - static_cast<int>(24 * f), 40 - static_cast<int>(14 * f));
    f = steps((t + 500) % 1000, 1000, 4);
    drop(266 + static_cast<int>(24 * f), 40 - static_cast<int>(14 * f));
    f = steps((t + 300) % 1200, 1200, 4);
    drop(156, 22 - static_cast<int>(22 * f));
    f = steps(t % 1600, 1600, 6);
    drop(112, 58 + static_cast<int>(36 * f));
    f = steps((t + 800) % 1600, 1600, 6);
    drop(200, 58 + static_cast<int>(36 * f));
}

// StackChan puts the suit on: it drops from above, squashes, StackChan peeks out of the eye holes and
// blinks into pixel eyes, then claws and legs pop out (docs: REQ-007, "SuitOn").
void PixelBuddy::draw_on(uint32_t t)
{
    FaceGeom f;
    f.shown = t < 1050;  // covered by the suit afterwards
    f.look  = lerp(0, -8, seg(t, 150, 300));
    f.smile = t < 200;
    f.o     = t >= 200;
    draw_face(f);

    SuitGeom g;
    g.claws = t >= 2050;
    g.legs  = t >= 2100;
    if (t < 400) {
        g.dy = lerp(-250, -120, ease_out(seg(t, 0, 400)));
    } else if (t < 900) {
        g.dy = -120;
        g.dx = static_cast<int>(std::lround(3.0f * std::sin((t - 400) / 90.0f)));  // dangling
    } else if (t < 1050) {
        g.dy = lerp(-120, 0, ease_in(seg(t, 900, 1050)));
    }
    if (t >= 1050 && t < 1150) {
        g.sy = 1.0f - 0.16f * seg(t, 1050, 1150);
    } else if (t >= 1150 && t < 1250) {
        g.sy = 0.84f + 0.21f * seg(t, 1150, 1250);
    } else if (t >= 1250 && t < 1320) {
        g.sy = 1.05f - 0.05f * seg(t, 1250, 1320);
    }
    if (t >= 1320 && t < 1800) {
        g.dx = keys(t, {{1320, 0}, {1440, -4}, {1560, 4}, {1680, -3}, {1800, 0}});  // wriggling in
    }
    g.clawDx[0] = lerp(30, 0, seg(t, 2050, 2200));  // out from behind the body
    g.clawDx[1] = -g.clawDx[0];
    g.legsDy    = lerp(-20, 0, seg(t, 2100, 2250));
    if (t >= 2300) {
        g.clawDy[0] = g.clawDy[1] = t < 2600 ? lerp(0, -40, seg(t, 2300, 2400)) : lerp(-40, 0, seg(t, 2600, 2700));
    }
    draw_suit(g);

    for (int i = 0; i < 2; ++i) {
        if (t >= 1050 && t < 1900) {
            const int dx = keys(t, {{1500, 0}, {1600, -4}, {1700, 4}, {1800, 0}});
            const int h  = t < 1800 ? 24 : t < 1850 ? 12 : 4;  // blink
            place(_peek[i], _hole[i].w / 2 - 12 + dx, _hole[i].h / 2 - h / 2, 24, h);
        } else {
            off(_peek[i]);
        }
    }
    hide_eye_shapes();
    off(_gap);
    off(_pull);
    off(_hand);
    off(_forearm);
    off(_upper_arm);

    if (t >= 1050 && t < 1500) {  // dust from the landing
        const float k = seg(t, 1050, 1500);
        const int d = lerp(0, 16, k), u = lerp(0, 6, k);
        particle(44 - d, 182 - u, 8, 8, kIvory);
        particle(34 - d, 192 - u, 6, 6, kIvory);
        particle(50 - d, 198 - u, 5, 5, kIvory);
        particle(268 + d, 182 - u, 8, 8, kIvory);
        particle(280 + d, 192 - u, 6, 6, kIvory);
        particle(265 + d, 198 - u, 5, 5, kIvory);
    }
    if (t >= 1900 && t < 2050) {  // pop: the eyes switched to pixels
        for (int i = 0; i < 2; ++i) {
            const int x = _hole[i].x;
            particle(x - 10, 76, 6, 6, kIvory);
            particle(x + 24, 76, 6, 6, kIvory);
            particle(x - 10, 126, 6, 6, kIvory);
            particle(x + 24, 126, 6, 6, kIvory);
        }
    }
    if (t >= 2300 && t < 2650 && ((t - 2300) / 100) % 2 == 0) {  // ta-da
        particle(14, 34, 8, 8, kGold);
        particle(298, 34, 8, 8, kGold);
        particle(150, 18, 8, 8, kIvory);
        particle(110, 26, 6, 6, kGold);
        particle(204, 26, 6, 6, kIvory);
    }
    end_particles();

    if (t >= 1050) fire(BuddyCue::Dip);
    if (t >= 2300) fire(BuddyCue::Lift);
}

// The buddy takes the suit off: a long arm reaches over the head, pulls the zipper down, steam puffs out,
// the halves fall away and a hot StackChan shakes the sweat off (docs: REQ-007, "SuitOff").
void PixelBuddy::draw_off(uint32_t t)
{
    FaceGeom f;
    f.shown = t >= 2000;
    if (t >= 2500 && t < 3060) {
        f.dx = keys(t, {{2500, 0}, {2580, -6}, {2660, 6}, {2740, -6}, {2820, 6}, {2900, -4}, {2980, 4}, {3060, 0}});
    }
    f.open  = t >= 3100;
    f.o     = t < 3200;
    f.smile = t >= 3200;
    f.blush = t < 3300 ? LV_OPA_90 : t < 3450 ? LV_OPA_40 : LV_OPA_TRANSP;
    draw_face(f);

    const bool pulling = t >= 300 && t < 1600;
    SuitGeom g;
    g.shown = t < 2500;
    g.clawR = !pulling;
    g.eyeY  = t < 600 ? lerp(0, -8, seg(t, 300, 600)) : lerp(-8, 8, seg(t, 600, 1400));
    if (t >= 2000) {
        const float k = ease_in(seg(t, 2000, 2500));
        g.split       = lerp(0, 230, k);
        g.drop        = lerp(0, 80, k);
    }
    draw_suit(g);
    off(_peek[0]);
    off(_peek[1]);
    hide_eye_shapes();

    const int len = lerp(0, 140, seg(t, 600, 1400));
    if (t < 2000) {
        if (len > 0) {
            place(_gap, kGapX, kBottom - kBodyH, 6, len);
        } else {
            off(_gap);
        }
        place(_pull, kPullX, kPullY + len, 10, 8);
    } else {
        off(_gap);
        off(_pull);
    }
    if (pulling) {
        int hx = 158, hy = kPullY + len - 11;  // the hand's centre on the pull
        if (t < 600) {
            const float k = ease_out(seg(t, 300, 600));
            hx            = lerp(kClawX[1], 158, k);
            hy            = lerp(kClawY, kPullY - 11, k);
        } else if (t >= 1400) {
            const float k = seg(t, 1400, 1600);
            hx            = lerp(158, kClawX[1], k);
            hy            = lerp(kPullY + 140 - 11, kClawY, k);
        }
        draw_arm(hx, hy);
    } else {
        off(_hand);
        off(_forearm);
        off(_upper_arm);
    }

    if (t < 1500) {
        sweat(t);
    }
    if (t >= 1500 && t < 2000) {  // steam out of the open zipper
        const int up = lerp(0, 34, seg(t, 1500, 2000));
        particle(148, 34 - up, 8, 8, kIvory);
        particle(160, 22 - up, 10, 10, kIvory);
        particle(174, 34 - up, 8, 8, kIvory);
    }
    if (t >= 2000 && t < 3000) {  // StackChan's own sweat, shaken off
        const float k = seg(t, 2500, 3000);
        drop(62 - static_cast<int>(34 * k), 76 - static_cast<int>(12 * k));
        drop(252 + static_cast<int>(34 * k), 76 - static_cast<int>(12 * k));
        drop(156, 62 - static_cast<int>(26 * k));
    }
    if (t >= 3200 && t < 3500) {  // phew
        const float k = seg(t, 3200, 3500);
        const int dx = lerp(0, 22, k), dy = lerp(0, -8, k);
        particle(186 + dx, 142 + dy, 8, 8, kIvory);
        particle(196 + dx, 134 + dy, 6, 6, kIvory);
        particle(204 + dx, 142 + dy, 5, 5, kIvory);
    }
    end_particles();

    if (t >= 1500) fire(BuddyCue::Recoil);
    if (t >= 2500) fire(BuddyCue::Shake);
}

void PixelBuddy::draw_pose(uint32_t now)
{
    const uint32_t t = now - _pose_start;
    draw_face(FaceGeom{});
    off(_peek[0]);
    off(_peek[1]);
    off(_gap);
    off(_pull);
    off(_hand);
    off(_forearm);
    off(_upper_arm);

    SuitGeom g;
    bool shapes = false;
    switch (_pose) {
        case BuddyPose::Idle:
            if (_next_blink == 0) {
                _next_blink  = now + rnd(2500, 5000);
                _next_glance = now + rnd(5000, 9000);
            }
            if (now >= _next_blink) {
                _blink_until = now + 140;
                _next_blink  = now + rnd(3500, 7000);
            }
            if (now >= _next_glance) {
                _glance_until = now + rnd(1200, 2200);
                _glance_dx    = rnd(0, 1) ? 6 : -6;
                _next_glance  = now + rnd(6000, 12000);
            }
            g.eyeX = now < _glance_until ? _glance_dx : 0;
            if (now < _blink_until) {
                g.eyeH = 4;
            }
            break;
        case BuddyPose::Think: {
            g.eyeY        = -17;  // looking up
            g.eyeH        = 30;
            const int lit = (t / 300) % 4;
            for (int k = 0; k < lit && k < 3; ++k) particle(250 + 16 * k, 20, 10, 10, kIvory);
            break;
        }
        case BuddyPose::Work: {
            g.eyeY         = 9;  // looking down at the work
            g.eyeH         = 26;
            const bool tap = (t / 300) % 2;
            g.clawDy[0]    = tap ? 10 : 0;
            g.clawDy[1]    = tap ? 0 : 10;
            sweat(t);
            break;
        }
        case BuddyPose::Needs: {
            g.eyeW         = 28;
            g.eyeH         = 52;
            g.eyeY         = -4;
            g.clawDy[1]    = -50 + (((t / 250) % 2) ? -10 : 0);  // waving
            const int hop  = ((t / 400) % 2) ? -6 : 0;
            particle(26, 14 + hop, 10, 26, kGold);
            particle(26, 46 + hop, 10, 10, kGold);
            break;
        }
        case BuddyPose::Done: {
            shapes = true;
            draw_eye_shape(EyeShape::Chevron);
            g.clawDy[0] = g.clawDy[1] = -40 + (((t / 400) % 2) ? -10 : 0);
            static constexpr int kConfetti[6][2] = {{150, 20}, {204, 28}, {104, 26}, {12, 210}, {298, 200}, {292, 22}};
            for (int k = 0; k < 6; ++k) {
                if ((t / 200 + k) % 2 == 0) {
                    particle(kConfetti[k][0], kConfetti[k][1], 8, 8, k % 2 ? kGold : kIvory);
                }
            }
            break;
        }
        case BuddyPose::Fail:
            shapes = true;
            draw_eye_shape(EyeShape::Cross);
            g.clawDy[0] = g.clawDy[1] = 12;
            drop(266, 40);
            break;
        case BuddyPose::Err:
            shapes = true;
            draw_eye_shape(EyeShape::Tee);
            g.clawDy[0] = g.clawDy[1] = 12;
            for (int i = 0; i < 2; ++i) {  // tears
                const uint32_t phase = (t + i * 450) % 900;
                particle(kEyeX[i] - 3, 122 + static_cast<int>(phase * 40 / 900), 6, 6, kBlue);
            }
            break;
        case BuddyPose::Compact: {
            shapes        = true;
            const float a = t / 160.0f;
            for (int i = 0; i < 2; ++i) {  // dizzy: one block circling in each eye
                const float dir = i ? -1.0f : 1.0f;
                place(_eye_block[i][0], kEyeX[i] - 5 + static_cast<int>(std::lround(10 * std::cos(a * dir))),
                      kEyeY - 5 + static_cast<int>(std::lround(10 * std::sin(a * dir))), 10, 10);
                off(_eye_block[i][1]);
                off(_eye_block[i][2]);
            }
            break;
        }
        case BuddyPose::Full:
            shapes = true;
            draw_eye_shape(EyeShape::Flat);
            particle(70, 134, 18, 8, kPink);
            particle(232, 134, 18, 8, kPink);
            break;
    }
    g.holes = !shapes;
    if (!shapes) {
        hide_eye_shapes();
    }
    draw_suit(g);
    end_particles();
}

}  // namespace stackchan::sc

/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_art.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace stackchan::sc {

// Panel coordinates (320x240) of the default features, from DefaultEyes / DefaultMouth.
static constexpr int kCenterX = 160;
static constexpr int kCenterY = 120;
static constexpr int kEyeDx   = 70;
static constexpr int kEyeY    = 104;
static constexpr int kEyeL    = kCenterX - kEyeDx;
static constexpr int kEyeR    = kCenterX + kEyeDx;
static constexpr int kMouthY  = 146;
// DefaultAvatar::init creates the left eye, the right eye, the mouth and then the speech bubble: the art
// layer goes right before the bubble.
static constexpr int kLayerIndex = 3;

static constexpr uint32_t kWhite = 0xFFFFFF;
static constexpr uint32_t kBlack = 0x000000;
static constexpr uint32_t kPink  = 0xFF8FB1;
static constexpr uint32_t kBlue  = 0x7FC8FF;
static constexpr uint32_t kGold  = 0xFFD166;

static constexpr uint32_t kAnimIntervalMs = 50;

const char* face_style_name(FaceStyle style)
{
    static constexpr const char* kNames[] = {"none",  "soft-smile", "sparkle", "joy",  "blush",   "curious",
                                             "wink",  "hum",        "peek",    "work", "think",   "needs",
                                             "done",  "fail",       "err",     "compact", "full"};
    const auto i = static_cast<size_t>(style);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

/* ------------------------------------------------------------------------------------------------ */
/*                         Filled shapes LVGL cannot draw: small ARGB bitmaps                         */
/* ------------------------------------------------------------------------------------------------ */

template <typename Inside>
static const lv_image_dsc_t* rasterize(int w, int h, uint32_t rgb, Inside inside)
{
    const size_t size = static_cast<size_t>(w) * h * 4;
    auto* data        = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (!data) {
        return nullptr;
    }
    auto* dsc = new lv_image_dsc_t{};  // kept for the lifetime of the firmware, like the pixels
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    hits += inside(x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f) ? 1 : 0;
                }
            }
            uint8_t* p = data + (static_cast<size_t>(y) * w + x) * 4;  // ARGB8888 is stored B, G, R, A
            p[0]       = rgb & 0xFF;
            p[1]       = (rgb >> 8) & 0xFF;
            p[2]       = (rgb >> 16) & 0xFF;
            p[3]       = static_cast<uint8_t>(hits * 255 / 16);
        }
    }
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w      = w;
    dsc->header.h      = h;
    dsc->header.stride = w * 4;
    dsc->data_size     = size;
    dsc->data          = data;
    return dsc;
}

static const lv_image_dsc_t* star_image()
{
    static const lv_image_dsc_t* star = []() {
        float vx[10], vy[10];
        for (int i = 0; i < 10; ++i) {
            const float r = i % 2 ? 8.0f : 19.0f;
            const float a = -3.14159265f / 2 + i * 3.14159265f / 5;
            vx[i]         = 20 + r * std::cos(a);
            vy[i]         = 20 + r * std::sin(a);
        }
        return rasterize(40, 40, kGold, [&](float x, float y) {
            bool in = false;  // even-odd rule
            for (int i = 0, j = 9; i < 10; j = i++) {
                if ((vy[i] > y) != (vy[j] > y) && x < (vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]) + vx[i]) {
                    in = !in;
                }
            }
            return in;
        });
    }();
    return star;
}

// A drop pointing up: a circle at the bottom and a cone towards the top.
static const lv_image_dsc_t* drop_image(int w, int h)
{
    auto make = [](int w, int h) {
        const float cx = w / 2.0f, r = w / 2.0f - 1, cy = h - w / 2.0f;
        return rasterize(w, h, kBlue, [=](float x, float y) {
            const float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                return true;
            }
            return y < cy && y > 1 && std::fabs(dx) <= (y - 1) / (cy - 1) * r * 0.95f;
        });
    };
    static const lv_image_dsc_t* sweat = make(20, 28);
    static const lv_image_dsc_t* tear  = make(12, 18);
    return w >= 20 ? sweat : tear;
}

/* ------------------------------------------------------------------------------------------------ */
/*                                          Object helpers                                          */
/* ------------------------------------------------------------------------------------------------ */

lv_obj_t* FaceArt::obj(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* FaceArt::circle(lv_obj_t* parent, int cx, int cy, int r, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t* o = obj(parent);
    lv_obj_set_size(o, 2 * r, 2 * r);
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    return o;
}

lv_obj_t* FaceArt::ring(lv_obj_t* parent, int cx, int cy, int r, int width, lv_color_t color)
{
    lv_obj_t* o = circle(parent, cx, cy, r, color, LV_OPA_TRANSP);
    lv_obj_set_style_border_color(o, color, 0);
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t* FaceArt::rect(lv_obj_t* parent, int x, int y, int w, int h, int radius, lv_color_t color)
{
    lv_obj_t* o = obj(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

// The lower half of a disc whose flat edge is at y = top (an open mouth).
lv_obj_t* FaceArt::half_disc(lv_obj_t* parent, int cx, int top, int r, lv_color_t color)
{
    lv_obj_t* box = obj(parent);
    lv_obj_set_size(box, 2 * r, r);
    lv_obj_set_pos(box, cx - r, top);
    lv_obj_t* disc = obj(box);  // children are clipped to the box: only the lower half shows
    lv_obj_set_size(disc, 2 * r, 2 * r);
    lv_obj_set_pos(disc, 0, -r);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, color, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    return box;
}

// Points are given in panel coordinates; the line object is placed at their bounding box (plus the line
// width) so that redraws stay small.
lv_obj_t* FaceArt::line(lv_obj_t* parent, std::vector<lv_point_precise_t> points, int width, lv_color_t color)
{
    lv_value_precise_t min_x = points.empty() ? 0 : points[0].x, min_y = points.empty() ? 0 : points[0].y;
    for (const auto& p : points) {
        min_x = std::min(min_x, p.x);
        min_y = std::min(min_y, p.y);
    }
    const int ox = static_cast<int>(min_x) - width, oy = static_cast<int>(min_y) - width;
    for (auto& p : points) {
        p.x -= ox;
        p.y -= oy;
    }
    _points.push_back(std::move(points));
    auto& pts   = _points.back();
    lv_obj_t* o = lv_line_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, ox, oy);
    lv_line_set_points(o, pts.data(), static_cast<uint32_t>(pts.size()));
    lv_obj_set_style_line_width(o, width, 0);
    lv_obj_set_style_line_color(o, color, 0);
    lv_obj_set_style_line_rounded(o, true, 0);
    return o;
}

lv_obj_t* FaceArt::curve(lv_obj_t* parent, float x0, float y0, float cx, float cy, float x1, float y1, int width,
                         lv_color_t color)
{
    std::vector<lv_point_precise_t> pts;
    constexpr int kSegments = 12;
    for (int i = 0; i <= kSegments; ++i) {  // quadratic Bezier
        const float t = static_cast<float>(i) / kSegments, u = 1 - t;
        pts.push_back({static_cast<lv_value_precise_t>(u * u * x0 + 2 * u * t * cx + t * t * x1),
                       static_cast<lv_value_precise_t>(u * u * y0 + 2 * u * t * cy + t * t * y1)});
    }
    return line(parent, std::move(pts), width, color);
}

lv_obj_t* FaceArt::image(lv_obj_t* parent, const lv_image_dsc_t* dsc, int cx, int cy)
{
    if (!dsc) {
        return nullptr;
    }
    lv_obj_t* o = lv_image_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(o, dsc);
    lv_obj_set_pos(o, cx - static_cast<int>(dsc->header.w) / 2, cy - static_cast<int>(dsc->header.h) / 2);
    return o;
}

void FaceArt::glint(bool left, float dx, float dy, float radiusScale)
{
    _glints.push_back({circle(_layer, 0, 0, 2, lv_color_hex(kBlack)), left, dx, dy, radiusScale});
}

lv_obj_t* FaceArt::flt(lv_obj_t* o)
{
    if (o) {
        _float_objs.push_back(o);
    }
    return o;
}

// Points relative to a 44x44 box centred on the eye (the spiral object sits at cx-22, cy-22).
void FaceArt::spiral(int cx, int cy, float phase, std::vector<lv_point_precise_t>& points)
{
    (void)cx;
    (void)cy;
    points.resize(40);
    for (size_t i = 0; i < points.size(); ++i) {
        const float a = phase + i * 0.42f, r = 2.0f + i * 0.32f;
        points[i]     = {static_cast<lv_value_precise_t>(22 + r * std::cos(a)),
                         static_cast<lv_value_precise_t>(22 + r * std::sin(a))};
    }
}

void FaceArt::forget_objects()
{
    _layer = nullptr;
    _float_objs.clear();
    _glints.clear();
    _spiral_obj[0] = _spiral_obj[1] = nullptr;
    _spiral_pts[0] = _spiral_pts[1] = nullptr;
}

// The panel (and with it the layer) can be deleted by others, e.g. when the avatar is replaced.
void FaceArt::on_layer_deleted(lv_event_t* e)
{
    auto* self = static_cast<FaceArt*>(lv_event_get_user_data(e));
    if (self && self->_layer == lv_event_get_target(e)) {
        self->forget_objects();
    }
}

/* ------------------------------------------------------------------------------------------------ */
/*                                              Styles                                              */
/* ------------------------------------------------------------------------------------------------ */

static bool keeps_eyes(FaceStyle s)
{
    switch (s) {
        case FaceStyle::SoftSmile:
        case FaceStyle::Sparkle:
        case FaceStyle::Curious:
        case FaceStyle::Peek:
        case FaceStyle::Work:
        case FaceStyle::Think:
            return true;
        default:
            return false;
    }
}

// Only Peek sets the eyelids; Work and Think move the eyes but must keep blinking.
bool FaceArt::holdsEyes() const
{
    return _style == FaceStyle::Peek;
}

bool FaceArt::allowsGlances() const
{
    return _style == FaceStyle::None || _style == FaceStyle::SoftSmile || _style == FaceStyle::Sparkle ||
           _style == FaceStyle::Curious;
}

// The owner calls clear() while the avatar exists. If the avatar went first, its panel already deleted our
// layer, so nothing may be deleted here.
FaceArt::~FaceArt() = default;

void FaceArt::clear(avatar::Avatar& av)
{
    if (_layer) {
        lv_obj_t* layer = _layer;
        forget_objects();
        lv_obj_delete(layer);  // the delete callback finds _layer already reset
    }
    forget_objects();
    _points.clear();  // after the lines that pointed into them are gone
    if (_style == FaceStyle::None) {
        return;
    }
    // Undo what the style changed on the default features (positions relatively: BreathModifier moves them
    // by deltas).
    auto offset = [&](int dx, int dy) {
        for (auto* eye : {&av.leftEye(), &av.rightEye()}) {
            auto p = eye->getPosition();
            eye->setPosition({p.x - dx, p.y - dy});
        }
    };
    if (_style == FaceStyle::Peek) offset(50, 0);
    if (_style == FaceStyle::Work) offset(0, 40);
    if (_style == FaceStyle::Think) offset(-40, -45);
    av.leftEye().setSize(0);
    av.rightEye().setSize(0);
    av.leftEye().setVisible(true);
    av.rightEye().setVisible(true);
    av.mouth().setVisible(true);
    av.setEmotion(av.getEmotion());  // re-sync the eyelids
    _style = FaceStyle::None;
}

void FaceArt::set(avatar::Avatar& av, lv_obj_t* panel, FaceStyle style, uint32_t now)
{
    if (style == _style && (_layer || style == FaceStyle::None)) {
        return;
    }
    clear(av);
    if (style == FaceStyle::None || !panel) {
        return;
    }
    _style = style;
    _start = now;

    _layer = obj(panel);
    lv_obj_set_size(_layer, 320, 240);
    // Centred like the default features: the panel has the theme's padding, which a top-left position at
    // (0, 0) would add (13 px).
    lv_obj_align(_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_to_index(_layer, std::min<int32_t>(kLayerIndex, lv_obj_get_child_count(panel) - 1));
    lv_obj_add_event_cb(_layer, on_layer_deleted, LV_EVENT_DELETE, this);
    _float_y = 0;

    const bool keep = keeps_eyes(style);
    av.leftEye().setVisible(keep && style != FaceStyle::Wink);
    av.rightEye().setVisible(keep || style == FaceStyle::Wink);
    av.mouth().setVisible(false);
    build(av, style);
    place_glints(av);
}

void FaceArt::build(avatar::Avatar& av, FaceStyle style)
{
    const lv_color_t white = lv_color_hex(kWhite), black = lv_color_hex(kBlack), pink = lv_color_hex(kPink),
                     gold = lv_color_hex(kGold);
    auto smile       = [&]() { curve(_layer, 140, kMouthY - 4, 160, kMouthY + 10, 180, kMouthY - 4, 5, white); };
    auto small_smile = [&]() { curve(_layer, 150, kMouthY - 2, 160, kMouthY + 5, 170, kMouthY - 2, 4, white); };
    auto small_flat  = [&](int w) { rect(_layer, kCenterX - w / 2, kMouthY - 2, w, 4, 2, white); };
    auto open_mouth  = [&]() {
        half_disc(_layer, kCenterX, kMouthY - 6, 22, white);
        half_disc(_layer, kCenterX, kMouthY + 4, 11, pink);
    };
    auto happy_eyes = [&]() {
        for (int x : {kEyeL, kEyeR}) curve(_layer, x - 15, kEyeY + 5, x, kEyeY - 14, x + 15, kEyeY + 5, 6, white);
    };
    auto wavy = [&]() {
        std::vector<lv_point_precise_t> pts;
        for (int i = 0; i <= 8; ++i) {
            pts.push_back({static_cast<lv_value_precise_t>(140 + i * 5),
                           static_cast<lv_value_precise_t>(kMouthY + 2 + (i % 2 ? -3 : 3))});
        }
        line(_layer, std::move(pts), 4, white);
    };
    auto sparks = [&]() {
        flt(line(_layer, {{40, 50}, {40, 64}}, 3, gold));
        flt(line(_layer, {{33, 57}, {47, 57}}, 3, gold));
        flt(line(_layer, {{282, 160}, {282, 172}}, 3, gold));
        flt(line(_layer, {{276, 166}, {288, 166}}, 3, gold));
    };

    switch (style) {
        case FaceStyle::SoftSmile:
            av.leftEye().setSize(10);
            av.rightEye().setSize(10);
            glint(true, 0.35f, -0.35f, 0.28f);
            glint(false, 0.35f, -0.35f, 0.28f);
            smile();
            break;
        case FaceStyle::Sparkle:
            av.leftEye().setSize(60);
            av.rightEye().setSize(60);
            for (bool left : {true, false}) {
                glint(left, 0.35f, -0.35f, 0.33f);
                glint(left, -0.35f, 0.3f, 0.15f);
            }
            small_smile();
            sparks();
            break;
        case FaceStyle::Joy:
            happy_eyes();
            open_mouth();
            break;
        case FaceStyle::Blush:
            happy_eyes();
            small_smile();
            for (int x : {62, 258}) {
                lv_obj_t* cheek = circle(_layer, x, 134, 16, pink, LV_OPA_80);
                lv_obj_set_height(cheek, 14);
                lv_obj_set_y(cheek, 134 - 7);
            }
            break;
        case FaceStyle::Curious:
            av.leftEye().setSize(-17);
            av.rightEye().setSize(67);
            glint(false, 0.35f, -0.35f, 0.28f);
            ring(_layer, 166, kMouthY, 5, 3, white);
            break;
        case FaceStyle::Wink:
            curve(_layer, kEyeL - 14, kEyeY + 2, kEyeL, kEyeY - 10, kEyeL + 14, kEyeY + 2, 6, white);
            av.rightEye().setSize(10);
            glint(false, 0.35f, -0.35f, 0.28f);
            smile();
            break;
        case FaceStyle::Hum:
            for (int x : {kEyeL, kEyeR}) curve(_layer, x - 14, kEyeY - 4, x, kEyeY + 12, x + 14, kEyeY - 4, 6, white);
            ring(_layer, kCenterX, kMouthY + 2, 7, 4, white);
            flt(line(_layer, {{262, 64}, {262, 92}}, 4, white));
            flt(circle(_layer, 256, 93, 6, white));
            flt(line(_layer, {{262, 64}, {276, 60}, {276, 68}}, 4, white));
            break;
        case FaceStyle::Peek:
            for (auto* eye : {&av.leftEye(), &av.rightEye()}) {
                auto p = eye->getPosition();
                eye->setPosition({p.x + 50, p.y});
                eye->setWeight(55);
            }
            small_flat(28);
            break;
        case FaceStyle::Work:
            for (auto* eye : {&av.leftEye(), &av.rightEye()}) {
                auto p = eye->getPosition();
                eye->setPosition({p.x, p.y + 40});
            }
            line(_layer, {{kEyeL - 14, kEyeY - 20}, {kEyeL + 14, kEyeY - 13}}, 5, white);  // determined brows
            line(_layer, {{kEyeR - 14, kEyeY - 13}, {kEyeR + 14, kEyeY - 20}}, 5, white);
            small_flat(28);
            break;
        case FaceStyle::Think:
            for (auto* eye : {&av.leftEye(), &av.rightEye()}) {
                auto p = eye->getPosition();
                eye->setPosition({p.x - 40, p.y - 45});
            }
            small_smile();
            flt(rect(_layer, 236, 46, 62, 30, 15, white));
            for (int x : {253, 267, 281}) flt(circle(_layer, x, 61, 4, black));
            break;
        case FaceStyle::Needs:
            for (int x : {kEyeL, kEyeR}) {
                ring(_layer, x, kEyeY, 17, 5, white);
                circle(_layer, x, kEyeY, 7, white);
            }
            ring(_layer, kCenterX, kMouthY + 2, 7, 4, white);
            flt(rect(_layer, 268, 34, 10, 34, 4, gold));
            flt(circle(_layer, 273, 80, 6, gold));
            break;
        case FaceStyle::Done:
            for (int x : {kEyeL, kEyeR}) image(_layer, star_image(), x, kEyeY);
            open_mouth();
            sparks();
            break;
        case FaceStyle::Fail:
            line(_layer, {{kEyeL - 12, kEyeY - 12}, {kEyeL + 10, kEyeY}, {kEyeL - 12, kEyeY + 12}}, 6, white);
            line(_layer, {{kEyeR + 12, kEyeY - 12}, {kEyeR - 10, kEyeY}, {kEyeR + 12, kEyeY + 12}}, 6, white);
            {
                std::vector<lv_point_precise_t> zig;
                for (int i = 0; i <= 6; ++i) {
                    zig.push_back({static_cast<lv_value_precise_t>(136 + i * 8),
                                   static_cast<lv_value_precise_t>(kMouthY + (i % 2 ? -3 : 4))});
                }
                line(_layer, std::move(zig), 4, white);
            }
            flt(image(_layer, drop_image(20, 28), 262, 78));
            break;
        case FaceStyle::Err:
            for (int x : {kEyeL, kEyeR}) {
                line(_layer, {{x - 14, kEyeY - 8}, {x + 14, kEyeY - 8}, {x, kEyeY - 8}, {x, kEyeY + 10}}, 6, white);
                flt(image(_layer, drop_image(12, 18), x, kEyeY + 24));
            }
            wavy();
            break;
        case FaceStyle::Compact:
            for (int i = 0; i < 2; ++i) {
                _points.emplace_back();
                _spiral_pts[i] = &_points.back();
                spiral(i ? kEyeR : kEyeL, kEyeY, 0, *_spiral_pts[i]);
                _spiral_obj[i] = lv_line_create(_layer);
                lv_obj_remove_flag(_spiral_obj[i], LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_pos(_spiral_obj[i], (i ? kEyeR : kEyeL) - 22, kEyeY - 22);
                lv_line_set_points(_spiral_obj[i], _spiral_pts[i]->data(), _spiral_pts[i]->size());
                lv_obj_set_style_line_width(_spiral_obj[i], 4, 0);
                lv_obj_set_style_line_color(_spiral_obj[i], white, 0);
                lv_obj_set_style_line_rounded(_spiral_obj[i], true, 0);
            }
            wavy();
            break;
        case FaceStyle::Full:
            for (int x : {kEyeL, kEyeR}) line(_layer, {{x - 14, kEyeY}, {x + 14, kEyeY}}, 6, white);
            circle(_layer, 118, kMouthY + 4, 9, white);
            circle(_layer, 202, kMouthY + 4, 9, white);
            small_flat(20);
            break;
        case FaceStyle::None:
            break;
    }
}

// Glints sit on the default eyes, which move (glances, breathing) and change size.
void FaceArt::place_glints(avatar::Avatar& av)
{
    for (auto& g : _glints) {
        auto& eye    = g.left ? av.leftEye() : av.rightEye();
        const auto p = eye.getPosition();
        // Same integer mapping as DefaultEyes::setPosition (map_range -100..100 -> -16..16).
        const int ex = kCenterX + (g.left ? -kEyeDx : kEyeDx) - 16 + (std::clamp(p.x, -100, 100) + 100) * 32 / 200;
        const int ey = kCenterY - 16 - 16 + (std::clamp(p.y, -100, 100) + 100) * 32 / 200;
        const float r = (8 + (std::clamp(eye.getSize(), -100, 100) + 100) * 24 / 200) / 2.0f;
        const int gr  = std::max(2, static_cast<int>(std::lround(r * g.radiusScale)));
        lv_obj_set_size(g.obj, 2 * gr, 2 * gr);
        lv_obj_set_pos(g.obj, static_cast<int>(std::lround(ex + g.dx * r)) - gr,
                       static_cast<int>(std::lround(ey + g.dy * r)) - gr);
    }
}

void FaceArt::update(avatar::Avatar& av, uint32_t now)
{
    if (!_layer || now < _next_anim) {
        return;
    }
    _next_anim = now + kAnimIntervalMs;
    place_glints(av);

    // Extras bob by up to 4 px over 2.4 s.
    const float t   = (now - _start) / 2400.0f;
    const int bob_y = static_cast<int>(std::lround(-2.0f + 2.0f * std::cos(t * 6.2831853f)));
    if (bob_y != _float_y) {
        _float_y = bob_y;
        for (lv_obj_t* o : _float_objs) {
            lv_obj_set_style_translate_y(o, bob_y, 0);
        }
    }

    for (int i = 0; i < 2; ++i) {
        if (_spiral_obj[i] && _spiral_pts[i]) {
            spiral(i ? kEyeR : kEyeL, kEyeY, (now - _start) / 1000.0f * 4.0f * (i ? -1 : 1), *_spiral_pts[i]);
            lv_line_set_points(_spiral_obj[i], _spiral_pts[i]->data(), _spiral_pts[i]->size());
        }
    }
}

}  // namespace stackchan::sc

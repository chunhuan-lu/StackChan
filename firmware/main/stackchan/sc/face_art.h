/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../avatar/avatar.h"
#include <lvgl.h>
#include <cstdint>
#include <deque>
#include <vector>

namespace stackchan::sc {

/**
 * @brief Faces beyond the six upstream emotions (designs and rules: docs/requirements/REQ-005-faces.md).
 */
enum class FaceStyle : uint8_t {
    None,  // upstream default features only
    // idle
    SoftSmile,  // the base face while awake
    Sparkle,
    Joy,
    Blush,
    Curious,
    Wink,
    Hum,
    Peek,
    // Claude Code
    Work,
    Think,
    Needs,
    Done,
    Fail,
    Err,
    Compact,
    Full,
};

const char* face_style_name(FaceStyle style);

/**
 * @brief Vector faces drawn with LVGL objects on the avatar panel, above the default features and
 *        below the speech bubble, never clickable.
 *
 * Styles that keep the default round eyes (SoftSmile, Sparkle, Curious, Peek, Work, Think and the open
 * eye of Wink) leave blinking and looking to the existing modifiers and add glints, brows and their own
 * mouth; the others hide the default eyes and draw their own. Use under the LVGL lock only.
 */
class FaceArt {
public:
    ~FaceArt();

    void set(avatar::Avatar& avatar, lv_obj_t* panel, FaceStyle style, uint32_t now);
    void clear(avatar::Avatar& avatar);  // back to the default features
    void update(avatar::Avatar& avatar, uint32_t now);

    FaceStyle style() const
    {
        return _style;
    }
    // The style sets eye positions or eyelids itself: blinking and glances must pause meanwhile.
    bool holdsEyes() const;
    // The style keeps the default eyes free, so eye-only animations (glances) may run on top of it.
    bool allowsGlances() const;

private:
    struct Glint {
        lv_obj_t* obj;
        bool left;
        float dx, dy, radiusScale;  // relative to the eye radius
    };

    lv_obj_t* obj(lv_obj_t* parent);
    lv_obj_t* circle(lv_obj_t* parent, int cx, int cy, int r, lv_color_t color, lv_opa_t opa = LV_OPA_COVER);
    lv_obj_t* ring(lv_obj_t* parent, int cx, int cy, int r, int width, lv_color_t color);
    lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, int radius, lv_color_t color);
    lv_obj_t* half_disc(lv_obj_t* parent, int cx, int top, int r, lv_color_t color);
    lv_obj_t* line(lv_obj_t* parent, std::vector<lv_point_precise_t> points, int width, lv_color_t color);
    lv_obj_t* curve(lv_obj_t* parent, float x0, float y0, float cx, float cy, float x1, float y1, int width,
                    lv_color_t color);
    lv_obj_t* image(lv_obj_t* parent, const lv_image_dsc_t* dsc, int cx, int cy);
    void glint(bool left, float dx, float dy, float radiusScale);
    void spiral(int cx, int cy, float phase, std::vector<lv_point_precise_t>& points);

    lv_obj_t* flt(lv_obj_t* o);  // registers a gently bobbing extra (sparkles, notes, "!")
    void build(avatar::Avatar& avatar, FaceStyle style);
    void place_glints(avatar::Avatar& avatar);
    void forget_objects();
    static void on_layer_deleted(lv_event_t* e);

    FaceStyle _style    = FaceStyle::None;
    lv_obj_t* _layer    = nullptr;
    std::vector<lv_obj_t*> _float_objs;  // moved with translate_y: only their own small areas redraw
    int _float_y        = 0;
    uint32_t _start     = 0;
    uint32_t _next_anim = 0;
    std::vector<Glint> _glints;
    std::deque<std::vector<lv_point_precise_t>> _points;  // lv_line keeps pointers into these
    lv_obj_t* _spiral_obj[2] = {nullptr, nullptr};
    std::vector<lv_point_precise_t>* _spiral_pts[2] = {nullptr, nullptr};
};

}  // namespace stackchan::sc

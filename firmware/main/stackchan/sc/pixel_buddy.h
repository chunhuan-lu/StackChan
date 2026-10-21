/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../avatar/avatar.h"
#include <lvgl.h>
#include <array>
#include <climits>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace stackchan::sc {

// What the buddy does while the suit is on.
enum class BuddyPose : uint8_t { Idle, Think, Work, Needs, Done, Fail, Err, Compact, Full };

// Head movements a transition asks for; the idle director turns them into servo moves.
enum class BuddyCue : uint8_t { None, Dip, Lift, Recoil, Shake };

const char* buddy_pose_name(BuddyPose pose);

/**
 * @brief The Claude Code pixel buddy (docs/requirements/REQ-007-claude-suit.md): an orange pixel costume the
 *        robot wears while Claude Code works, and the transitions into it (the suit drops onto StackChan) and
 *        out of it (the buddy unzips itself and the halves fall off).
 *
 * Drawn with plain LVGL rectangles on a layer of the avatar panel, above the default features and below the
 * speech bubble; the default eyes and mouth are hidden while it is shown. Only objects whose position, size
 * or colour changed are touched each frame. Use under the LVGL lock only.
 */
class PixelBuddy {
public:
    ~PixelBuddy();

    void suitOn(avatar::Avatar& avatar, lv_obj_t* panel, uint32_t now);  // StackChan puts the suit on
    void suitOff(uint32_t now);                                          // the buddy takes it off
    void show(avatar::Avatar& avatar, lv_obj_t* panel, uint32_t now);    // suit on at once, no transition
    void hide(avatar::Avatar& avatar);                                   // gone at once, default features back
    void setPose(BuddyPose pose, uint32_t now);
    // 0 clay (the brand colour, muted on the CoreS3 LCD), 1 orange, 2 bright orange.
    void setColorScheme(int scheme);
    // Draws the next frame; hides itself when the transition out ends.
    void update(avatar::Avatar& avatar, uint32_t now);
    // The head movement asked for since the last call, if any.
    BuddyCue takeCue();

    bool visible() const
    {
        return _layer != nullptr;
    }
    bool takingOff() const
    {
        return _clip == Clip::Off;
    }
    BuddyPose pose() const
    {
        return _pose;
    }

private:
    enum class Clip : uint8_t { None, On, Off };
    enum class EyeShape : uint8_t { Chevron, Cross, Tee, Flat };

    struct Box {
        lv_obj_t* obj = nullptr;
        int x = INT_MIN, y = INT_MIN, w = -1, h = -1;
        uint32_t rgb  = UINT32_MAX;
        lv_opa_t opa  = LV_OPA_COVER;
        bool shown    = false;
    };

    // The suit's geometry for one frame.
    struct SuitGeom {
        bool shown = true;
        int dx = 0, dy = 0;     // whole suit
        float sy = 1.0f;        // squash about the bottom edge
        int split = 0, drop = 0;  // halves apart and falling
        bool holes = true;      // black eye holes, which are also the pixel eyes
        int eyeX = 0, eyeY = 0, eyeW = 20, eyeH = 40;
        bool claws = true, clawR = true, legs = true;
        int clawDx[2] = {0, 0}, clawDy[2] = {0, 0};
        int legsDy = 0;
    };

    // StackChan's own face under the suit during the transitions.
    struct FaceGeom {
        bool shown   = false;
        int dx       = 0;
        int look     = 0;
        bool open    = true;
        bool smile   = true;
        bool o       = false;
        lv_opa_t blush = LV_OPA_TRANSP;
    };

    static constexpr int kParticles = 16;  // sweat (10) is the most at once

    void build(avatar::Avatar& avatar, lv_obj_t* panel);
    void forget();
    static void on_layer_deleted(lv_event_t* e);
    lv_obj_t* make(lv_obj_t* parent, uint32_t rgb, int radius);
    lv_obj_t* make_curve(std::array<lv_point_precise_t, 13>& pts, lv_point_t& origin, float x0, float y0, float cx,
                         float cy, float x1, float y1, int width);
    void place(Box& b, int x, int y, int w = -1, int h = -1);
    void paint(Box& b, uint32_t rgb);
    void fade(Box& b, lv_opa_t opa);
    void off(Box& b);

    void draw_face(const FaceGeom& f);
    void draw_suit(const SuitGeom& g);
    void draw_arm(int hx, int hy);  // hand top-left at (hx, hy), arm from the right shoulder
    void draw_eye_shape(EyeShape shape);
    void hide_eye_shapes();
    void draw_on(uint32_t t);
    void draw_off(uint32_t t);
    void draw_pose(uint32_t now);
    void sweat(uint32_t t);
    void particle(int x, int y, int w, int h, uint32_t rgb);
    void drop(int x, int y);
    void end_particles();
    void fire(BuddyCue cue);

    lv_obj_t* _layer = nullptr;
    Clip _clip       = Clip::None;
    uint32_t _clip_start = 0;
    BuddyPose _pose      = BuddyPose::Idle;
    uint32_t _pose_start = 0;
    uint32_t _next_frame = 0;
    BuddyCue _cue        = BuddyCue::None;
    uint8_t _cues_fired  = 0;  // bit per cue of the running transition

    // Idle eye life
    uint32_t _next_blink  = 0;
    uint32_t _blink_until = 0;
    uint32_t _next_glance = 0;
    uint32_t _glance_until = 0;
    int _glance_dx        = 0;

    // StackChan underneath
    Box _sc_eye[2], _sc_glint[2], _sc_closed[2], _sc_smile, _sc_o, _sc_blush[2];
    std::array<lv_point_precise_t, 13> _closed_pts[2], _smile_pts;
    lv_point_t _closed_org[2] = {}, _smile_org = {};
    // The suit, back to front; the long arm (upper arm, forearm, hand) pulls the zipper
    Box _legs[4], _claw[2], _half[2], _hole[2], _peek[2], _peek_glint[2], _eye_block[2][3], _gap, _upper_arm,
        _forearm, _hand, _pull;
    std::array<Box, kParticles> _parts;
    int _parts_used = 0;
    uint32_t _body    = 0;  // suit colour and the outline of hands and arm
    uint32_t _outline = 0;
};

}  // namespace stackchan::sc

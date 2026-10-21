/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
// Face tracking for the idle director (REQ-008): the eyes follow the face the camera sees, and the head turns so
// the face stays in the middle of the view.
#include "idle_director.h"
#include "sc_settings.h"
#include <hal/sc/face_probe.h>
#include <mooncake_log.h>
#include <sdkconfig.h>
#include <algorithm>
#include <cmath>

#if CONFIG_SC_IDLE_DIRECTOR

namespace stackchan::sc {

static const std::string_view _tag = "FaceTrack";

// Camera on the head (GC0308, about 66 degrees diagonal, 4:3): half the field of view in degrees.
static constexpr float kHalfFovX = 27.0f;
static constexpr float kHalfFovY = 20.5f;
static constexpr float kPi       = 3.14159265f;

static constexpr uint32_t kMovePollMs   = 150;     // each servo read is a bus transaction
static constexpr uint32_t kSettleMs     = 250;     // after the servos stop, before a frame counts for the head
static constexpr uint32_t kLostMs       = 4000;    // no face this long: the eyes go back
static constexpr uint32_t kReleaseMs    = 10000;   // no face this long: glances and head behaviours resume
static constexpr uint32_t kGreetAfterMs = 2 * 60 * 1000;
static constexpr uint32_t kHeadLogMs    = 3000;
static constexpr float kWrongWayDeg     = 3.0f;  // a turn that leaves the face this much further off went wrong
static constexpr int kWrongWayStrikes   = 2;
static constexpr uint32_t kHeadOffMs    = 30 * 60 * 1000;  // head tracking pause after wrong-way turns
static constexpr uint32_t kMaxAgeMs     = 1500;  // older sightings (from before a conversation or a doze) are skipped

static constexpr float kDeadbandDeg = 5.0f;  // closer to the middle than this: the head stays
static constexpr float kHeadGain    = 0.7f;  // part of the angle the head turns per step (no overshoot)
static constexpr int kHeadSpeed     = 300;
static constexpr int kMaxYaw        = 700;   // 0.1 degree; +/-70 of the servo's +/-128
static constexpr int kMinPitch      = 150;   // within the 5-85 degree safety range
static constexpr int kMaxPitch      = 750;
static constexpr int kGazeX         = 70;    // eye offset for a face at the edge of the view
static constexpr int kGazeY         = 40;
static constexpr float kGazeSpeed   = 0.25f;  // eye offset units per ms

// Angle between the camera axis and a point of the frame, in degrees; frac 0..1 across the frame.
static float view_angle(float frac, float half_fov)
{
    return std::atan((frac - 0.5f) * 2.0f * std::tan(half_fov * kPi / 180.0f)) * 180.0f / kPi;
}

static int approach(int value, int target, int step)
{
    return value < target ? std::min(value + step, target) : std::max(value - step, target);
}

// cur + delta within [lo, hi], but never a move against delta (the head may stand outside the range already, e.g.
// still at the sleep pitch after being picked up).
static int step_within(int cur, int delta, int lo, int hi)
{
    const int target = std::clamp(cur + delta, lo, hi);
    return (delta > 0 && target < cur) || (delta < 0 && target > cur) ? cur : target;
}

void IdleDirectorModifier::update_face_track(Modifiable& sc, uint32_t now)
{
#if CONFIG_SC_FACE_PROBE
    auto& settings   = ScSettings::get();
    const bool track = settings.enabled(SettingId::FaceTrack);
    const bool awake = _phase == Phase::Awake && !_sleep_pose;
    if (awake && (track || settings.enabled(SettingId::FaceBox))) {
        ::sc::face_probe::keep_running();
    }
    if (!track || !awake) {
        end_face_track(now);
        step_gaze(sc, now);
        return;
    }

    // Frames count for the head only when it stood still while they were taken.
    if (now >= _next_move_poll) {
        _next_move_poll = now + kMovePollMs;
        if (sc.motion().isMoving()) {
            _head_still_since = 0;
        } else if (_head_still_since == 0) {
            _head_still_since = now;
        }
    }
    if (_face_check_pending && _head_still_since && now - _head_still_since >= kSettleMs) {
        _face_check_pending = false;
        ::sc::face_probe::request_check();  // look again right away instead of at the idle rate
    }

    ::sc::face_probe::Sighting s;
    if (::sc::face_probe::latest(s) && s.seq != _face_seq) {
        _face_seq = s.seq;
        if (static_cast<int32_t>(now - s.at) > static_cast<int32_t>(kMaxAgeMs)) {
            // from before this director (a conversation) or before a doze
        } else if (s.found) {
            on_face_sighting(sc, s.cx, s.cy, s.at, now);
        } else if (_face_turned && frame_still(s.at)) {
            _face_turned = false;  // the face left the view right after a turn: maybe the turn went the wrong way
            note_turn_result(true, now);
        }
    }

    if (_face_tracking && now - _face_seen_at > kLostMs) {
        _gaze_tx = _gaze_ty = 0;
    }
    if (_face_tracking && now - _face_seen_at > kReleaseMs) {
        end_face_track(now);
    }
    step_gaze(sc, now);
#else
    (void)sc;
    (void)now;
#endif
}

void IdleDirectorModifier::on_face_sighting(Modifiable& sc, float cx, float cy, uint32_t taken, uint32_t now)
{
    auto& mood = GetIdleMood();
    if (!_face_tracking) {
        const uint32_t last_seen = std::max(mood.lastFaceMs, mood.lastInteractionMs);
        mclog::tagInfo(_tag, "face found at ({:.2f}, {:.2f})", cx, cy);
        if ((last_seen == 0 || now - last_seen > kGreetAfterMs) && !_buddy.visible() && !_anim && !_touching) {
            // Someone came back: a glad face (or the happy emotion without the drawn faces).
            if (ScSettings::get().enabled(SettingId::Faces)) {
                show_temp_face(sc, FaceStyle::Joy, 2500, now);
            } else {
                set_emotion_for(sc, avatar::Emotion::Happy, 2000, now);
            }
            mclog::tagInfo(_tag, "hello again");
        }
    }
    _face_tracking   = true;
    _face_seen_at    = now;
    mood.lastFaceMs  = now;
    // Looking at someone: no idle glances or head behaviours meanwhile.
    _next_glance = std::max(_next_glance, now + 6000);
    _next_action = std::max(_next_action, now + 20000);

    // Positive: the face is right of / below the camera axis.
    const float ex = view_angle(cx, kHalfFovX);
    const float ey = view_angle(cy, kHalfFovY);
    // The screen faces the user: the robot's right is the screen's left.
    _gaze_tx = -static_cast<int>(std::lround(std::clamp(ex / kHalfFovX, -1.0f, 1.0f) * kGazeX));
    _gaze_ty = static_cast<int>(std::lround(std::clamp(ey / kHalfFovY, -1.0f, 1.0f) * kGazeY));

    if (!frame_still(taken)) {
        return;
    }
    // The first still frame after a turn: did the face get closer to the middle? A turn the wrong way (a camera or
    // servo mounted the other way round) would chase the face out of view.
    if (_face_turned) {
        _face_turned       = false;
        const bool wrong_x = std::fabs(_face_turn_ex) >= kDeadbandDeg && ex * _face_turn_ex > 0 &&
                             std::fabs(ex) > std::fabs(_face_turn_ex) + kWrongWayDeg;
        const bool wrong_y = std::fabs(_face_turn_ey) >= kDeadbandDeg && ey * _face_turn_ey > 0 &&
                             std::fabs(ey) > std::fabs(_face_turn_ey) + kWrongWayDeg;
        note_turn_result(wrong_x || wrong_y, now);
    }
    const bool head_off = _face_head_off_until && static_cast<int32_t>(now - _face_head_off_until) < 0;
    if (head_off || !face_head_allowed(sc) || (std::fabs(ex) < kDeadbandDeg && std::fabs(ey) < kDeadbandDeg)) {
        return;
    }
    // Servo angles in 0.1 degree: yaw > 0 turns to the robot's right (measured with the gyro: +20 degrees turned
    // clockwise seen from above), pitch > 0 is up.
    auto& motion    = sc.motion();
    const auto cur  = motion.getCurrentAngles();
    const int yaw   = step_within(cur.x, static_cast<int>(std::lround(ex * 10.0f * kHeadGain)), -kMaxYaw, kMaxYaw);
    const int pitch = step_within(cur.y, -static_cast<int>(std::lround(ey * 10.0f * kHeadGain)), kMinPitch, kMaxPitch);
    if (std::abs(yaw - cur.x) < 10 && std::abs(pitch - cur.y) < 10) {
        return;  // at a limit already
    }
    motion.moveWithSpeed(yaw, pitch, kHeadSpeed);
    _servo_busy_until   = std::max(_servo_busy_until, now + 1500);  // the IMU feels the servos: no pick-up meanwhile
    _head_still_since   = 0;
    _next_move_poll     = now + kMovePollMs;
    _face_check_pending = true;
    _face_turned        = true;
    _face_turn_ex       = ex;
    _face_turn_ey       = ey;
    // The head turns towards the face, so the eyes need less of the way.
    _gaze_tx = static_cast<int>(std::lround(_gaze_tx * (1.0f - kHeadGain)));
    _gaze_ty = static_cast<int>(std::lround(_gaze_ty * (1.0f - kHeadGain)));
    if (now >= _face_head_log_at) {
        _face_head_log_at = now + kHeadLogMs;
        mclog::tagInfo(_tag, "head ({}, {}) -> ({}, {}) for a face {:.1f} / {:.1f} degrees off", cur.x, cur.y, yaw,
                       pitch, ex, ey);
    }
}

bool IdleDirectorModifier::frame_still(uint32_t taken) const
{
    return _head_still_since && static_cast<int32_t>(taken - (_head_still_since + kSettleMs)) >= 0;
}

// Wrong-way turns in a row (the face further off, or gone, on the first still frame after a turn) pause head
// tracking; a turn that brought the face closer clears the count.
void IdleDirectorModifier::note_turn_result(bool wrong, uint32_t now)
{
    _face_wrong_way = wrong ? _face_wrong_way + 1 : 0;
    if (_face_wrong_way >= kWrongWayStrikes) {
        _face_wrong_way      = 0;
        _face_head_off_until = (now + kHeadOffMs) | 1;
        mclog::tagWarn(_tag,
                       "{} turns in a row left the face further off or out of view (last turn for {:.1f} / {:.1f} "
                       "degrees): head tracking paused for {} min",
                       kWrongWayStrikes, _face_turn_ex, _face_turn_ey, kHeadOffMs / 60000);
    }
}

bool IdleDirectorModifier::face_head_allowed(Modifiable& sc) const
{
    if (!ScSettings::get().enabled(SettingId::FaceTrackHead)) {
        return false;
    }
    // Suit transitions, nod-off, touches, being held or tilted and the wake-up stretch own the head.
    const bool head_busy = !_head_keys.empty() || _pending_move_at != 0 || _touching || _held || _tilted;
    const bool anim_busy = _anim && _anim_purpose != AnimPurpose::Idle;
    return !head_busy && !anim_busy && GetHAL().millis() >= _paused_until && !sc.motion().isModifyLocked();
}

void IdleDirectorModifier::end_face_track(uint32_t now)
{
    (void)now;
    if (_face_tracking) {
        mclog::tagInfo(_tag, "face gone");
    }
    _face_tracking      = false;
    _face_check_pending = false;
    _face_turned        = false;  // a face lost right after a turn was counted when that frame came
    _gaze_tx = _gaze_ty = 0;
}

void IdleDirectorModifier::step_gaze(Modifiable& sc, uint32_t now)
{
    const uint32_t dt = _gaze_step_at ? now - _gaze_step_at : 0;
    _gaze_step_at     = now;
    if (_anim) {
        return;  // an animation moves the eyes (by offsets, too); carry on afterwards
    }
    // Only the default eyes follow; drawn faces that bring their own eyes, the buddy and sleep get them back centred.
    const bool eyes_free = _phase == Phase::Awake && !_sleep_pose && !_buddy.visible() && _face.allowsGlances();
    const int tx         = eyes_free ? _gaze_tx : 0;
    const int ty         = eyes_free ? _gaze_ty : 0;
    if (tx == _gaze_x && ty == _gaze_y) {
        return;
    }
    const int step     = std::max(1, static_cast<int>(dt * kGazeSpeed));
    const auto applied = move_eyes(sc, approach(_gaze_x, tx, step) - _gaze_x, approach(_gaze_y, ty, step) - _gaze_y);
    _gaze_x += applied.x;
    _gaze_y += applied.y;
}

// Back to centred eyes at once: before an animation or a drawn face moves the eyes by its own offsets (on top of the
// gaze they could reach the +/-100 limit, and the part cut off would never be undone), and when the director goes.
void IdleDirectorModifier::drop_gaze(Modifiable& sc)
{
    const auto applied = move_eyes(sc, -_gaze_x, -_gaze_y);
    _gaze_x += applied.x;
    _gaze_y += applied.y;
}

// Relative moves, as BreathModifier and the face animations move the features by offsets too. Positions are clamped
// to +/-100, so the step is limited to what both eyes can take; returns the step taken.
uitk::Vector2i IdleDirectorModifier::move_eyes(Modifiable& sc, int dx, int dy)
{
    if ((dx == 0 && dy == 0) || !sc.hasAvatar()) {
        return {0, 0};
    }
    auto& left   = sc.avatar().leftEye();
    auto& right  = sc.avatar().rightEye();
    const auto l = left.getPosition();
    const auto r = right.getPosition();
    dx           = std::clamp(dx, -100 - std::min(l.x, r.x), 100 - std::max(l.x, r.x));
    dy           = std::clamp(dy, -100 - std::min(l.y, r.y), 100 - std::max(l.y, r.y));
    left.setPosition({l.x + dx, l.y + dy});
    right.setPosition({r.x + dx, r.y + dy});
    return {dx, dy};
}

}  // namespace stackchan::sc

#endif  // CONFIG_SC_IDLE_DIRECTOR

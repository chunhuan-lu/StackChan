/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
// Face tracking for the idle director (REQ-008). Every sighting is turned into a direction in the room: the head
// angle when the frame was taken (from a history of servo polls) plus the angle in the view. The head follows that
// direction continuously, a little ahead of someone moving; when the person is lost it turns on the way they went,
// looks back and to the other side, and for two minutes keeps facing where they were instead of looking around.
#include "idle_director.h"
#include "sc_settings.h"
#include <mooncake_log.h>
#include <sdkconfig.h>
#include <algorithm>
#include <cmath>

#if CONFIG_SC_IDLE_DIRECTOR

namespace stackchan::sc {

using ::sc::face_probe::kHalfFovX;
using ::sc::face_probe::kHalfFovY;
using ::sc::face_probe::Sighting;
using ::sc::face_probe::SightKind;

static const std::string_view _tag = "FaceTrack";

static constexpr float kPi = 3.14159265f;

// Servo angles are in 0.1 degree: yaw > 0 turns to the robot's right (measured with the gyro, 2026-10-22), pitch > 0
// is up.
static constexpr uint32_t kHeadPollMs   = 100;   // each poll reads both servos over the bus
static constexpr int kMovingDelta       = 6;     // changed more between two polls: the head moves
static constexpr uint32_t kSettleMs     = 150;   // still this long after a turn: ask for a detection
static constexpr uint32_t kMaxAgeMs     = 1500;  // older sightings (before a conversation or a doze) are skipped
static constexpr uint32_t kFollowMs     = 2500;  // placed this recently: following (detections can wait 2 s)
static constexpr uint32_t kPresenceMs   = 2 * 60 * 1000;  // someone is probably still here
static constexpr uint32_t kGreetAfterMs = 2 * 60 * 1000;
static constexpr uint32_t kLogMs        = 3000;

// Following
// Small offsets are for the eyes (they reach the edge of the view); the head follows bigger ones (setting
// "face_dead", up and down three quarters of it), so it keeps still while someone sits and types: detections and the
// tracked patch disagree by a few degrees.
static constexpr float kDeadbandPitchShare = 0.75f;
// A placement moves the person this much of the way to the new position, by "face_steady" (low, medium, high);
// detections are trusted more than the tracked patch.
static constexpr float kFaceWeight[]  = {0.9f, 0.6f, 0.35f};
static constexpr float kTrackWeight[] = {0.6f, 0.35f, 0.2f};
static constexpr float kRetarget     = 25;     // a new command when the target moved this much since the last one
static constexpr uint32_t kCmdGapMs  = 150;
static constexpr uint32_t kResendMs  = 800;    // same target, not there yet: command again after this
static constexpr float kLeadS        = 0.35f;  // turn this far ahead of someone moving ...
static constexpr float kMinLeadSpeed = 100;    // ... faster than this (0.1 degree per second): walking, not fidgeting
static constexpr float kMaxSpeed     = 400;    // speed estimates are clamped to this
static constexpr int kMaxYaw         = 700;    // +/-70 of the servo's +/-128
static constexpr int kMinPitch       = 150;    // within the 5-85 degree safety range
static constexpr int kMaxPitch       = 750;

// Searching
static constexpr float kCoastSpeed     = 80;    // lost while moving faster than this: turn on that way
static constexpr float kEdgeFrac       = 0.6f;  // or last seen this far out (of half the view)
static constexpr float kCoastMin       = 150;
static constexpr float kCoastMax       = 300;
static constexpr float kSweep          = 150;   // the other side
static constexpr uint32_t kDwellMs     = 1500;  // look this long at each search stop
static constexpr uint32_t kHoldMs      = 4000;  // lost in the middle: wait this long before "searched"
static constexpr uint32_t kMotionGapMs = 2500;
static constexpr float kMotionMinArea  = 0.003f;
static constexpr float kMotionMaxArea  = 0.3f;
static constexpr float kOwnMotionYaw   = 200;  // motion within this of where the face was, and below it, is their
static constexpr float kOwnMotionBelow = 50;   // own hands or body (typing)
static constexpr uint32_t kLookBackMs  = 5000;
static constexpr float kLookBackOff    = 100;  // pointing further than this from where someone was: turn back

// Eyes
static constexpr int kGazeX       = 70;  // eye offset for someone at the edge of the view
static constexpr int kGazeY       = 40;
static constexpr float kGazeSpeed = 0.25f;  // eye offset units per ms

// Angle between the camera axis and a point of the frame, in degrees; frac 0..1 across the frame.
static float view_angle(float frac, float half_fov)
{
    return std::atan((frac - 0.5f) * 2.0f * std::tan(half_fov * kPi / 180.0f)) * 180.0f / kPi;
}

static int approach(int value, int target, int step)
{
    return value < target ? std::min(value + step, target) : std::max(value - step, target);
}

// Servo speed for a turn of err (0.1 degree): calm for small corrections, quick for big jumps, up to "face_speed".
static int turn_speed(float err)
{
    const int fastest = setting(SettingId::FaceSpeed);
    return std::clamp(static_cast<int>(err / 10.0f * 25.0f), std::max(100, fastest * 36 / 100), fastest);
}

static const char* side(int dir)
{
    return dir > 0 ? "right" : "left";
}

void IdleDirectorModifier::update_face_track(Modifiable& sc, uint32_t now)
{
#if CONFIG_SC_FACE_PROBE
    auto& settings   = ScSettings::get();
    const bool track = settings.enabled(SettingId::FaceTrack);
    const bool awake = _phase == Phase::Awake && !_sleep_pose;
    if (!track || !awake) {
        if (awake && settings.enabled(SettingId::FaceBox)) {
            ::sc::face_probe::keep_running();
        }
        end_face_track(now);
        step_gaze(sc, now);
        return;
    }
    ::sc::face_probe::keep_running(!_face_tracking && person_present(now));  // lost: motion frames too
    poll_head(sc, now);
    if (_check_on_settle && _head_still_since && now - _head_still_since >= kSettleMs) {
        _check_on_settle = false;
        ::sc::face_probe::request_check();  // look right away instead of at the idle rate
    }

    Sighting s;
    if (::sc::face_probe::latest(s) && s.seq != _face_seq) {
        _face_seq = s.seq;
        if (static_cast<int32_t>(now - s.at) <= static_cast<int32_t>(kMaxAgeMs)) {
            on_sighting(sc, s, now);
        }
    }

    if (_face_tracking && static_cast<int32_t>(now - _person_at) > static_cast<int32_t>(kFollowMs)) {
        _face_tracking = false;
        _seek          = Seek::None;  // seek() decides what to do
        mclog::tagInfo(_tag, "lost (moving {:.0f} deg/s, last {:.1f} degrees off)", _person_vyaw / 10.0f, _person_ex);
    }
    if (_face_tracking) {
        follow(sc, now);
        // Eyes on the person while the head catches up.
        const auto& mood = GetIdleMood();
        const float ox   = (mood.faceYaw - _head_yaw) / 10.0f;      // degrees, > 0: to the robot's right
        const float oy   = (_head_pitch - mood.facePitch) / 10.0f;  // > 0: below
        // The screen faces the user: the robot's right is the screen's left.
        _gaze_tx = -static_cast<int>(std::lround(std::clamp(ox / kHalfFovX, -1.0f, 1.0f) * kGazeX));
        _gaze_ty = static_cast<int>(std::lround(std::clamp(oy / kHalfFovY, -1.0f, 1.0f) * kGazeY));
    } else {
        _gaze_tx = _gaze_ty = 0;
        if (person_present(now)) {
            seek(sc, now);
            look_back(sc, now);
        } else {
            _seek = Seek::None;
        }
    }
    step_gaze(sc, now);
#else
    (void)sc;
    (void)now;
#endif
}

// Servo angles every 100 ms: the history turns frame times into head angles, the probe gets them for the patch
// search and to tell motion from the picture moving with the head.
void IdleDirectorModifier::poll_head(Modifiable& sc, uint32_t now)
{
    if (now < _next_head_poll) {
        return;
    }
    _next_head_poll   = now + kHeadPollMs;
    const auto angles = sc.motion().getCurrentAngles();
    const bool moving = _head_len > 0 && (std::abs(angles.x - _head_yaw) > kMovingDelta ||
                                          std::abs(angles.y - _head_pitch) > kMovingDelta);
    _head_yaw             = angles.x;
    _head_pitch           = angles.y;
    _head_hist[_head_pos] = {now, static_cast<int16_t>(angles.x), static_cast<int16_t>(angles.y)};
    _head_pos             = static_cast<uint8_t>((_head_pos + 1) % _head_hist.size());
    _head_len             = static_cast<uint8_t>(std::min<size_t>(_head_len + 1, _head_hist.size()));
    if (moving || now - _last_cmd_at < 300) {  // a fresh command may not have started the servos yet
        _head_still_since = 0;
    } else if (_head_still_since == 0) {
        _head_still_since = now;
    }
    ::sc::face_probe::note_head(now, angles.x, angles.y, _head_still_since == 0);
}

bool IdleDirectorModifier::head_at(uint32_t t, float& yaw, float& pitch) const
{
    if (_head_len == 0) {
        return false;
    }
    const size_t cap       = _head_hist.size();
    const size_t oldest    = (_head_pos + cap - _head_len) % cap;
    const HeadSample* prev = nullptr;
    for (size_t k = 0; k < _head_len; ++k) {
        const HeadSample& h = _head_hist[(oldest + k) % cap];
        if (static_cast<int32_t>(h.at - t) >= 0) {
            if (!prev || h.at == prev->at) {
                yaw   = h.yaw;
                pitch = h.pitch;
            } else {
                const float f = static_cast<float>(t - prev->at) / static_cast<float>(h.at - prev->at);
                yaw           = prev->yaw + (h.yaw - prev->yaw) * f;
                pitch         = prev->pitch + (h.pitch - prev->pitch) * f;
            }
            return true;
        }
        prev = &h;
    }
    yaw   = prev->yaw;  // newer than the last poll
    pitch = prev->pitch;
    return true;
}

void IdleDirectorModifier::on_sighting(Modifiable& sc, const Sighting& s, uint32_t now)
{
    float head_yaw, head_pitch;
    if (s.kind == SightKind::None || !head_at(s.at, head_yaw, head_pitch)) {
        return;
    }
    // Positive: right of / below the camera axis.
    const float ex    = view_angle(s.cx, kHalfFovX);
    const float ey    = view_angle(s.cy, kHalfFovY);
    const float yaw   = head_yaw + ex * 10.0f;
    const float pitch = head_pitch - ey * 10.0f;
    if (s.kind == SightKind::Face || s.kind == SightKind::Track) {
        place_person(sc, s, yaw, pitch, ex, now);
        return;
    }
    // Motion while someone was here a moment ago and is not seen now: perhaps them, turn there and look. Not while
    // they were lost in the middle of the view (looking down, turned away: they are still there), and not just below
    // where their face was (their hands on the keyboard).
    if (_face_tracking || !person_present(now) || now < _next_motion_turn || s.size < kMotionMinArea ||
        s.size > kMotionMaxArea || _seek == Seek::Coast || _seek == Seek::Hold) {
        return;
    }
    const auto& mood = GetIdleMood();
    if (std::fabs(yaw - mood.faceYaw) < kOwnMotionYaw && pitch < mood.facePitch - kOwnMotionBelow) {
        return;
    }
    if (head_turn(sc, yaw, pitch, 250, now)) {
        _next_motion_turn = now + kMotionGapMs;
        _seek             = Seek::Hold;
        _seek_until       = now + kHoldMs;
        _check_on_settle  = true;
        mclog::tagInfo(_tag, "motion {:.1f} / {:.1f} degrees off ({:.1f} % of the view): turning there", ex, ey,
                       s.size * 100.0f);
    }
}

void IdleDirectorModifier::place_person(Modifiable& sc, const Sighting& s, float yaw, float pitch, float ex,
                                        uint32_t now)
{
    auto& mood = GetIdleMood();
    if (!_face_tracking) {
        if (s.kind != SightKind::Face) {
            return;  // a tracked patch only keeps following going, it does not start it
        }
        const uint32_t last_seen = std::max(mood.lastFaceMs, mood.lastInteractionMs);
        if (_seek != Seek::None && _person_at) {
            mclog::tagInfo(_tag, "found again after {} ms", static_cast<int32_t>(s.at - _person_at));
        } else {
            mclog::tagInfo(_tag, "face at ({:.2f}, {:.2f}): following", s.cx, s.cy);
        }
        if ((last_seen == 0 || now - last_seen > kGreetAfterMs) && !_buddy.visible() && !_anim && !_touching) {
            // Someone came back: a glad face (or the happy emotion without the drawn faces).
            if (ScSettings::get().enabled(SettingId::Faces)) {
                show_temp_face(sc, FaceStyle::Joy, 2500, now);
            } else {
                set_emotion_for(sc, avatar::Emotion::Happy, 2000, now);
            }
            mclog::tagInfo(_tag, "hello again");
        }
        _person_vyaw = _person_vpitch = 0;
    } else {
        // Smoothed position (detections jitter by a few degrees, the tracked patch drifts), and the speed from the
        // smoothed positions.
        const int steady   = std::clamp(setting(SettingId::FaceSteady), 0, 2);
        const float weight = s.kind == SightKind::Face ? kFaceWeight[steady] : kTrackWeight[steady];
        yaw                = mood.faceYaw + weight * (yaw - mood.faceYaw);
        pitch              = mood.facePitch + weight * (pitch - mood.facePitch);
        const float dt     = static_cast<int32_t>(s.at - _person_at) / 1000.0f;
        if (dt > 0.1f && dt < 1.5f) {
            _person_vyaw   = std::clamp(0.6f * _person_vyaw + 0.4f * (yaw - mood.faceYaw) / dt, -kMaxSpeed, kMaxSpeed);
            _person_vpitch = std::clamp(0.6f * _person_vpitch + 0.4f * (pitch - mood.facePitch) / dt, -kMaxSpeed,
                                        kMaxSpeed);
        }
    }
    mood.faceYaw    = yaw;
    mood.facePitch  = pitch;
    mood.lastFaceMs = now;
    _person_at      = s.at;
    _person_ex      = ex;
    _face_tracking  = true;
    _seek           = Seek::None;
    // Looking at someone: no idle glances or head behaviours meanwhile.
    _next_glance = std::max(_next_glance, now + 6000);
    _next_action = std::max(_next_action, now + 20000);
}

// Keep the head on the person (a little ahead when they move); the eyes cover what the head has not done yet.
void IdleDirectorModifier::follow(Modifiable& sc, uint32_t now)
{
    if (!face_head_allowed(sc)) {
        return;
    }
    const auto& mood = GetIdleMood();
    float yaw        = mood.faceYaw;
    float pitch      = mood.facePitch;
    if (std::fabs(_person_vyaw) > kMinLeadSpeed) {
        yaw += _person_vyaw * kLeadS;
    }
    if (std::fabs(_person_vpitch) > kMinLeadSpeed) {
        pitch += _person_vpitch * kLeadS;
    }
    yaw             = std::clamp(yaw, static_cast<float>(-kMaxYaw), static_cast<float>(kMaxYaw));
    pitch           = std::clamp(pitch, static_cast<float>(kMinPitch), static_cast<float>(kMaxPitch));
    const float dead_yaw = setting(SettingId::FaceDeadband) * 10.0f;
    const float err_yaw = std::fabs(yaw - _head_yaw), err_pitch = std::fabs(pitch - _head_pitch);
    if (err_yaw < dead_yaw && err_pitch < dead_yaw * kDeadbandPitchShare) {
        return;
    }
    const float err = std::max(err_yaw, err_pitch);
    const bool retarget = std::fabs(yaw - _cmd_yaw) > kRetarget || std::fabs(pitch - _cmd_pitch) > kRetarget;
    if (now - _last_cmd_at < kCmdGapMs || (!retarget && now - _last_cmd_at < kResendMs)) {
        return;
    }
    if (head_turn(sc, yaw, pitch, turn_speed(err), now) && now >= _face_log_at) {
        _face_log_at = now + kLogMs;
        mclog::tagInfo(_tag, "head ({}, {}) -> ({:.0f}, {:.0f}), the person moves {:.0f} / {:.0f} deg/s", _head_yaw,
                       _head_pitch, yaw, pitch, _person_vyaw / 10.0f, _person_vpitch / 10.0f);
    }
}

// Lost: on the way they went, back to the last place, the other side; or, lost in the middle of the view (turned
// away, covered), wait there. Each stop asks for a detection once the head has settled.
void IdleDirectorModifier::seek(Modifiable& sc, uint32_t now)
{
    const auto& mood = GetIdleMood();
    switch (_seek) {
        case Seek::None: {
            if (_person_at == 0 || !ScSettings::get().enabled(SettingId::FaceTrackHead)) {
                _seek = Seek::Done;  // nothing to search from (a new director): look_back() only
                break;
            }
            const bool moving = std::fabs(_person_vyaw) >= kCoastSpeed;
            const bool edge   = std::fabs(_person_ex) >= kEdgeFrac * kHalfFovX;
            if (!moving && !edge) {
                _seek       = Seek::Hold;
                _seek_until = now + kHoldMs;
                mclog::tagInfo(_tag, "lost in the middle of the view: waiting");
                break;
            }
            _seek_dir         = (moving ? _person_vyaw : _person_ex) > 0 ? 1 : -1;
            const float extra = std::clamp(std::fabs(_person_vyaw) * 0.8f, kCoastMin, kCoastMax);
            const float pitch = mood.facePitch + std::clamp(_person_vpitch * 0.5f, -100.0f, 100.0f);
            if (head_turn(sc, mood.faceYaw + _seek_dir * extra, pitch, 450, now)) {
                _seek            = Seek::Coast;
                _seek_until      = 0;
                _check_on_settle = true;
                mclog::tagInfo(_tag, "lost going {}: turning {:.0f} degrees further", side(_seek_dir), extra / 10.0f);
            } else {
                _seek       = Seek::Hold;
                _seek_until = now + kHoldMs;
            }
            break;
        }
        case Seek::Hold:
            if (now >= _seek_until) {
                _seek = Seek::Done;
            }
            break;
        case Seek::Coast:
        case Seek::Back:
        case Seek::Other: {
            if (_head_still_since == 0) {
                break;  // still turning
            }
            if (_seek_until == 0) {
                _seek_until = now + kDwellMs;
                break;
            }
            if (now < _seek_until || !face_head_allowed(sc)) {
                break;
            }
            const Seek next = _seek == Seek::Coast ? Seek::Back : _seek == Seek::Back ? Seek::Other : Seek::Done;
            const float yaw = next == Seek::Other ? mood.faceYaw - _seek_dir * kSweep : mood.faceYaw;
            if (head_turn(sc, yaw, mood.facePitch, 350, now)) {
                _seek            = next;
                _seek_until      = 0;
                _check_on_settle = true;
                mclog::tagInfo(_tag, "{}",
                               next == Seek::Back    ? "searching: back where they were"
                               : next == Seek::Other ? "searching: the other side"
                                                     : "searched: facing where they were");
            }
            break;
        }
        case Seek::Done:
            break;
    }
}

// Someone was here in the last two minutes: when something else turned the head away (a reaction, a conversation),
// turn back to where they were, now and then.
void IdleDirectorModifier::look_back(Modifiable& sc, uint32_t now)
{
    if ((_seek != Seek::Done && _seek != Seek::Hold) || now < _next_look_back || !face_head_allowed(sc)) {
        return;
    }
    const auto& mood = GetIdleMood();
    if (std::fabs(mood.faceYaw - _head_yaw) <= kLookBackOff && std::fabs(mood.facePitch - _head_pitch) <= kLookBackOff) {
        return;
    }
    if (head_turn(sc, mood.faceYaw, mood.facePitch, 200, now)) {
        _next_look_back  = now + kLookBackMs;
        _check_on_settle = true;
        mclog::tagInfo(_tag, "looking back to where someone was ({:.0f}, {:.0f})", mood.faceYaw, mood.facePitch);
    }
}

bool IdleDirectorModifier::head_turn(Modifiable& sc, float yaw, float pitch, int speed, uint32_t now)
{
    if (!face_head_allowed(sc)) {
        return false;
    }
    const int y = std::clamp(static_cast<int>(std::lround(yaw)), -kMaxYaw, kMaxYaw);
    const int p = std::clamp(static_cast<int>(std::lround(pitch)), kMinPitch, kMaxPitch);
    sc.motion().moveWithSpeed(y, p, speed);
    _cmd_yaw          = static_cast<float>(y);
    _cmd_pitch        = static_cast<float>(p);
    _last_cmd_at      = now;
    _head_still_since = 0;
    _servo_busy_until = std::max(_servo_busy_until, now + 1500);  // the IMU feels the servos: no pick-up meanwhile
    return true;
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

bool IdleDirectorModifier::person_present(uint32_t now) const
{
#if CONFIG_SC_FACE_PROBE
    const uint32_t at = GetIdleMood().lastFaceMs;
    return at != 0 && ScSettings::get().enabled(SettingId::FaceTrack) && now - at < kPresenceMs;
#else
    (void)now;
    return false;
#endif
}

void IdleDirectorModifier::end_face_track(uint32_t now)
{
    (void)now;
    if (_face_tracking) {
        mclog::tagInfo(_tag, "face tracking paused");
    }
    _face_tracking   = false;
    _seek            = Seek::None;
    _check_on_settle = false;
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

/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "idle_director.h"
#include "sc_settings.h"
#include "settings_panel.h"
#include "../avatar/decorators/decorators.h"
#include "../avatar/skins/default/default.h"
#include "../utils/random.h"
#include "application.h"
#include <assets/lang_config.h>
#include <mooncake_log.h>
#include <sdkconfig.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#if CONFIG_SC_IDLE_DIRECTOR

namespace stackchan::sc {

static const std::string_view _tag = "IdleDirector";

// Timing
static constexpr uint32_t kMoodIntervalMs   = 1000;
static constexpr uint32_t kClockIntervalMs  = 30000;
static constexpr uint32_t kLightIntervalMs  = 2000;
static constexpr uint32_t kImuIntervalMs    = 100;
static constexpr uint32_t kLedIntervalMs    = 100;
static constexpr uint32_t kLuxLogIntervalMs = 5 * 60 * 1000;
static constexpr uint32_t kSleepGraceMs     = 60 * 1000;   // stay awake this long after waking or talking
static constexpr uint32_t kDarkHoldMs       = 60 * 1000;   // darkness needed before falling asleep
static constexpr uint32_t kBrightHoldMs     = 5 * 1000;    // light needed before waking up
static constexpr uint32_t kNapMs            = 10 * 60 * 1000;
static constexpr uint32_t kStrokeRestoreMs  = 3600;        // HeadPetModifier restores after 3000 ms
static constexpr uint32_t kBacklightDarkMs  = 10 * 1000;   // darkness needed before dimming a sleeping face
static constexpr uint32_t kFaceHoldMs       = 3000;        // shortest time a face stays (no flicker)
static constexpr uint32_t kClaudeFaceMs     = 250;         // how often the Claude face is re-evaluated
static constexpr uint32_t kDoneFaceMs       = 20 * 1000;   // a finished session shows the "done" face this long
static constexpr uint32_t kErrFaceMs        = 60 * 1000;

// Touch
static constexpr uint32_t kTapMaxMs       = 450;
static constexpr uint32_t kTapWindowMs    = 2500;
static constexpr uint32_t kLongPressMs    = 2500;
static constexpr int kAnnoyedTaps         = 3;
static constexpr int kDizzyTaps           = 6;

// IMU (m/s^2)
static constexpr float kGravity         = 9.81f;
static constexpr float kStillEnergy     = 0.6f;
static constexpr float kPickupEnergy    = 2.5f;
static constexpr float kPickupMagDelta  = 2.2f;
static constexpr uint32_t kPickupHoldMs = 300;
static constexpr uint32_t kTouchQuietMs = 1500;  // first test: a tap burst was reported as a pick-up
static constexpr uint32_t kSetDownMs    = 2500;
static constexpr float kTiltOnDeg       = 22.0f;
static constexpr float kTiltOffDeg      = 12.0f;
static constexpr uint32_t kTiltHoldMs   = 1200;

// Poses (servo angles in 0.1 degree)
static constexpr int kSleepPitch = 60;
static constexpr int kAwakePitch = 200;
static constexpr int kSlowSpeed  = 60;

// Eyelid weight while sleeping (100 = open)
static constexpr int kSleepEyes = 6;

static constexpr int kMoodLeds[] = {1, 2, 3, 4, 5, 7, 8, 9, 10, 11};

// Screen taps arrive from the avatar's click handler; both sides run under the LVGL lock, the
// atomics only keep the hand-over explicit.
static std::atomic<bool> s_asleep{false};
static std::atomic<bool> s_screen_wake{false};

IdleMood& GetIdleMood()
{
    static IdleMood mood;
    return mood;
}

// Mirrors Pmic::SetBrightness (hal/board/stackchan.cc): DLDO1 code = 20 + b * 8 / 100, 0 = off.
static int backlight_step(int b)
{
    return b <= 0 ? -1 : std::min(b, 100) * 8 / 100;
}

// Lowest brightness value that selects a step: 1, 13, 25, 38, 50, 63, 75, 88, 100.
static int backlight_of_step(int step)
{
    return step < 0 ? 0 : std::max(1, (step * 100 + 7) / 8);
}

static int rnd(int a, int b)
{
    return Random::getInstance().getInt(a, b);
}

static float rndf(float a, float b)
{
    return Random::getInstance().getFloat(a, b);
}

static float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

static float head_moves_per_hour(int activity)
{
    static constexpr float kScale[] = {0.0f, 0.5f, 1.0f, 2.0f};
    return setting(SettingId::HeadMoves) * kScale[std::clamp(activity, 0, 3)];
}

static bool faces_on()
{
    return ScSettings::get().enabled(SettingId::Faces);
}

static bool claude_on()
{
    return ScSettings::get().enabled(SettingId::Claude);
}

// Next special idle face, minutes from the settings (the first one comes after half the time).
static uint32_t idle_face_delay(bool first)
{
    const int scale = first ? 30000 : 60000;
    // The pair is read without a lock: never hand rnd() a reversed range.
    const int a = setting(SettingId::FaceMinMinutes);
    const int b = setting(SettingId::FaceMaxMinutes);
    return rnd(std::min(a, b) * scale, std::max(a, b) * scale);
}

static float clock_energy(int hour)
{
    if (hour < 0) return 0.7f;
    if (hour >= 22) return 0.35f;
    if (hour >= 19) return 0.6f;
    if (hour >= 15) return 0.75f;
    if (hour >= 13) return 0.5f;  // post-lunch dip
    if (hour >= 12) return 0.75f;
    return 0.9f;
}

/* ---------------------------------------------------------------------------------------------- */
/*                                          Construction                                          */
/* ---------------------------------------------------------------------------------------------- */

IdleDirectorModifier::IdleDirectorModifier(int activityLevel) : _activity(std::clamp(activityLevel, 0, 3))
{
    _claude_written.fill(kNoColor);
    s_asleep      = false;
    s_screen_wake = false;  // a tap left over from a previous director must not wake this one
    _touch_conn = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture gesture) {
        std::lock_guard<std::mutex> lock(_event_mutex);
        if (_events.size() < 16) {
            _events.push_back({gesture, GetHAL().millis()});
        }
    });
    _imu_conn = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent event) {
        if (event == ImuMotionEvent::Shake) {
            _shake_event = true;
        }
    });
}

IdleDirectorModifier::~IdleDirectorModifier()
{
    GetHAL().onHeadPetGesture.disconnect(_touch_conn);
    GetHAL().onImuMotionEvent.disconnect(_imu_conn);

    // Leaving standby means a conversation (or the power-save sleep) started: count it as interaction.
    auto& mood             = GetIdleMood();
    mood.lastInteractionMs = GetHAL().millis();
    mood.boredom           = 0.0f;

    if (_sc && _sc->hasAvatar()) {
        auto& av = _sc->avatar();
        _buddy.hide(av);
        _face.clear(av);  // conversations use the default features (the speaking mouth)
        if (_anim) {
            _anim->end(av);
            _anim.reset();
        }
        if (_sweat_id >= 0) {
            av.removeDecorator(_sweat_id);
        }
        av.leftEye().setSize(0);
        av.rightEye().setSize(0);
        av.mouth().setWeight(0);
        av.mouth().setRotation(0);

        // The power-save path sets "sleepy" and its own "Zzz" just before removing us: keep those.
        const bool power_save_sleep = av.getEmotion() == avatar::Emotion::Sleepy && !_sleep_pose;
        if (_bubble_active && !power_save_sleep) {
            av.clearSpeech();
        }
        av.setEmotion(av.getEmotion());  // re-sync eyelids with the current emotion
        if (_locked_by_us) {
            av.setModifyLock(false);
        }
    }

    s_asleep      = false;
    s_screen_wake = false;
    restore_backlight();
    _led_written[0] = _led_written[1] = _led_written[2] = -1;  // the session LEDs may be lit: force the clear
    write_led(0, 0, 0);
    mclog::tagInfo(_tag, "stopped");
}

bool IdleDirectorModifier::requestScreenWake()
{
    if (!s_asleep || !ScSettings::get().enabled(SettingId::TapWake)) {
        return false;
    }
    s_screen_wake = true;
    return true;
}

/* ---------------------------------------------------------------------------------------------- */
/*                                           Main loop                                            */
/* ---------------------------------------------------------------------------------------------- */

void IdleDirectorModifier::_update(Modifiable& sc)
{
    if (!sc.hasAvatar()) {
        return;
    }
    _sc                = &sc;
    const uint32_t now = GetHAL().millis();

    if (!_started) {
        _started     = true;
        _created     = now;
        _awake_since = now;
        auto& mood   = GetIdleMood();
        if (mood.headTokens < 0) {
            mood.headTokens = 1.0f;
        }
        _next_glance    = now + rnd(4000, 9000);
        _next_action    = now + rnd(30000, 60000);
        _next_idle_face = now + idle_face_delay(true);
        _activity       = setting(SettingId::Activity);
        poll_clock(now);
        mclog::tagInfo(_tag, "started: activity {}, {} head moves/h, sleep {}:00-{}:00", _activity,
                       head_moves_per_hour(_activity), setting(SettingId::SleepHour), setting(SettingId::WakeHour));
    }

    handle_touch_events(sc, now);
    if (s_screen_wake.exchange(false) && _phase == Phase::Sleeping) {
        // Tapping the dim sleeping face used to start a conversation instead (device test 2026-10-21).
        mclog::tagInfo(_tag, "screen tap while asleep");
        note_interaction(now);
        wake_up(sc, now, false);
    }
#if CONFIG_SC_SETTINGS_PANEL
    if (settings_panel_open()) {
        // Someone is changing settings: stay awake (and with a normal screen) meanwhile.
        GetIdleMood().lastInteractionMs = now;
        if (_phase == Phase::Sleeping) {
            mclog::tagInfo(_tag, "settings panel opened while asleep");
            wake_up(sc, now, false);
        }
    }
#endif
    ClaudeEvent claude_event;
    while (GetClaudeStatus().popEvent(claude_event, now)) {
        if (claude_on()) {
            on_claude_event(sc, claude_event, now);
        }
    }
    if (_shake_event.exchange(false)) {
        on_shake(sc, now);
    }
    if (now >= _next_imu) {
        _next_imu = now + kImuIntervalMs;
        poll_imu(sc, now);
    }
    if (now >= _next_light) {
        _next_light = now + kLightIntervalMs;
        poll_light(now);
    }
    if (now >= _next_clock) {
        _next_clock = now + kClockIntervalMs;
        poll_clock(now);
    }
    if (now >= _next_mood) {
        _next_mood = now + kMoodIntervalMs;
        update_mood(now);
    }

    if (_anim && !_anim->step(sc.avatar(), now)) {
        finish_anim(sc, now);
    }

    if (_emotion_restore_at && now >= _emotion_restore_at) {
        _emotion_restore_at = 0;
        if (_phase == Phase::Awake && !_tilted) {
            sc.avatar().setEmotion(avatar::Emotion::Neutral);
        }
    }

    if (_pending_move_at && now >= _pending_move_at) {
        _pending_move_at = 0;
        try_head_move(sc, now, INT32_MIN, _pending_pitch, _pending_speed, false);
    }

    update_sleep_state(sc, now);
    if (_phase == Phase::Awake) {
        update_awake(sc, now);
    } else {
        update_sleeping(sc, now);
    }

    if (_bubble_active && now >= _bubble_until) {
        _bubble_active = false;
        sc.avatar().clearSpeech();
    }

    if (_sleep_pose) {
        _bl_target = sleep_backlight();  // follows the room light while asleep
    }
    update_backlight(now);
    update_suit(sc, now);
    run_head_keys(sc, now);
    update_face(sc, now);

    if (now >= _next_led) {
        _next_led = now + kLedIntervalMs;
        update_led(now);
    }

    sync_lock(sc);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                             Touch                                              */
/* ---------------------------------------------------------------------------------------------- */

void IdleDirectorModifier::handle_touch_events(Modifiable& sc, uint32_t now)
{
    std::vector<TouchEvent> events;
    {
        std::lock_guard<std::mutex> lock(_event_mutex);
        events.swap(_events);
    }

    for (const auto& e : events) {
        _touch_quiet_until = now + kTouchQuietMs;
        switch (e.gesture) {
            case HeadPetGesture::Press:
                _press_ms = e.ms;
                on_press(sc, now);
                break;
            case HeadPetGesture::SwipeForward:
            case HeadPetGesture::SwipeBackward:
                on_swipe(now);
                break;
            case HeadPetGesture::Release:
                on_release(sc, now, e.ms);
                break;
            default:
                break;
        }
    }

    if (_touching && !_swiped && !_long_press_fired && now - _press_ms >= kLongPressMs) {
        _long_press_fired = true;
        on_long_press(sc, now);
    }
}

void IdleDirectorModifier::on_press(Modifiable& sc, uint32_t now)
{
    _touching         = true;
    _swiped           = false;
    _long_press_fired = false;
    note_interaction(now);

    if (_phase == Phase::Sleeping) {
        if (_sleep_reason == SleepReason::Nap) {
            wake_up(sc, now, false);
        } else if (!_anim) {
            play_keys(sc, anim::sleepPeek(), AnimPurpose::Reaction, now);
        }
        return;
    }

    // Make room for HeadPetModifier and the tap reactions.
    if (_anim && _anim_purpose == AnimPurpose::Idle) {
        stop_anim(sc);
    }
}

void IdleDirectorModifier::on_swipe(uint32_t now)
{
    _swiped = true;
    ++_strokes;
    _servo_busy_until = now + 2500;  // HeadPetModifier moves the head
    _paused_until     = now + kStrokeRestoreMs;

    auto& mood     = GetIdleMood();
    mood.valence   = clamp01(mood.valence + 0.04f);
    mood.affection = clamp01(mood.affection + 0.01f);
}

void IdleDirectorModifier::on_release(Modifiable& sc, uint32_t now, uint32_t ms)
{
    _touching = false;

    if (!_swiped && ms - _press_ms <= kTapMaxMs) {
        _taps.push_back(ms);
        on_taps(sc, now);
    }

    if (_swiped) {
        if (_phase == Phase::Sleeping) {
            _resleep_pose_at = now + kStrokeRestoreMs;  // smiled in its sleep, close the eyes again
        } else if (_strokes >= 2) {
            _afterglow_at = now + kStrokeRestoreMs;
        }
        _strokes = 0;
    }
}

void IdleDirectorModifier::on_taps(Modifiable& sc, uint32_t now)
{
    _taps.erase(std::remove_if(_taps.begin(), _taps.end(), [now](uint32_t t) { return now - t > kTapWindowMs; }),
                _taps.end());
    if (_phase == Phase::Sleeping) {
        return;  // on_press already handled the peek / wake-up
    }

    auto& av   = sc.avatar();
    auto& mood = GetIdleMood();
    const int count = static_cast<int>(_taps.size());

    if (count >= kDizzyTaps) {
        mclog::tagInfo(_tag, "taps x{}: dizzy", count);
        stop_anim(sc);
        av.addDecorator(std::make_unique<avatar::DizzyDecorator>(lv_screen_active(), 2500, 300));
        set_emotion_for(sc, avatar::Emotion::Doubt, 2500, now);
        set_led_fx(LedFx::Pulse, 20, 0, 20, 2500, now);
        mood.valence  = clamp01(mood.valence - 0.1f);
        _paused_until = now + 2500;
        _taps.clear();
    } else if (count >= kAnnoyedTaps && now >= _annoyed_until) {
        mclog::tagInfo(_tag, "taps x{}: annoyed", count);
        stop_anim(sc);
        av.addDecorator(std::make_unique<avatar::AngryDecorator>(lv_screen_active(), 2500, 500));
        set_emotion_for(sc, avatar::Emotion::Angry, 2500, now);
        set_led_fx(LedFx::Pulse, 24, 0, 0, 2500, now);
        mood.valence   = clamp01(mood.valence - 0.08f);
        _annoyed_until = now + 2500;
        _paused_until  = now + 2500;
    }
}

void IdleDirectorModifier::on_long_press(Modifiable& sc, uint32_t now)
{
    if (_phase != Phase::Awake) {
        return;
    }
    mclog::tagInfo(_tag, "long press: lulled to sleep");
    begin_doze(sc, now, SleepReason::Nap);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                              IMU                                               */
/* ---------------------------------------------------------------------------------------------- */

void IdleDirectorModifier::on_shake(Modifiable& sc, uint32_t now)
{
    // ImuEventModifier plays the dizzy reaction; step aside and remember it.
    mclog::tagInfo(_tag, "shaken");
    note_interaction(now);
    stop_anim(sc);
    _face.clear(sc.avatar());  // ImuEventModifier hides and restores the default eyes itself
    _temp_face = FaceStyle::None;
    if (_phase == Phase::Sleeping) {
        leave_sleep_pose(sc);
        _phase       = Phase::Awake;
        _awake_since = now;
    }
    _locked_by_us = false;  // ImuEventModifier owns the lock during its reaction
    auto& mood    = GetIdleMood();
    mood.valence  = clamp01(mood.valence - 0.15f);
    _paused_until = now + 4500;
    _servo_busy_until = now + 4500;
}

void IdleDirectorModifier::poll_imu(Modifiable& sc, uint32_t now)
{
    float a[3];
    if (!GetHAL().getImuAccel(a[0], a[1], a[2])) {
        return;
    }
    if (!_imu_init) {
        _imu_init = true;
        std::copy(a, a + 3, _g);
        std::copy(a, a + 3, _prev);
        return;
    }

    const float diff = std::fabs(a[0] - _prev[0]) + std::fabs(a[1] - _prev[1]) + std::fabs(a[2] - _prev[2]);
    std::copy(a, a + 3, _prev);
    _motion_energy = 0.7f * _motion_energy + 0.3f * diff;
    for (int i = 0; i < 3; ++i) {
        _g[i] = 0.85f * _g[i] + 0.15f * a[i];
    }
    const float mag   = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const bool still  = _motion_energy < kStillEnergy && std::fabs(mag - kGravity) < 0.8f;
    _stable_since     = still ? (_stable_since ? _stable_since : now) : 0;

    if (now >= _next_imu_log) {
        // Raw axes for calibrating head-levelling later (see REQ-002).
        _next_imu_log = now + 60000;
        mclog::tagInfo(_tag, "imu g=({:.2f},{:.2f},{:.2f}) energy={:.2f}", _g[0], _g[1], _g[2], _motion_energy);
    }

    // The IMU sits in the head: ignore our own servo motion and re-learn "level" afterwards.
    if (now < _servo_busy_until) {
        _base_valid   = false;
        _pickup_since = 0;
        return;
    }

    if (_stable_since && now - _stable_since > 3000 && !_held && !_tilted) {
        if (!_base_valid) {
            std::copy(_g, _g + 3, _base);
            _base_valid = true;
        } else {
            for (int i = 0; i < 3; ++i) {
                _base[i] = 0.98f * _base[i] + 0.02f * _g[i];
            }
        }
    }

    if (!_held) {
        if (_touching || now < _touch_quiet_until) {
            _pickup_since = 0;
            return;
        }
        const bool jolt = _motion_energy > kPickupEnergy || std::fabs(mag - kGravity) > kPickupMagDelta;
        _pickup_since   = jolt ? (_pickup_since ? _pickup_since : now) : 0;
        if (_pickup_since && now - _pickup_since >= kPickupHoldMs) {
            on_pickup(sc, now);
        }
    } else {
        _held_still_since = still ? (_held_still_since ? _held_still_since : now) : 0;
        if (_held_still_since && now - _held_still_since >= kSetDownMs) {
            on_set_down(sc, now);
        }
        return;
    }

    if (!_base_valid || _phase == Phase::Sleeping) {
        return;  // tilt is ignored while asleep; being picked up still wakes it
    }
    const float dot  = _g[0] * _base[0] + _g[1] * _base[1] + _g[2] * _base[2];
    const float norm = std::sqrt((_g[0] * _g[0] + _g[1] * _g[1] + _g[2] * _g[2]) *
                                 (_base[0] * _base[0] + _base[1] * _base[1] + _base[2] * _base[2]));
    if (norm <= 0.0f) {
        return;
    }
    const float angle = std::acos(std::clamp(dot / norm, -1.0f, 1.0f)) * 57.2958f;

    if (!_tilted) {
        _tilt_since = angle > kTiltOnDeg ? (_tilt_since ? _tilt_since : now) : 0;
        if (_tilt_since && now - _tilt_since >= kTiltHoldMs) {
            on_tilt(sc, angle);
        }
    } else {
        _untilt_since = angle < kTiltOffDeg ? (_untilt_since ? _untilt_since : now) : 0;
        if (_untilt_since && now - _untilt_since >= 1000) {
            on_untilt(sc);
        }
    }
}

void IdleDirectorModifier::on_pickup(Modifiable& sc, uint32_t now)
{
    mclog::tagInfo(_tag, "picked up (energy {:.2f})", _motion_energy);
    _held             = true;
    _held_still_since = 0;
    _next_held_look   = now + 3000;
    note_interaction(now);

    if (_tilted) {
        on_untilt(sc);
    }
    if (_phase == Phase::Sleeping) {
        leave_sleep_pose(sc);
        _phase       = Phase::Awake;
        _awake_since = now;
        sc.avatar().setEmotion(avatar::Emotion::Neutral);
    }
    stop_anim(sc);
    play_keys(sc, anim::startled(), AnimPurpose::Reaction, now);
    show_bubble(sc, "?!", 1500, now);
    set_led_fx(LedFx::Pulse, 24, 14, 0, 2000, now);
    GetIdleMood().valence = clamp01(GetIdleMood().valence - 0.03f);
}

void IdleDirectorModifier::on_set_down(Modifiable& sc, uint32_t now)
{
    mclog::tagInfo(_tag, "set down");
    _held       = false;
    _base_valid = false;  // orientation may have changed
    stop_anim(sc);
    sc.avatar().addDecorator(std::make_unique<avatar::SweatDecorator>(lv_screen_active(), 1500, 700));
    set_emotion_for(sc, avatar::Emotion::Happy, 1500, now);
    set_led_fx(LedFx::Pulse, 0, 20, 6, 1500, now);
    GetIdleMood().valence = clamp01(GetIdleMood().valence + 0.05f);
}

void IdleDirectorModifier::on_tilt(Modifiable& sc, float angle)
{
    mclog::tagInfo(_tag, "tilted {:.0f} deg", angle);
    _tilted       = true;
    _untilt_since = 0;
    stop_anim(sc);
    sc.avatar().setEmotion(avatar::Emotion::Doubt);
    if (_sweat_id < 0) {
        _sweat_id = sc.avatar().addDecorator(std::make_unique<avatar::SweatDecorator>(lv_screen_active(), 0, 700));
    }
}

void IdleDirectorModifier::on_untilt(Modifiable& sc)
{
    mclog::tagInfo(_tag, "level again");
    _tilted     = false;
    _tilt_since = 0;
    if (_sweat_id >= 0) {
        sc.avatar().removeDecorator(_sweat_id);
        _sweat_id = -1;
    }
    if (_phase == Phase::Awake) {
        sc.avatar().setEmotion(avatar::Emotion::Neutral);
    }
}

/* ---------------------------------------------------------------------------------------------- */
/*                                       Light, clock, mood                                       */
/* ---------------------------------------------------------------------------------------------- */

void IdleDirectorModifier::poll_light(uint32_t now)
{
    float lux = 0.0f;
    if (!GetHAL().getAmbientLux(lux)) {
        _dark = false;
        return;
    }
    const int dark_lux   = setting(SettingId::DarkLux);
    const int bright_lux = setting(SettingId::BrightLux);
    if (_lux < 0.0f) {
        _bl_lit = lux > dark_lux;  // first sample
    }
    _lux = lux;

    if (lux < dark_lux) {
        _bright_since = 0;
        _dark_since   = _dark_since ? _dark_since : now;
        if (!_dark && now - _dark_since >= kDarkHoldMs) {
            _dark = true;
            mclog::tagInfo(_tag, "dark ({:.1f} lux)", lux);
        }
    } else if (lux > bright_lux) {
        _dark_since   = 0;
        _bright_since = _bright_since ? _bright_since : now;
        if (_dark && now - _bright_since >= kBrightHoldMs) {
            _dark          = false;
            _light_on_edge = true;
            mclog::tagInfo(_tag, "light on ({:.1f} lux)", lux);
        }
    } else {
        _dark_since = _bright_since = 0;  // inside the hysteresis band: keep the state
    }

    // Sleep backlight: brighten as soon as the room is lit, dim after a shorter darkness hold than
    // the one that sends the robot to sleep.
    if (lux > bright_lux) {
        _bl_lit = true;
    } else if (_dark_since && now - _dark_since >= kBacklightDarkMs) {
        _bl_lit = false;
    }

    if (now >= _next_lux_log) {
        _next_lux_log = now + kLuxLogIntervalMs;
        mclog::tagInfo(_tag, "ambient {:.1f} lux ({})", lux, _dark ? "dark" : "light");
    }
}

void IdleDirectorModifier::poll_clock(uint32_t now)
{
    bool valid = false;
    int hour   = -1;
    if (GetHAL().isTimeSynced()) {
        time_t t = time(nullptr);
        struct tm local;
        localtime_r(&t, &local);
        if (local.tm_year + 1900 >= 2025) {
            valid = true;
            hour  = local.tm_hour;
        }
    }
    if (valid != _time_valid || hour != _hour) {
        mclog::tagInfo(_tag, "local hour {} ({})", hour, valid ? "synced" : "not synced, schedule off");
    }
    _time_valid = valid;
    _hour       = hour;
}

bool IdleDirectorModifier::is_night() const
{
    if (!_time_valid) {
        return false;
    }
    const int wake  = setting(SettingId::WakeHour);
    const int sleep = setting(SettingId::SleepHour);
    if (sleep > wake) {
        return _hour >= sleep || _hour < wake;
    }
    return _hour >= sleep && _hour < wake;
}

void IdleDirectorModifier::update_mood(uint32_t now)
{
    auto& mood     = GetIdleMood();
    const float dt = kMoodIntervalMs / 1000.0f;

    if (_phase == Phase::Awake) {
        mood.boredom = clamp01(mood.boredom + dt / 2700.0f);  // 45 min to full boredom
    } else {
        mood.boredom = clamp01(mood.boredom - dt / 600.0f);
    }
    mood.valence += (0.5f - mood.valence) * dt / 1800.0f;
    mood.affection += (0.3f - mood.affection) * dt / 86400.0f;
    mood.energy = clock_energy(_time_valid ? _hour : -1);

    _activity        = setting(SettingId::Activity);  // the settings panel and voice change it live
    const float rate = head_moves_per_hour(_activity);
    const float cap  = std::max(1.0f, rate / 4.0f);
    mood.headTokens  = std::min(cap, mood.headTokens + rate / 3600.0f * dt);
}

void IdleDirectorModifier::note_interaction(uint32_t now)
{
    auto& mood             = GetIdleMood();
    mood.lastInteractionMs = now;
    mood.boredom           = std::max(0.0f, mood.boredom - 0.5f);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                            Sleeping                                            */
/* ---------------------------------------------------------------------------------------------- */

void IdleDirectorModifier::update_sleep_state(Modifiable& sc, uint32_t now)
{
    const bool night        = is_night();
    const bool should_sleep = night || _dark;
    auto& mood              = GetIdleMood();

    if (_phase == Phase::Awake) {
        _light_on_edge = false;  // only meaningful while asleep
        if (_anim && _anim_purpose == AnimPurpose::DozeOff) {
            return;
        }
        const bool quiet = now - mood.lastInteractionMs > kSleepGraceMs && now - _awake_since > kSleepGraceMs &&
                           !_touching && !_held && !_tilted && now >= _paused_until && !_buddy.visible();
        if (!quiet) {
            return;
        }
        if (should_sleep) {
            begin_doze(sc, now, night ? SleepReason::Night : SleepReason::Dark);
        } else if (mood.boredom >= 0.92f && !_anim) {
            begin_doze(sc, now, SleepReason::Nap);  // bored stiff: take a nap
        }
        return;
    }

    // Sleeping
    if (_sleep_reason == SleepReason::Nap) {
        if (now >= _nap_until) {
            if (should_sleep) {
                _sleep_reason = night ? SleepReason::Night : SleepReason::Dark;
            } else {
                wake_up(sc, now, false);
            }
        }
        return;
    }

    if (!should_sleep) {
        wake_up(sc, now, _sleep_reason == SleepReason::Night);
    } else if (night && _light_on_edge && !_anim) {
        _light_on_edge = false;
        play_keys(sc, anim::sleepPeek(), AnimPurpose::Reaction, now);  // light at night: grumpy peek
    }
}

void IdleDirectorModifier::begin_doze(Modifiable& sc, uint32_t now, SleepReason reason)
{
    static constexpr const char* kNames[] = {"none", "night", "dark", "nap"};
    mclog::tagInfo(_tag, "dozing off ({})", kNames[static_cast<int>(reason)]);
    _doze_reason = reason;
    stop_anim(sc);
    play_keys(sc, anim::dozeOff(), AnimPurpose::DozeOff, now);
    try_head_move(sc, now, INT32_MIN, kSleepPitch, kSlowSpeed, false);
}

void IdleDirectorModifier::apply_sleep_pose(Modifiable& sc, uint32_t now)
{
    auto& av = sc.avatar();
    _face.clear(av);  // the sleep pose uses the default eyes and their eyelids
    _temp_face = FaceStyle::None;
    av.setEmotion(avatar::Emotion::Sleepy);
    av.leftEye().setWeight(kSleepEyes);
    av.rightEye().setWeight(kSleepEyes);
    av.mouth().setWeight(0);

    if (!_sleep_pose) {
        _sleep_pose = true;
        if (_saved_backlight < 0) {
            _saved_backlight = GetHAL().getBackLightBrightness();
        }
        s_asleep = _phase == Phase::Sleeping;
    }
    _bl_target = sleep_backlight();  // update_backlight() fades towards it
}

void IdleDirectorModifier::leave_sleep_pose(Modifiable& sc)
{
    _sleep_pose      = false;
    _resleep_pose_at = 0;
    s_asleep         = false;  // every way out of sleep (wake-up, pick-up, shake) passes here
    restore_backlight();
    if (_bubble_active) {
        _bubble_active = false;
        sc.avatar().clearSpeech();
    }
}

void IdleDirectorModifier::wake_up(Modifiable& sc, uint32_t now, bool morning)
{
    mclog::tagInfo(_tag, "{}", morning ? "good morning" : "waking up");
    stop_anim(sc);
    leave_sleep_pose(sc);
    _phase        = Phase::Awake;
    _sleep_reason = SleepReason::None;
    _awake_since  = now;
    _light_on_edge = false;

    sc.avatar().setEmotion(avatar::Emotion::Neutral);
    play_keys(sc, anim::wakeUp(), AnimPurpose::WakeUp, now);
    // A morning stretch lifts the head a bit higher before settling.
    try_head_move(sc, now, INT32_MIN, morning ? kAwakePitch + 150 : kAwakePitch, kSlowSpeed, false);
    if (morning) {
        _pending_move_at = now + 4500;
        _pending_pitch   = kAwakePitch;
        _pending_speed   = kSlowSpeed;
    }
    _next_glance = now + rnd(6000, 12000);
    _next_action = now + rnd(40000, 80000);
}

void IdleDirectorModifier::update_sleeping(Modifiable& sc, uint32_t now)
{
    if (_resleep_pose_at && now >= _resleep_pose_at) {
        _resleep_pose_at = 0;
        apply_sleep_pose(sc, now);
    }
    if (_anim) {
        return;
    }
    if (now >= _next_zzz && !_bubble_active) {
        show_bubble(sc, "Zzz", 4000, now);
        _next_zzz = now + rnd(40000, 90000);
    }
    if (now >= _next_dream) {
        // Mumbling in its sleep: a tiny mouth twitch, silently.
        _next_dream = now + rnd(180000, 360000);
        play_keys(sc, {key(0), key(300).mouthOpen(12), key(700).mouthOpen(4), key(1100).mouthOpen(10), key(1600)},
                  AnimPurpose::Reaction, now);
        show_bubble(sc, "...", 2000, now);
    }
}

/* ---------------------------------------------------------------------------------------------- */
/*                                             Awake                                              */
/* ---------------------------------------------------------------------------------------------- */

void IdleDirectorModifier::update_awake(Modifiable& sc, uint32_t now)
{
    if (_buddy.visible()) {
        _afterglow_at = 0;  // the buddy has its own faces
        return;
    }
    if (now < _paused_until || _touching) {
        return;
    }

    if (_afterglow_at && now >= _afterglow_at) {
        _afterglow_at = 0;
        mclog::tagInfo(_tag, "stroke afterglow");
        if (faces_on()) {
            show_temp_face(sc, FaceStyle::Blush, 5000, now);
        } else {
            play_keys(sc, anim::contentSquint(), AnimPurpose::Reaction, now);
        }
        set_led_fx(LedFx::Heartbeat, 24, 4, 10, 5000, now);
        return;
    }

    if (_anim || !_face.allowsGlances()) {
        return;  // a special or Claude face owns the eyes
    }

    if (_held) {
        if (now >= _next_held_look) {
            _next_held_look = now + rnd(2500, 4000);
            play_keys(sc, anim::lookAround(2200), AnimPurpose::Reaction, now);
        }
        return;
    }
    if (_tilted) {
        return;
    }

    const float boredom = GetIdleMood().boredom;
    if (now >= _next_glance) {
        const int x = rnd(30, 75) * (rnd(0, 1) ? 1 : -1);
        const int y = rnd(-40, 40);
        play_keys(sc, anim::glance(x, y, rnd(1200, 3000)), AnimPurpose::Idle, now);
        const float scale = (1.4f - 0.6f * boredom) * (_activity == 0 ? 1.2f : 1.0f);
        _next_glance      = now + static_cast<uint32_t>(rnd(6000, 15000) * scale);
        return;
    }

    if (now >= _next_action) {
        run_behavior(sc, now);
        static constexpr float kActionScale[] = {1.5f, 1.3f, 1.0f, 0.7f};
        _next_action = now + static_cast<uint32_t>(rnd(45000, 120000) * kActionScale[_activity]);
    }
}

float IdleDirectorModifier::behavior_weight(int behavior) const
{
    const auto& m     = GetIdleMood();
    const bool tokens = _activity > 0 && m.headTokens >= 1.0f;
    const bool afternoon = _time_valid && _hour >= 13 && _hour < 16;

    switch (behavior) {
        case kHeadLook:
            return tokens ? 2.0f + 3.0f * m.boredom : 0.0f;
        case kYawn:
            return 2.5f * (1.0f - m.energy);
        case kSideEye:
            return 0.8f;
        case kSigh:
            return 1.5f * m.boredom;
        case kBugChase:
            return 3.0f * std::max(0.0f, m.boredom - 0.35f);
        case kSneeze:
        case kHiccup:
            return 0.2f;
        case kHum:
            return 2.0f * std::max(0.0f, m.valence - 0.6f);
        case kNodOff:
            return afternoon && m.energy < 0.6f ? 0.5f + 3.0f * (0.6f - m.energy) : 0.0f;
        case kLookAround:
            return 1.0f + 1.5f * m.boredom;
        case kNothing:
            return 3.0f;
        default:
            return 0.0f;
    }
}

void IdleDirectorModifier::run_behavior(Modifiable& sc, uint32_t now)
{
    static constexpr uint32_t kCooldownMs[kBehaviorCount] = {
        60000, 8 * 60000, 5 * 60000, 5 * 60000, 15 * 60000, 30 * 60000, 30 * 60000, 10 * 60000, 20 * 60000,
        3 * 60000, 0,
    };
    static constexpr const char* kNames[kBehaviorCount] = {
        "head-look", "yawn", "side-eye", "sigh", "bug-chase", "sneeze", "hiccup", "hum", "nod-off", "look-around",
        "nothing",
    };

    float weights[kBehaviorCount];
    float total = 0.0f;
    for (int b = 0; b < kBehaviorCount; ++b) {
        const bool blocked = now < _cooldown_until[b] || (b == _last_behavior && b != kNothing);
        weights[b]         = blocked ? 0.0f : behavior_weight(b);
        total += weights[b];
    }
    if (total <= 0.0f) {
        return;
    }

    float pick   = rndf(0.0f, total);
    int behavior = kNothing;
    for (int b = 0; b < kBehaviorCount; ++b) {
        if (pick < weights[b]) {
            behavior = b;
            break;
        }
        pick -= weights[b];
    }

    _last_behavior            = behavior;
    _cooldown_until[behavior] = now + kCooldownMs[behavior];
    mclog::tagInfo(_tag, "behaviour: {}", kNames[behavior]);

    switch (behavior) {
        case kHeadLook: {
            const float x = rndf(-0.35f, 0.35f);
            const float y = rndf(-0.75f, -0.1f);
            auto& motion  = sc.motion();
            if (motion.isModifyLocked()) {
                break;
            }
            // Eyes lead, the head follows slowly.
            play_keys(sc, anim::glance(static_cast<int16_t>(x * 150), static_cast<int16_t>(-y * 40), 1500),
                      AnimPurpose::Idle, now);
            GetIdleMood().headTokens -= 1.0f;
            motion.lookAtNormalized(x, y, rnd(90, 160));
            _servo_busy_until = now + 4000;
            break;
        }
        case kYawn:
            play_keys(sc, anim::yawn(), AnimPurpose::Idle, now);
            break;
        case kSideEye:
            if (faces_on()) {
                show_temp_face(sc, FaceStyle::Peek, 3500, now);
            } else {
                play_keys(sc, anim::sideEyeAtLight(rnd(0, 1) == 0), AnimPurpose::Idle, now);
            }
            break;
        case kSigh:
            play_keys(sc, anim::sigh(), AnimPurpose::Idle, now);
            break;
        case kBugChase:
            play(sc, std::make_unique<BugChaseAnimation>(rnd(4000, 6000)), AnimPurpose::Idle, now);
            break;
        case kSneeze:
            play_keys(sc, anim::sneeze(), AnimPurpose::Idle, now);
            break;
        case kHiccup:
            play_keys(sc, anim::hiccup(), AnimPurpose::Idle, now);
            break;
        case kHum:
            if (faces_on()) {
                show_temp_face(sc, FaceStyle::Hum, 6000, now);
            } else {
                play_keys(sc, anim::silentHum(), AnimPurpose::Idle, now);
            }
            break;
        case kNodOff: {
            // Eyes droop, the head dips a little, then jerks awake.
            play_keys(sc,
                      {key(0), key(2400).eyes(25).bob(4), key(2600).eyes(100).size(40).bob(-8),
                       key(3200).eyes(100).size(20), key(3700)},
                      AnimPurpose::Idle, now);
            if (GetIdleMood().headTokens >= 1.0f && try_head_move(sc, now, INT32_MIN, kAwakePitch - 80, 40, true)) {
                _pending_move_at = now + 2600;
                _pending_pitch   = kAwakePitch;
                _pending_speed   = 350;
            }
            break;
        }
        case kLookAround:
            play_keys(sc, anim::lookAround(rnd(2500, 4000)), AnimPurpose::Idle, now);
            break;
        default:
            break;
    }
}

bool IdleDirectorModifier::try_head_move(Modifiable& sc, uint32_t now, int yaw, int pitch, int speed, bool budgeted)
{
    auto& motion = sc.motion();
    if (motion.isModifyLocked()) {
        return false;
    }
    if (budgeted) {
        auto& mood = GetIdleMood();
        if (mood.headTokens < 1.0f) {
            return false;
        }
        mood.headTokens -= 1.0f;
    }
    if (yaw == INT32_MIN) {
        motion.movePitchWithSpeed(pitch, speed);
    } else {
        motion.moveWithSpeed(yaw, pitch, speed);
    }
    _servo_busy_until = now + 5000;
    return true;
}

/* ---------------------------------------------------------------------------------------------- */
/*                                            Helpers                                             */
/* ---------------------------------------------------------------------------------------------- */

void IdleDirectorModifier::play(Modifiable& sc, std::unique_ptr<FaceAnimation> anim, AnimPurpose purpose,
                                uint32_t now)
{
    stop_anim(sc);
    // Animations work on the default features: drop a face they would fight with before they capture
    // their base values. Eye-only idle animations (glances) may run on top of a face that keeps the eyes.
    if (anim->touchesMouth() || purpose != AnimPurpose::Idle || !_face.allowsGlances()) {
        _face.clear(sc.avatar());
        _temp_face = FaceStyle::None;
    }
    _anim         = std::move(anim);
    _anim_purpose = purpose;
    sync_lock(sc);
    _anim->begin(sc.avatar(), now);
}

void IdleDirectorModifier::play_keys(Modifiable& sc, std::vector<FaceKey> keys, AnimPurpose purpose, uint32_t now)
{
    play(sc, std::make_unique<TimelineAnimation>(std::move(keys)), purpose, now);
}

void IdleDirectorModifier::stop_anim(Modifiable& sc)
{
    if (!_anim) {
        return;
    }
    const bool was_doze = _anim_purpose == AnimPurpose::DozeOff;
    _anim->end(sc.avatar());
    _anim.reset();
    if (was_doze && _phase == Phase::Awake) {
        sc.avatar().setEmotion(avatar::Emotion::Neutral);  // interrupted while dozing off
    }
}

void IdleDirectorModifier::finish_anim(Modifiable& sc, uint32_t now)
{
    const AnimPurpose purpose = _anim_purpose;
    _anim.reset();

    if (purpose == AnimPurpose::DozeOff) {
        _phase        = Phase::Sleeping;
        _sleep_reason = _doze_reason;
        _nap_until    = now + kNapMs;
        _next_zzz     = now + rnd(5000, 15000);
        _next_dream   = now + rnd(180000, 360000);
        apply_sleep_pose(sc, now);
    } else if (_phase == Phase::Sleeping) {
        apply_sleep_pose(sc, now);  // keep the eyes closed after a peek or a dream
    }
}

void IdleDirectorModifier::set_emotion_for(Modifiable& sc, avatar::Emotion emotion, uint32_t durationMs, uint32_t now)
{
    sc.avatar().setEmotion(emotion);
    _emotion_restore_at = now + durationMs;
}

void IdleDirectorModifier::show_bubble(Modifiable& sc, const char* text, uint32_t durationMs, uint32_t now)
{
    sc.avatar().setSpeech(text);
    _bubble_active = true;
    _bubble_until  = now + durationMs;
}

void IdleDirectorModifier::sync_lock(Modifiable& sc)
{
    auto& av           = sc.avatar();
    const bool desired = _anim != nullptr || _sleep_pose || _face.holdsEyes();
    if (desired && !av.isModifyLocked()) {
        av.setModifyLock(true);
        _locked_by_us = true;
    } else if (!desired && _locked_by_us) {
        av.setModifyLock(false);
        _locked_by_us = false;
    }
}

void IdleDirectorModifier::set_led_fx(LedFx fx, uint8_t r, uint8_t g, uint8_t b, uint32_t durationMs, uint32_t now)
{
    _led_fx          = fx;
    _led_fx_start    = now;
    _led_fx_until    = now + durationMs;
    _led_fx_color[0] = r;
    _led_fx_color[1] = g;
    _led_fx_color[2] = b;
}

void IdleDirectorModifier::update_led(uint32_t now)
{
    // Claude Code sessions take the mood LEDs; short reaction effects (pick-up, petting) still win.
    if (!(_led_fx != LedFx::None && now < _led_fx_until) && update_claude_led(now)) {
        return;
    }

    const auto& m     = GetIdleMood();
    const float scale = setting(SettingId::LedMax) / 24.0f;
    float target[3]   = {0, 0, 0};

    if (_led_fx != LedFx::None && now < _led_fx_until) {
        const float t = (now - _led_fx_start) / 1000.0f;
        float k       = 0.0f;
        if (_led_fx == LedFx::Heartbeat) {
            const float p = std::fmod(t, 1.0f);  // two quick beats per second
            k             = (p < 0.12f || (p > 0.25f && p < 0.37f)) ? 1.0f : 0.25f;
        } else {
            k = 0.55f + 0.45f * std::sin(t * 6.28f * 1.5f);
        }
        for (int i = 0; i < 3; ++i) {
            target[i] = _led_fx_color[i] * k;
        }
        std::copy(target, target + 3, _led);  // effects are not smoothed
    } else {
        _led_fx = LedFx::None;
        if (_phase == Phase::Awake && !(_anim && _anim_purpose == AnimPurpose::DozeOff)) {
            // Hue from valence, brightness from energy, slow breathing (slower when bored).
            float base[3];
            if (m.valence < 0.35f) {
                base[0] = 0, base[1] = 6, base[2] = 18;  // blue: grumpy
            } else if (m.valence > 0.65f) {
                base[0] = 20, base[1] = 10, base[2] = 2;  // amber: happy
            } else {
                base[0] = 0, base[1] = 14, base[2] = 12;  // teal: calm
            }
            const float period = 6.0f + 4.0f * m.boredom;
            const float breath = 0.8f + 0.2f * std::sin(now / 1000.0f * 6.28f / period);
            const float bright = (0.35f + 0.65f * m.energy) * breath;
            for (int i = 0; i < 3; ++i) {
                target[i] = base[i] * bright;
            }
        }
        for (int i = 0; i < 3; ++i) {
            _led[i] += (target[i] - _led[i]) * 0.15f;
        }
    }

    write_led(static_cast<uint8_t>(std::lround(std::min(255.0f, _led[0] * scale))),
              static_cast<uint8_t>(std::lround(std::min(255.0f, _led[1] * scale))),
              static_cast<uint8_t>(std::lround(std::min(255.0f, _led[2] * scale))));
}

void IdleDirectorModifier::write_led(uint8_t r, uint8_t g, uint8_t b)
{
    if (_led_written[0] == r && _led_written[1] == g && _led_written[2] == b) {
        return;
    }
    _led_written[0] = r;
    _led_written[1] = g;
    _led_written[2] = b;
    for (int index : kMoodLeds) {
        GetHAL().setRgbColor(index, r, g, b);
    }
    GetHAL().refreshRgb();
    _claude_written.fill(kNoColor);  // the session LEDs must be rewritten afterwards
}

/* ---------------------------------------------------------------------------------------------- */
/*                                   Claude Code status (REQ-003)                                 */
/* ---------------------------------------------------------------------------------------------- */

void IdleDirectorModifier::on_claude_event(Modifiable& sc, const ClaudeEvent& event, uint32_t now)
{
    static constexpr const char* kNames[] = {"needs", "done", "fail", "err", "compact", "pass"};
    mclog::tagInfo(_tag, "claude {} (slot {})", kNames[static_cast<int>(event.ev)], event.slot);

    if (event.ev == ClaudeEv::Fail) {
        _claude_flash_until[event.slot] = now + 3000;
    }
    if (is_night()) {
        return;  // REQ-003: at night only the session LEDs, no face, head move or sound
    }

    const bool needs = event.ev == ClaudeEv::Needs;
    if (needs) {
        if (event.sound) {
            // Played from the main task: powering the codec up does not belong under the LVGL lock.
            Application::GetInstance().Schedule(
                []() { Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP); });
        }
        note_interaction(now);  // stay awake for the grace period
        if (_phase == Phase::Sleeping || (_anim && _anim_purpose == AnimPurpose::DozeOff)) {
            wake_up(sc, now, false);  // waking up is the call-back gesture here
            show_bubble(sc, event.text, 8000, now);
            return;
        }
    } else if (_phase == Phase::Sleeping) {
        return;  // asleep: the session LEDs tell it
    }

    const bool busy = _held || _tilted || _touching || now < _paused_until ||
                      (_anim && _anim_purpose != AnimPurpose::Idle);
    if (busy) {
        show_bubble(sc, event.text, 6000, now);  // another reaction owns the face and the head
        return;
    }
    if (_anim) {
        stop_anim(sc);
    }

    auto nod = [&]() {
        if (try_head_move(sc, now, INT32_MIN, kAwakePitch - 60, kSlowSpeed * 2, true)) {
            _pending_move_at = now + 700;
            _pending_pitch   = kAwakePitch;
            _pending_speed   = kSlowSpeed * 2;
        }
    };
    switch (event.ev) {
        case ClaudeEv::Needs:
            claude_reaction(sc, FaceStyle::Needs, BuddyPose::Needs, 8000, now);
            show_bubble(sc, event.text, 8000, now);
            if (try_head_move(sc, now, INT32_MIN, kAwakePitch + 120, kSlowSpeed * 2, true)) {
                _pending_move_at = now + 2500;  // look up at the user, then settle
                _pending_pitch   = kAwakePitch;
                _pending_speed   = kSlowSpeed;
            }
            break;
        case ClaudeEv::Done:
            claude_reaction(sc, FaceStyle::Done, BuddyPose::Done, 5000, now);
            show_bubble(sc, event.text, 4000, now);
            nod();
            break;
        case ClaudeEv::Pass:
            claude_reaction(sc, FaceStyle::Done, BuddyPose::Done, 4000, now);
            show_bubble(sc, event.text, 4000, now);
            nod();
            break;
        case ClaudeEv::Fail:
            claude_reaction(sc, FaceStyle::Fail, BuddyPose::Fail, 4000, now);
            show_bubble(sc, event.text, 4000, now);
            break;
        case ClaudeEv::Err:
            claude_reaction(sc, FaceStyle::Err, BuddyPose::Err, 5000, now);
            show_bubble(sc, event.text, 5000, now);
            break;
        case ClaudeEv::Compact:
            claude_reaction(sc, FaceStyle::Compact, BuddyPose::Compact, 4000, now);
            show_bubble(sc, event.text, 4000, now);
            break;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/*                                              Faces                                             */
/* ---------------------------------------------------------------------------------------------- */

// The avatar in AI Agent mode is always the DefaultAvatar created by StackChanAvatarDisplay::SetupUI.
static lv_obj_t* avatar_panel(Modifiable& sc)
{
    auto* panel = static_cast<avatar::DefaultAvatar&>(sc.avatar()).getPanel();
    return panel ? panel->get() : nullptr;
}

void IdleDirectorModifier::show_temp_face(Modifiable& sc, FaceStyle style, uint32_t durationMs, uint32_t now)
{
    if (_phase != Phase::Awake || !faces_on()) {
        return;  // without faces the bubble alone tells it
    }
    if (_anim) {
        stop_anim(sc);
    }
    _emotion_restore_at = 0;
    sc.avatar().setEmotion(avatar::Emotion::Neutral);
    _temp_face       = style;
    _temp_face_until = now + durationMs;
    _face.set(sc.avatar(), avatar_panel(sc), style, now);
    _face_since = now;
    mclog::tagInfo(_tag, "face: {} ({} ms)", face_style_name(style), durationMs);
}

FaceStyle IdleDirectorModifier::claude_face(uint32_t now)
{
    if (now < _next_claude_face) {
        return _claude_face;
    }
    _next_claude_face = now + kClaudeFaceMs;
    _claude_face      = FaceStyle::None;
    _claude_full      = false;

    ClaudeStatus::Sessions sessions;
    if (!claude_on() || !GetClaudeStatus().snapshot(sessions, now)) {
        return _claude_face;
    }
    // Priority: a waiting session, then any working one (the latest started), then a recent result.
    const ClaudeSession* working = nullptr;
    const ClaudeSession* latest  = nullptr;
    bool waiting                 = false;
    for (const auto& s : sessions) {
        if (!s.used) {
            continue;
        }
        _claude_full = _claude_full || s.ctx >= 80;
        waiting      = waiting || s.st == ClaudeSt::Wait;
        if (s.st == ClaudeSt::Work && (!working || age_ms(s.stSinceMs, working->stSinceMs) > 0)) {
            working = &s;
        }
        if (!latest || age_ms(s.stSinceMs, latest->stSinceMs) > 0) {
            latest = &s;
        }
    }
    if (is_night()) {
        return _claude_face;  // night: the LEDs only
    }
    if (waiting) {
        _claude_face = FaceStyle::Needs;
    } else if (working) {
        switch (working->act) {
            case ClaudeAct::Think:
            case ClaudeAct::Agents:
                _claude_face = FaceStyle::Think;
                break;
            case ClaudeAct::Web:
                _claude_face = FaceStyle::Curious;  // looking around the web
                break;
            default:
                _claude_face = FaceStyle::Work;
                break;
        }
    } else if (latest && latest->st == ClaudeSt::Done &&
               age_ms(now, latest->stSinceMs) < static_cast<int32_t>(kDoneFaceMs)) {
        _claude_face = FaceStyle::Done;
    } else if (latest && latest->st == ClaudeSt::Err &&
               age_ms(now, latest->stSinceMs) < static_cast<int32_t>(kErrFaceMs)) {
        _claude_face = FaceStyle::Err;
    }
    return _claude_face;
}

FaceStyle IdleDirectorModifier::pick_idle_face() const
{
    const auto& m = GetIdleMood();
    struct Option {
        FaceStyle style;
        float weight;
    } options[] = {
        {FaceStyle::Sparkle, 1.0f + (m.energy > 0.8f ? 2.0f : 0.0f)},
        {FaceStyle::Joy, 1.0f + (m.valence > 0.65f ? 2.0f : 0.0f)},
        {FaceStyle::Blush, 0.5f + (m.affection > 0.5f ? 2.0f : 0.0f)},
        {FaceStyle::Curious, 1.0f + (m.boredom > 0.4f ? 2.0f : 0.0f)},
        {FaceStyle::Wink, 0.7f},
        {FaceStyle::Hum, m.valence > 0.5f ? 1.0f : 0.3f},
        {FaceStyle::Peek, 1.0f},
        {FaceStyle::Full, _claude_full && !is_night() ? 3.0f : 0.0f},  // a session's context is nearly full
    };
    float total = 0.0f;
    for (auto& o : options) {
        if (o.style == _last_idle_face) {
            o.weight = 0.0f;
        }
        total += o.weight;
    }
    float pick = rndf(0.0f, total);
    for (const auto& o : options) {
        if (pick < o.weight) {
            return o.style;
        }
        pick -= o.weight;
    }
    return FaceStyle::SoftSmile;
}

void IdleDirectorModifier::update_face(Modifiable& sc, uint32_t now)
{
    auto& av = sc.avatar();
    if (_buddy.visible()) {
        if (_face.style() != FaceStyle::None) {
            _face.clear(av);
        }
        _temp_face = FaceStyle::None;
        return;
    }
    // Faces are for the awake robot. Reactions, mouth animations and emotion overrides use the default
    // features meanwhile; eye-only idle animations (glances) run on top of a face that keeps the eyes.
    const bool anim_owns =
        _anim && (_anim->touchesMouth() || _anim_purpose != AnimPurpose::Idle || !_face.allowsGlances());
    // _paused_until covers the shake and tap reactions, which other modifiers draw on the default features.
    if (_phase != Phase::Awake || _sleep_pose || anim_owns || _held || _tilted || _emotion_restore_at ||
        now < _paused_until || !faces_on()) {
        if (_face.style() != FaceStyle::None) {
            _face.clear(av);
            _temp_face = FaceStyle::None;
        }
        return;
    }

    FaceStyle desired = FaceStyle::SoftSmile;
    if (_temp_face != FaceStyle::None && now < _temp_face_until) {
        desired = _temp_face;
    } else {
        _temp_face            = FaceStyle::None;
        const FaceStyle claude = claude_face(now);
        if (claude != FaceStyle::None) {
            desired = claude;
        } else if (now >= _next_idle_face && !_anim && !_touching) {
            // Now and then a special face for a few seconds; mostly the soft smile.
            _next_idle_face = now + idle_face_delay(false);
            _last_idle_face = pick_idle_face();
            show_temp_face(sc, _last_idle_face, rnd(5000, 8000), now);
            return;
        }
    }

    if (desired != _face.style()) {
        // Hold each face a moment so quick activity changes do not flicker; a call for the user never waits.
        if (_face.style() != FaceStyle::None && desired != FaceStyle::Needs &&
            age_ms(now, _face_since) < static_cast<int32_t>(kFaceHoldMs)) {
            _face.update(av, now);
            return;
        }
        if (_anim) {
            stop_anim(sc);  // an eye animation captured the old face's sizes and offsets: end it first
        }
        _face.set(av, avatar_panel(sc), desired, now);
        _face_since = now;
    }
    _face.update(av, now);
}

void IdleDirectorModifier::claude_reaction(Modifiable& sc, FaceStyle face, BuddyPose pose, uint32_t durationMs,
                                           uint32_t now)
{
    if (_buddy.visible() && !_buddy.takingOff()) {
        _buddy_temp       = pose;
        _buddy_temp_until = now + durationMs;
        return;
    }
    show_temp_face(sc, face, durationMs, now);
}

/* ---------------------------------------------------------------------------------------------- */
/*                                  Pixel buddy suit (REQ-007)                                    */
/* ---------------------------------------------------------------------------------------------- */

bool IdleDirectorModifier::suit_allowed() const
{
    return ScSettings::get().enabled(SettingId::ClaudeSuit) && claude_on() && faces_on() &&
           _phase == Phase::Awake && !_sleep_pose && !is_night();
}

// On while a session works or waits for the user, off a while (setting) after the last such moment, so
// the robot does not change clothes between two prompts.
void IdleDirectorModifier::update_suit(Modifiable& sc, uint32_t now)
{
    auto& av           = sc.avatar();
    const bool allowed = suit_allowed();
    bool active        = false;
    ClaudeStatus::Sessions sessions;
    if (allowed && GetClaudeStatus().snapshot(sessions, now)) {
        for (const auto& s : sessions) {
            active = active || (s.used && (s.st == ClaudeSt::Work || s.st == ClaudeSt::Wait));
        }
    }
    if (active) {
        _claude_active_at = now;
    }
    const bool wearing  = _buddy.visible() && !_buddy.takingOff();
    const uint32_t keep = static_cast<uint32_t>(setting(SettingId::SuitOffMinutes)) * 60000u;
    const bool want     = allowed && (active || (wearing && now - _claude_active_at < keep));

    if (want && !wearing) {
        const bool busy = _held || _tilted || _touching || now < _paused_until ||
                          (_anim && _anim_purpose != AnimPurpose::Idle);
        if (!busy) {
            if (_anim) {
                stop_anim(sc);
            }
            _face.clear(av);
            _temp_face = FaceStyle::None;
            _buddy.setColorScheme(setting(SettingId::SuitColor));
            if (now - _created < 3000) {
                _buddy.show(av, avatar_panel(sc), now);  // back after a conversation: no transition
                mclog::tagInfo(_tag, "claude suit: on");
            } else {
                _buddy.suitOn(av, avatar_panel(sc), now);
                mclog::tagInfo(_tag, "claude suit: putting it on");
            }
        }
    } else if (!want && wearing) {
        mclog::tagInfo(_tag, "claude suit: taking it off ({})", allowed ? "no activity" : "not now");
        _buddy.suitOff(now);
    }

    if (_buddy.visible()) {
        _buddy.setColorScheme(setting(SettingId::SuitColor));
        _buddy.setPose(buddy_pose(now), now);
        _buddy.update(av, now);
        on_buddy_cue(_buddy.takeCue(), now);
        if (!_buddy.visible()) {
            mclog::tagInfo(_tag, "claude suit: off");
        }
    }
}

BuddyPose IdleDirectorModifier::buddy_pose(uint32_t now)
{
    if (now < _buddy_temp_until) {
        return _buddy_temp;
    }
    switch (claude_face(now)) {  // the same priority as the faces: waiting > working > recent result
        case FaceStyle::Needs:
            return BuddyPose::Needs;
        case FaceStyle::Think:
        case FaceStyle::Curious:
            return BuddyPose::Think;
        case FaceStyle::Work:
            return BuddyPose::Work;
        case FaceStyle::Done:
            return BuddyPose::Done;
        case FaceStyle::Err:
            return BuddyPose::Err;
        default:
            return _claude_full ? BuddyPose::Full : BuddyPose::Idle;
    }
}

// Head movements that go with the transitions (settings "suit_head", "suit_head_spd").
void IdleDirectorModifier::on_buddy_cue(BuddyCue cue, uint32_t now)
{
    if (cue == BuddyCue::None || !ScSettings::get().enabled(SettingId::SuitHead)) {
        return;
    }
    const int fast = setting(SettingId::SuitHeadSpeed);
    const int back = std::max(60, fast / 3);
    switch (cue) {
        case BuddyCue::Dip:  // the suit lands on the head
            _head_keys = {{now, INT32_MIN, kAwakePitch - 70, fast}, {now + 350, INT32_MIN, kAwakePitch, back}};
            break;
        case BuddyCue::Lift:  // ta-da
            _head_keys = {{now, INT32_MIN, kAwakePitch + 70, fast}, {now + 450, INT32_MIN, kAwakePitch, back}};
            break;
        case BuddyCue::Recoil:  // the steam puffs out
            _head_keys = {{now, INT32_MIN, kAwakePitch + 100, fast}, {now + 450, INT32_MIN, kAwakePitch, back}};
            break;
        case BuddyCue::Shake:  // shaking the sweat off
            _head_keys = {{now, 150, kAwakePitch, fast},
                          {now + 160, -150, kAwakePitch, fast},
                          {now + 320, 120, kAwakePitch, fast},
                          {now + 480, -120, kAwakePitch, fast},
                          {now + 640, 0, kAwakePitch, back}};
            break;
        default:
            return;
    }
    static constexpr const char* kNames[] = {"none", "dip", "lift", "recoil", "shake"};
    mclog::tagInfo(_tag, "suit head: {} (speed {})", kNames[static_cast<int>(cue)], fast);
}

void IdleDirectorModifier::run_head_keys(Modifiable& sc, uint32_t now)
{
    while (!_head_keys.empty() && now >= _head_keys.front().at) {
        const HeadKey key = _head_keys.front();
        _head_keys.erase(_head_keys.begin());
        try_head_move(sc, now, key.yaw, key.pitch, key.speed, false);
    }
}

bool IdleDirectorModifier::update_claude_led(uint32_t now)
{
    ClaudeStatus::Sessions sessions;
    if (!claude_on() || !GetClaudeStatus().snapshot(sessions, now)) {
        if (_claude_led_active) {
            _claude_led_active = false;
            _led_written[0] = _led_written[1] = _led_written[2] = -1;  // mood light rewrites everything
        }
        return false;
    }
    if (!_claude_led_active) {
        _claude_led_active = true;
        _claude_written.fill(kNoColor);
        _led_written[0] = _led_written[1] = _led_written[2] = -1;  // the mood cache no longer matches
    }

    static constexpr uint32_t kDoneDimMs = 10 * 60 * 1000;
    const float t     = now / 1000.0f;
    const float scale = setting(SettingId::LedMax) / 24.0f * (_phase == Phase::Sleeping ? 0.35f : 1.0f);
    bool dirty        = false;
    for (int slot = 0; slot < ClaudeStatus::kSlots; ++slot) {
        const auto& s = sessions[slot];
        float rgb[3]  = {0, 0, 0};
        auto set      = [&rgb](float r, float g, float b) { rgb[0] = r, rgb[1] = g, rgb[2] = b; };
        if (now < _claude_flash_until[slot]) {
            std::fmod(t, 0.4f) < 0.2f ? set(24, 0, 0) : set(0, 0, 0);  // a tool failed
        } else if (s.used) {
            switch (s.st) {
                case ClaudeSt::Idle:
                    set(8, 8, 8);
                    break;
                case ClaudeSt::Work:
                    if (s.act == ClaudeAct::Agents) {
                        const float k = 0.55f + 0.45f * std::sin(t * 6.28f);  // subagents: faster, cyan
                        set(0, 14 * k, 18 * k);
                    } else {
                        const float k = 0.55f + 0.45f * std::sin(t * 3.14f);  // 2 s breathing
                        set(0, 0, 22 * k);
                    }
                    break;
                case ClaudeSt::Wait:
                    std::fmod(t, 1.0f) < 0.5f ? set(24, 12, 0) : set(3, 1, 0);  // needs the user
                    break;
                case ClaudeSt::Done:
                    age_ms(now, s.stSinceMs) < static_cast<int32_t>(kDoneDimMs) ? set(0, 18, 0) : set(0, 4, 0);
                    break;
                case ClaudeSt::Err:
                    set(22, 0, 0);
                    break;
            }
        }
        // The PY32 LED driver keeps RGB565 (r, b: 8 per step, g: 4): lift dim non-zero channels to the
        // first step so night and idle colours do not vanish.
        static constexpr int kMinStep[3] = {8, 4, 8};
        uint8_t c[3];
        for (int i = 0; i < 3; ++i) {
            int v = static_cast<int>(std::lround(std::min(255.0f, rgb[i] * scale)));
            if (rgb[i] > 0.5f && v < kMinStep[i]) {
                v = kMinStep[i];
            }
            c[i] = static_cast<uint8_t>(v);
        }
        const uint32_t packed = static_cast<uint32_t>(c[0]) << 16 | c[1] << 8 | c[2];
        if (_claude_written[slot] != packed) {
            _claude_written[slot] = packed;
            GetHAL().setRgbColor(kMoodLeds[slot], c[0], c[1], c[2]);
            GetHAL().setRgbColor(kMoodLeds[slot + ClaudeStatus::kSlots], c[0], c[1], c[2]);
            dirty = true;
        }
    }
    if (dirty) {
        GetHAL().refreshRgb();
    }
    return true;
}

void IdleDirectorModifier::restore_backlight()
{
    _bl_target = -1;
    _bl_logged = -1;
    if (_saved_backlight >= 0) {
        GetHAL().setBackLightBrightness(static_cast<uint8_t>(_saved_backlight), false);  // instant
        _saved_backlight = -1;
    }
}

int IdleDirectorModifier::sleep_backlight() const
{
    int target = setting((_lux >= 0.0f && _bl_lit) ? SettingId::SleepBacklightLit : SettingId::SleepBacklight);
    if (_saved_backlight >= 0) {
        target = std::min(target, _saved_backlight);  // sleeping never brightens the screen (0 stays off)
    }
    return target;
}

void IdleDirectorModifier::update_backlight(uint32_t now)
{
    if (_bl_target < 0 || now < _next_bl_step) {
        return;
    }
    if (_bl_target != _bl_logged) {
        _bl_logged = _bl_target;
        mclog::tagInfo(_tag, "sleep backlight {} ({:.1f} lux)", _bl_target, _lux);
    }
    const int current = GetHAL().getBackLightBrightness();
    if (current == _bl_target) {
        return;
    }
    int next       = _bl_target;
    const int from = backlight_step(current);
    const int to   = backlight_step(_bl_target);
    if (CONFIG_SC_IDLE_BACKLIGHT_FADE_MS > 0 && from >= 0 && to >= 0 && std::abs(to - from) > 1) {
        next = backlight_of_step(from + (to > from ? 1 : -1));  // one 100 mV step at a time
    }
    GetHAL().setBackLightBrightness(static_cast<uint8_t>(next), false);
    _next_bl_step = now + CONFIG_SC_IDLE_BACKLIGHT_FADE_MS;
}

}  // namespace stackchan::sc

#endif  // CONFIG_SC_IDLE_DIRECTOR

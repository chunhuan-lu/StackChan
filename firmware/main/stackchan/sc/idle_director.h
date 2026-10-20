/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "face_animation.h"
#include <hal/hal.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace stackchan::sc {

/**
 * @brief Inner state that survives the director being re-created after every conversation.
 */
struct IdleMood {
    float energy               = 0.7f;   // 0 tired .. 1 lively, follows the clock
    float boredom              = 0.0f;   // 0 entertained .. 1 very bored
    float valence              = 0.5f;   // 0 grumpy .. 1 happy
    float affection            = 0.3f;   // 0 .. 1, grows slowly with petting
    float headTokens           = -1.0f;  // servo budget for spontaneous head moves, < 0 = uninitialised
    uint32_t lastInteractionMs = 0;
};

IdleMood& GetIdleMood();

/**
 * @brief Mood-driven idle behaviour for the AI Agent standby state. Silent by design.
 *
 * Owns: daily schedule and sleeping (night or darkness), head-touch taps / long press / stroke
 * afterglow, pick-up and tilt reactions, screen-only micro animations, the mood light on LEDs
 * 1-5 and 7-11 (LED 0 and 6 are left to the device status indicator) and a servo budget.
 */
class IdleDirectorModifier : public Modifier {
public:
    explicit IdleDirectorModifier(int activityLevel);
    ~IdleDirectorModifier() override;

    void _update(Modifiable& stackchan) override;

private:
    enum class Phase { Awake, Sleeping };
    enum class SleepReason { None, Night, Dark, Nap };
    enum class AnimPurpose { Idle, Reaction, DozeOff, WakeUp };
    enum class LedFx { None, Heartbeat, Pulse };
    enum Behavior {
        kHeadLook = 0,
        kYawn,
        kSideEye,
        kSigh,
        kBugChase,
        kSneeze,
        kHiccup,
        kHum,
        kNodOff,
        kLookAround,
        kNothing,
        kBehaviorCount,
    };

    struct TouchEvent {
        HeadPetGesture gesture;
        uint32_t ms;
    };

    // Inputs
    void handle_touch_events(Modifiable& sc, uint32_t now);
    void on_press(Modifiable& sc, uint32_t now);
    void on_swipe(uint32_t now);
    void on_release(Modifiable& sc, uint32_t now, uint32_t ms);
    void on_taps(Modifiable& sc, uint32_t now);
    void on_long_press(Modifiable& sc, uint32_t now);
    void on_shake(Modifiable& sc, uint32_t now);
    void poll_imu(Modifiable& sc, uint32_t now);
    void on_pickup(Modifiable& sc, uint32_t now);
    void on_set_down(Modifiable& sc, uint32_t now);
    void on_tilt(Modifiable& sc, float angle);
    void on_untilt(Modifiable& sc);
    void poll_light(uint32_t now);
    void poll_clock(uint32_t now);
    void update_mood(uint32_t now);
    void note_interaction(uint32_t now);

    // Phases
    void update_sleep_state(Modifiable& sc, uint32_t now);
    void update_awake(Modifiable& sc, uint32_t now);
    void update_sleeping(Modifiable& sc, uint32_t now);
    void begin_doze(Modifiable& sc, uint32_t now, SleepReason reason);
    void apply_sleep_pose(Modifiable& sc, uint32_t now);
    void leave_sleep_pose(Modifiable& sc);
    void wake_up(Modifiable& sc, uint32_t now, bool morning);

    // Behaviours
    void run_behavior(Modifiable& sc, uint32_t now);
    float behavior_weight(int behavior) const;
    bool try_head_move(Modifiable& sc, uint32_t now, int yaw, int pitch, int speed, bool budgeted);

    // Helpers
    void play(Modifiable& sc, std::unique_ptr<FaceAnimation> anim, AnimPurpose purpose, uint32_t now);
    void play_keys(Modifiable& sc, std::vector<FaceKey> keys, AnimPurpose purpose, uint32_t now);
    void stop_anim(Modifiable& sc);
    void finish_anim(Modifiable& sc, uint32_t now);
    void set_emotion_for(Modifiable& sc, avatar::Emotion emotion, uint32_t durationMs, uint32_t now);
    void show_bubble(Modifiable& sc, const char* text, uint32_t durationMs, uint32_t now);
    void sync_lock(Modifiable& sc);
    void set_led_fx(LedFx fx, uint8_t r, uint8_t g, uint8_t b, uint32_t durationMs, uint32_t now);
    void update_led(uint32_t now);
    void write_led(uint8_t r, uint8_t g, uint8_t b);
    void restore_backlight();
    bool is_night() const;

    int _activity;
    Modifiable* _sc     = nullptr;
    bool _started       = false;
    uint32_t _created   = 0;
    uint32_t _awake_since = 0;

    Phase _phase               = Phase::Awake;
    SleepReason _sleep_reason  = SleepReason::None;
    SleepReason _doze_reason   = SleepReason::None;
    bool _sleep_pose           = false;
    uint32_t _nap_until        = 0;
    int _saved_backlight       = -1;
    uint32_t _resleep_pose_at  = 0;

    std::unique_ptr<FaceAnimation> _anim;
    AnimPurpose _anim_purpose = AnimPurpose::Idle;
    bool _locked_by_us        = false;
    uint32_t _paused_until    = 0;

    // Emotion override with automatic restore
    uint32_t _emotion_restore_at = 0;

    // Pending servo command (used by nod-off)
    uint32_t _pending_move_at = 0;
    int _pending_pitch        = 0;
    int _pending_speed        = 0;

    // Schedule inputs
    uint32_t _next_mood   = 0;
    uint32_t _next_clock  = 0;
    uint32_t _next_light  = 0;
    uint32_t _next_imu    = 0;
    uint32_t _next_lux_log = 0;
    bool _time_valid      = false;
    int _hour             = -1;
    bool _dark            = false;
    bool _light_on_edge   = false;
    uint32_t _dark_since   = 0;
    uint32_t _bright_since = 0;
    float _lux             = -1.0f;

    // Spontaneous behaviour scheduling
    uint32_t _next_glance = 0;
    uint32_t _next_action = 0;
    std::array<uint32_t, kBehaviorCount> _cooldown_until{};
    int _last_behavior = -1;

    // Touch
    std::mutex _event_mutex;
    std::vector<TouchEvent> _events;
    int _touch_conn = -1;
    bool _touching  = false;
    bool _swiped    = false;
    bool _long_press_fired = false;
    uint32_t _press_ms     = 0;
    std::vector<uint32_t> _taps;
    uint32_t _annoyed_until = 0;
    int _strokes            = 0;
    uint32_t _afterglow_at  = 0;

    // IMU
    std::atomic<bool> _shake_event{false};
    int _imu_conn = -1;
    bool _imu_init = false;
    float _g[3]    = {0, 0, 0};
    float _prev[3] = {0, 0, 0};
    float _base[3] = {0, 0, 0};
    bool _base_valid       = false;
    float _motion_energy   = 0.0f;
    uint32_t _stable_since = 0;
    uint32_t _servo_busy_until = 0;
    uint32_t _touch_quiet_until = 0;  // touching the head jolts the IMU: no pick-up detection meanwhile
    bool _held                 = false;
    uint32_t _pickup_since     = 0;
    uint32_t _held_still_since = 0;
    uint32_t _next_held_look   = 0;
    bool _tilted               = false;
    uint32_t _tilt_since       = 0;
    uint32_t _untilt_since     = 0;
    int _sweat_id              = -1;
    uint32_t _next_imu_log     = 0;

    // Mood light
    float _led[3]        = {0, 0, 0};
    int _led_written[3]  = {-1, -1, -1};
    LedFx _led_fx        = LedFx::None;
    uint32_t _led_fx_start = 0;
    uint32_t _led_fx_until = 0;
    float _led_fx_color[3] = {0, 0, 0};
    uint32_t _next_led   = 0;

    // Speech bubble (silent "Zzz", "?!", "...")
    bool _bubble_active    = false;
    uint32_t _bubble_until = 0;
    uint32_t _next_zzz     = 0;
    uint32_t _next_dream   = 0;
};

}  // namespace stackchan::sc

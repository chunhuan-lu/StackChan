/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "sc_settings.h"
#include <hal/hal.h>
#include <settings.h>
#include <sdkconfig.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// Defaults for settings whose feature is not compiled in (they are then not listed, but stay readable).
#ifndef CONFIG_SC_IDLE_HEAD_MOVES_PER_HOUR
#define CONFIG_SC_IDLE_HEAD_MOVES_PER_HOUR 20
#endif
#ifndef CONFIG_SC_IDLE_WAKE_HOUR
#define CONFIG_SC_IDLE_WAKE_HOUR 8
#endif
#ifndef CONFIG_SC_IDLE_SLEEP_HOUR
#define CONFIG_SC_IDLE_SLEEP_HOUR 23
#endif
#ifndef CONFIG_SC_IDLE_DARK_LUX
#define CONFIG_SC_IDLE_DARK_LUX 3
#endif
#ifndef CONFIG_SC_IDLE_BRIGHT_LUX
#define CONFIG_SC_IDLE_BRIGHT_LUX 8
#endif
#ifndef CONFIG_SC_IDLE_SLEEP_BACKLIGHT
#define CONFIG_SC_IDLE_SLEEP_BACKLIGHT 5
#endif
#ifndef CONFIG_SC_IDLE_SLEEP_BACKLIGHT_LIT
#define CONFIG_SC_IDLE_SLEEP_BACKLIGHT_LIT 45
#endif
#ifndef CONFIG_SC_IDLE_LED_MAX
#define CONFIG_SC_IDLE_LED_MAX 24
#endif
#ifndef CONFIG_SC_IDLE_FACE_MIN_MINUTES
#define CONFIG_SC_IDLE_FACE_MIN_MINUTES 4
#endif
#ifndef CONFIG_SC_IDLE_FACE_MAX_MINUTES
#define CONFIG_SC_IDLE_FACE_MAX_MINUTES 10
#endif
#ifdef CONFIG_SC_IDLE_FACES
#define SC_FACES_DEFAULT 1
#else
#define SC_FACES_DEFAULT 0
#endif
#ifdef CONFIG_SC_CLAUDE_SUIT
#define SC_SUIT_DEFAULT 1
#else
#define SC_SUIT_DEFAULT 0
#endif
#ifndef CONFIG_SC_CLAUDE_SUIT_COLOR
#define CONFIG_SC_CLAUDE_SUIT_COLOR 1
#endif
#ifndef CONFIG_SC_CLAUDE_SUIT_OFF_MINUTES
#define CONFIG_SC_CLAUDE_SUIT_OFF_MINUTES 2
#endif
#ifdef CONFIG_SC_CLAUDE_SUIT_HEAD
#define SC_SUIT_HEAD_DEFAULT 1
#else
#define SC_SUIT_HEAD_DEFAULT 0
#endif
#ifndef CONFIG_SC_CLAUDE_SUIT_HEAD_SPEED
#define CONFIG_SC_CLAUDE_SUIT_HEAD_SPEED 600
#endif

namespace stackchan::sc {

static constexpr const char* kNvsNamespace = "sc_cfg";
static const char* const kActivityLabels[] = {"Off", "Low", "Medium", "High"};
static const char* const kSuitColorLabels[] = {"Clay", "Orange", "Bright"};

// Ranges here and in Kconfig.projbuild are the same.
static const SettingInfo kSettings[] = {
    {SettingId::Activity, "activity", "Idle", "Activity", "", SettingType::Choice, 2, 0, 3, 1, kActivityLabels,
     "How lively the robot is while idle: 0 off, 1 low, 2 medium, 3 high."},
    {SettingId::HeadMoves, "head_moves", "Idle", "Head moves / hour", "", SettingType::Int,
     CONFIG_SC_IDLE_HEAD_MOVES_PER_HOUR, 0, 120, 5, nullptr,
     "Spontaneous head movements per hour at medium activity."},
    {SettingId::WakeHour, "wake_hour", "Sleep", "Wake up at", ":00", SettingType::Int, CONFIG_SC_IDLE_WAKE_HOUR, 0,
     23, 1, nullptr, "Local hour when the robot wakes up."},
    {SettingId::SleepHour, "sleep_hour", "Sleep", "Go to sleep at", ":00", SettingType::Int,
     CONFIG_SC_IDLE_SLEEP_HOUR, 0, 23, 1, nullptr, "Local hour when the robot goes to sleep."},
    {SettingId::DarkLux, "dark_lux", "Sleep", "Dark below", " lux", SettingType::Int, CONFIG_SC_IDLE_DARK_LUX, 0, 100,
     1, nullptr, "The robot sleeps when the room stays darker than this."},
    {SettingId::BrightLux, "bright_lux", "Sleep", "Light above", " lux", SettingType::Int, CONFIG_SC_IDLE_BRIGHT_LUX, 1,
     200, 1, nullptr, "The room counts as lit above this (must be above the dark level)."},
    {SettingId::SleepBacklight, "sleep_bl", "Sleep", "Sleep screen, dark", "%", SettingType::Int,
     CONFIG_SC_IDLE_SLEEP_BACKLIGHT, 0, 100, 5, nullptr, "Screen brightness while sleeping in a dark room."},
    {SettingId::SleepBacklightLit, "sleep_bl_lit", "Sleep", "Sleep screen, lit", "%", SettingType::Int,
     CONFIG_SC_IDLE_SLEEP_BACKLIGHT_LIT, 0, 100, 5, nullptr,
     "Screen brightness while sleeping with the lights on."},
    {SettingId::LedMax, "led_max", "Faces", "Light brightness", "", SettingType::Int, CONFIG_SC_IDLE_LED_MAX, 0, 255,
     4, nullptr, "Brightness of the mood light and the Claude Code session lights."},
    {SettingId::FaceMinMinutes, "face_min", "Faces", "Special face, from", " min", SettingType::Int,
     CONFIG_SC_IDLE_FACE_MIN_MINUTES, 1, 60, 1, nullptr, "Shortest time between two special idle faces."},
    {SettingId::FaceMaxMinutes, "face_max", "Faces", "Special face, to", " min", SettingType::Int,
     CONFIG_SC_IDLE_FACE_MAX_MINUTES, 1, 60, 1, nullptr, "Longest time between two special idle faces."},
    {SettingId::Faces, "faces", "Faces", "Expressive faces", "", SettingType::Bool, SC_FACES_DEFAULT, 0, 1, 1,
     nullptr, "Soft smile and the other drawn faces instead of the plain dot eyes."},
    {SettingId::TapWake, "tap_wake", "Faces", "Tap wakes me up", "", SettingType::Bool, 1, 0, 1, 1, nullptr,
     "A tap on the sleeping face wakes the robot instead of starting a conversation."},
    {SettingId::Claude, "claude", "Claude Code", "Show Claude Code", "", SettingType::Bool, 1, 0, 1, 1, nullptr,
     "Show Claude Code sessions on the lights and the face."},
    {SettingId::ClaudeSuit, "claude_suit", "Claude Code", "Pixel buddy suit", "", SettingType::Bool, SC_SUIT_DEFAULT,
     0, 1, 1, nullptr, "While Claude Code works the robot wears an orange pixel buddy costume, put on and taken off with a funny transition."},
    {SettingId::SuitColor, "suit_color", "Claude Code", "Suit colour", "", SettingType::Choice,
     CONFIG_SC_CLAUDE_SUIT_COLOR, 0, 2, 1, kSuitColorLabels,
     "Colour of the costume: 0 clay (the Claude brand colour, muted on this screen), 1 orange, 2 bright orange."},
    {SettingId::SuitOffMinutes, "suit_off_min", "Claude Code", "Suit off after", " min", SettingType::Int,
     CONFIG_SC_CLAUDE_SUIT_OFF_MINUTES, 1, 30, 1, nullptr,
     "Minutes without Claude Code activity before the robot takes the costume off."},
    {SettingId::SuitHead, "suit_head", "Claude Code", "Head moves along", "", SettingType::Bool,
     SC_SUIT_HEAD_DEFAULT, 0, 1, 1, nullptr, "The head nods, leans back and shakes with the costume transitions."},
    {SettingId::SuitHeadSpeed, "suit_head_spd", "Claude Code", "Head speed", "", SettingType::Int,
     CONFIG_SC_CLAUDE_SUIT_HEAD_SPEED, 100, 1000, 50, nullptr,
     "Servo speed of those head movements, 100 slow to 1000 fastest."},
    {SettingId::UsbAwake, "usb_awake", "Power", "No standby on USB", "", SettingType::Bool, 1, 0, 1, 1, nullptr,
     "Never power-save or power off while USB power is present."},
};
static_assert(sizeof(kSettings) / sizeof(kSettings[0]) == static_cast<size_t>(SettingId::Count),
              "one table entry per SettingId, in enum order");

static bool compiled_in(SettingId id)
{
    switch (id) {
        case SettingId::TapWake:
#if CONFIG_SC_IDLE_DIRECTOR && CONFIG_SC_TOUCH_FIXES
            return true;
#else
            return false;
#endif
        case SettingId::Claude:
        case SettingId::ClaudeSuit:
        case SettingId::SuitColor:
        case SettingId::SuitOffMinutes:
        case SettingId::SuitHead:
        case SettingId::SuitHeadSpeed:
#if CONFIG_SC_IDLE_DIRECTOR && CONFIG_SC_USB_LINK
            return true;
#else
            return false;
#endif
        case SettingId::UsbAwake:
#if CONFIG_SC_EXT_POWER_NO_STANDBY
            return true;
#else
            return false;
#endif
        default:
#if CONFIG_SC_IDLE_DIRECTOR
            return true;
#else
            return false;
#endif
    }
}

ScSettings& ScSettings::get()
{
    static ScSettings instance;
    return instance;
}

ScSettings::ScSettings()
{
    for (const auto& s : kSettings) {
        _values[static_cast<size_t>(s.id)].store(s.def);
        if (compiled_in(s.id)) {
            _available.push_back(&s);
        }
    }
    load();
}

const SettingInfo& ScSettings::info(SettingId id) const
{
    return kSettings[static_cast<size_t>(id)];  // the table is in enum order
}

const SettingInfo* ScSettings::find(const char* key) const
{
    for (const auto* s : _available) {
        if (std::strcmp(s->key, key) == 0) {
            return s;
        }
    }
    return nullptr;
}

void ScSettings::load()
{
    Settings nvs(kNvsNamespace, false);
    for (const auto& s : kSettings) {
        int v = s.def;
        if (s.id == SettingId::Activity) {
            v = GetHAL().getXiaozhiConfig().idleRandomMovementLevel;  // upstream storage
        } else {
#if CONFIG_SC_SETTINGS_PANEL
            v = nvs.GetInt(s.key, s.def);  // without the panel, values left in NVS must not stick
#endif
        }
        _values[static_cast<size_t>(s.id)].store(std::clamp(v, s.min, s.max));
    }
    // Stored values and Kconfig defaults can disagree (e.g. a new default for one side of a pair).
    keep_pairs(SettingId::BrightLux);
    keep_pairs(SettingId::FaceMaxMinutes);
}

void ScSettings::apply(SettingId id, int value)
{
    _values[static_cast<size_t>(id)].store(value);
    _dirty.fetch_or(1u << static_cast<unsigned>(id));
    _version.fetch_add(1, std::memory_order_relaxed);
}

// Light above dark, the longest face interval not below the shortest; the other side of the pair moves.
void ScSettings::keep_pairs(SettingId changed)
{
    auto keep_order = [&](SettingId low, SettingId high, int gap) {
        if (value(high) - value(low) >= gap) {
            return false;
        }
        if (changed == low) {
            _values[static_cast<size_t>(high)].store(std::min(info(high).max, value(low) + gap));
        }
        if (changed != low || value(high) - value(low) < gap) {
            _values[static_cast<size_t>(low)].store(std::max(info(low).min, value(high) - gap));
        }
        return true;
    };
    if (changed == SettingId::DarkLux || changed == SettingId::BrightLux) {
        if (keep_order(SettingId::DarkLux, SettingId::BrightLux, 1)) {
            _dirty.fetch_or(1u << static_cast<unsigned>(SettingId::DarkLux) |
                            1u << static_cast<unsigned>(SettingId::BrightLux));
        }
    }
    if (changed == SettingId::FaceMinMinutes || changed == SettingId::FaceMaxMinutes) {
        if (keep_order(SettingId::FaceMinMinutes, SettingId::FaceMaxMinutes, 0)) {
            _dirty.fetch_or(1u << static_cast<unsigned>(SettingId::FaceMinMinutes) |
                            1u << static_cast<unsigned>(SettingId::FaceMaxMinutes));
        }
    }
}

bool ScSettings::set(SettingId id, int value, bool save_now)
{
    std::lock_guard<std::mutex> lock(_write_mutex);
    const auto& s = info(id);
    value         = std::clamp(value, s.min, s.max);
    if (value == this->value(id)) {
        return false;
    }
    apply(id, value);
    keep_pairs(id);  // both values are in place before anything is written
    if (save_now) {
        save_locked();
    }
    return true;
}

void ScSettings::save()
{
    std::lock_guard<std::mutex> lock(_write_mutex);
    save_locked();
}

void ScSettings::save_locked()
{
    uint32_t dirty = _dirty.exchange(0);
#if CONFIG_SC_SETTINGS_PANEL
    if (dirty & 1u << static_cast<unsigned>(SettingId::Activity)) {
        dirty &= ~(1u << static_cast<unsigned>(SettingId::Activity));
        auto config                    = GetHAL().getXiaozhiConfig();
        config.idleRandomMovementLevel = static_cast<uint8_t>(value(SettingId::Activity));
        GetHAL().setXiaozhiConfig(config);
    }
    if (!dirty) {
        return;
    }
    Settings nvs(kNvsNamespace, true);
    for (const auto& s : kSettings) {
        if (dirty & 1u << static_cast<unsigned>(s.id)) {
            nvs.SetInt(s.key, value(s.id));
        }
    }
#else
    (void)dirty;  // nothing is changed at runtime without the panel
#endif
}

void ScSettings::reload()
{
    std::lock_guard<std::mutex> lock(_write_mutex);
    save_locked();  // keep unsaved changes
    load();
    _version.fetch_add(1, std::memory_order_relaxed);
}

void ScSettings::resetAll()
{
    std::lock_guard<std::mutex> lock(_write_mutex);
    for (const auto* s : _available) {
        if (value(s->id) != s->def) {
            apply(s->id, s->def);
        }
    }
    save_locked();
}

const char* ScSettings::format(const SettingInfo& s, int value, char* buffer, size_t size)
{
    switch (s.type) {
        case SettingType::Bool:
            snprintf(buffer, size, "%s", value ? "On" : "Off");
            break;
        case SettingType::Choice:
            snprintf(buffer, size, "%s", s.choices[std::clamp(value, s.min, s.max) - s.min]);
            break;
        default:
            snprintf(buffer, size, "%d%s", value, s.unit);
            break;
    }
    return buffer;
}

}  // namespace stackchan::sc

/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace stackchan::sc {

/**
 * @brief Runtime settings of the fork features (REQ-006).
 *
 * One table describes every setting: key, label, type, default, range. Defaults come from Kconfig, so a
 * fork chooses its own defaults in sdkconfig.defaults; values changed on the device (settings panel, voice,
 * CONFIG_SC_SETTINGS_PANEL) are stored in NVS namespace "sc_cfg" and always clamped to the range. Only
 * settings whose feature is compiled in are listed. "Activity" is the upstream idle movement level and keeps
 * its upstream storage, so the Settings app and the panel show the same value; it is listed with the idle
 * director only (the upstream idle motion reads the level once, at start).
 *
 * Thread safe: values are atomics; reads are cheap and can be done every frame.
 */
enum class SettingId : uint8_t {
    Activity,
    HeadMoves,
    Sleep,
    WakeHour,
    SleepHour,
    DarkLux,
    BrightLux,
    SleepBacklight,
    SleepBacklightLit,
    LedMax,
    FaceMinMinutes,
    FaceMaxMinutes,
    Faces,
    TapWake,
    Claude,
    ClaudeSuit,
    SuitColor,
    SuitR,
    SuitG,
    SuitB,
    SuitOffMinutes,
    SuitHead,
    SuitHeadSpeed,
    UsbAwake,
    FaceTrack,
    FaceTrackHead,
    FaceBox,
    FaceProbeRate,
    Count,
};

enum class SettingType : uint8_t { Int, Bool, Choice };

struct SettingInfo {
    SettingId id;
    const char* key;    // NVS key and voice-tool key (<= 15 characters)
    const char* group;  // panel section
    const char* label;  // short English label for the panel
    const char* unit;   // shown after the value, may be empty
    SettingType type;
    int def;
    int min;
    int max;
    int step;
    const char* const* choices;  // labels for Choice values min..max
    const char* help;            // one sentence for the voice tool
};

class ScSettings {
public:
    static ScSettings& get();

    int value(SettingId id) const
    {
        return _values[static_cast<size_t>(id)].load(std::memory_order_relaxed);
    }
    bool enabled(SettingId id) const
    {
        return value(id) != 0;
    }
    // Changes with every stored change; lets a view notice changes made elsewhere (e.g. by voice).
    uint32_t version() const
    {
        return _version.load(std::memory_order_relaxed);
    }
    // Clamps to the range (and keeps related settings consistent) and applies the value at once; true if
    // it changed. save_now = false leaves the NVS write to save(): the panel saves after a pause and on
    // close, so holding +/- does not commit NVS ten times a second.
    bool set(SettingId id, int value, bool save_now = true);
    void save();
    void resetAll();
    // Re-read the stored values. "activity" can be changed by the upstream Settings app after the first
    // read (the board power poll creates this object at boot), so the AI Agent start calls this.
    void reload();

    const std::vector<const SettingInfo*>& available() const
    {
        return _available;
    }
    const SettingInfo* find(const char* key) const;
    const SettingInfo& info(SettingId id) const;
    static const char* format(const SettingInfo& info, int value, char* buffer, size_t size);

private:
    ScSettings();
    void load();
    void apply(SettingId id, int value);
    void keep_pairs(SettingId changed);
    void save_locked();

    std::atomic<int> _values[static_cast<size_t>(SettingId::Count)];
    std::atomic<uint32_t> _version{0};
    std::atomic<uint32_t> _dirty{0};  // bit per SettingId: changed, not yet in NVS
    std::vector<const SettingInfo*> _available;
    std::mutex _write_mutex;
};

inline int setting(SettingId id)
{
    return ScSettings::get().value(id);
}

// Pixel buddy suit colour from the settings (REQ-007): body and the darker outline, 0xRRGGBB.
void suit_colors(uint32_t& body, uint32_t& outline);

}  // namespace stackchan::sc

/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "settings_panel.h"
#include <sdkconfig.h>

#if CONFIG_SC_SETTINGS_PANEL

#include "sc_settings.h"
#include <lvgl.h>
#include <mooncake_log.h>
#include <cstring>
#include <deque>
#include <memory>

namespace stackchan::sc {

static const std::string_view _tag = "Settings";

// Colours of the upstream Settings app.
static constexpr uint32_t kBg      = 0xEDF4FF;
static constexpr uint32_t kText    = 0x26206A;
static constexpr uint32_t kMuted   = 0x7E7B9C;
static constexpr uint32_t kRow     = 0xFFFFFF;
static constexpr uint32_t kButton  = 0xB8D3FD;
static constexpr uint32_t kWarn    = 0xF2C6C6;
static constexpr uint32_t kIdleMs  = 60 * 1000;
static constexpr uint32_t kArmedMs = 3000;
static constexpr uint32_t kPollMs  = 500;
static constexpr uint32_t kSaveMs  = 1000;  // store once the value stopped changing for this long

namespace {

struct Row {
    SettingId id;
    lv_obj_t* value  = nullptr;  // value label (Int / Choice)
    lv_obj_t* toggle = nullptr;  // switch (Bool)
};

struct Step {
    Row* row;
    int direction;
};

// Exists only while the panel is open. Rows and steps are LVGL user data: a deque keeps them in place.
struct Panel {
    lv_obj_t* root      = nullptr;
    lv_obj_t* reset     = nullptr;
    lv_timer_t* timer   = nullptr;  // closes the panel when nobody touches it
    lv_timer_t* poll    = nullptr;  // saves, picks up changes made by voice, disarms the reset
    uint32_t version    = 0;
    uint32_t armed_at   = 0;  // "Reset to defaults" waits for a second tap
    uint32_t changed_at = 0;  // unsaved change
    std::deque<Row> rows;
    std::deque<Step> steps;
};

std::unique_ptr<Panel> s_panel;

void touch_activity()
{
    lv_timer_reset(s_panel->timer);
}

void note_change()
{
    s_panel->changed_at = lv_tick_get() | 1;  // 0 means "nothing to save"
    touch_activity();
}

void refresh_values()
{
    auto& settings   = ScSettings::get();
    s_panel->version = settings.version();
    char text[32];
    for (auto& row : s_panel->rows) {
        const auto& info = settings.info(row.id);
        const int v      = settings.value(row.id);
        if (row.value) {
            lv_label_set_text(row.value, ScSettings::format(info, v, text, sizeof(text)));
        }
        if (row.toggle && (v != 0) != lv_obj_has_state(row.toggle, LV_STATE_CHECKED)) {
            if (v) {
                lv_obj_add_state(row.toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(row.toggle, LV_STATE_CHECKED);
            }
        }
    }
}

lv_obj_t* plain(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

lv_obj_t* button(lv_obj_t* parent, const char* text, int w, int h, uint32_t color)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_t* l = label(b, text, &lv_font_montserrat_20, kText);
    lv_obj_center(l);
    return b;
}

void set_reset_text(const char* text)
{
    lv_label_set_text(lv_obj_get_child(s_panel->reset, 0), text);
}

void on_step(lv_event_t* e)
{
    auto* step       = static_cast<Step*>(lv_event_get_user_data(e));
    auto& settings   = ScSettings::get();
    const auto& info = settings.info(step->row->id);
    if (settings.set(info.id, settings.value(info.id) + step->direction * info.step, false)) {
        mclog::tagInfo(_tag, "{} = {}", info.key, settings.value(info.id));
        note_change();
    }
    refresh_values();
    touch_activity();
}

void on_toggle(lv_event_t* e)
{
    auto* row      = static_cast<Row*>(lv_event_get_user_data(e));
    const bool on  = lv_obj_has_state(row->toggle, LV_STATE_CHECKED);
    auto& settings = ScSettings::get();
    if (settings.set(row->id, on ? 1 : 0, false)) {
        mclog::tagInfo(_tag, "{} = {}", settings.info(row->id).key, on ? 1 : 0);
        note_change();
    }
    touch_activity();
}

void on_reset(lv_event_t*)
{
    const uint32_t now = lv_tick_get();
    if (s_panel->armed_at && lv_tick_diff(now, s_panel->armed_at) < kArmedMs) {
        s_panel->armed_at   = 0;
        s_panel->changed_at = 0;
        ScSettings::get().resetAll();  // saves
        mclog::tagInfo(_tag, "reset to defaults");
        set_reset_text("Reset to defaults");
        refresh_values();
    } else {
        s_panel->armed_at = now | 1;
        set_reset_text("Tap again to reset");
    }
    touch_activity();
}

void on_close(lv_event_t*)
{
    close_settings_panel();
}

void on_idle(lv_timer_t*)
{
    close_settings_panel();
}

void on_poll(lv_timer_t*)
{
    const uint32_t now = lv_tick_get();
    if (s_panel->changed_at && lv_tick_diff(now, s_panel->changed_at) >= kSaveMs) {
        s_panel->changed_at = 0;
        ScSettings::get().save();
    }
    if (s_panel->armed_at && lv_tick_diff(now, s_panel->armed_at) >= kArmedMs) {
        s_panel->armed_at = 0;
        set_reset_text("Reset to defaults");
    }
    if (ScSettings::get().version() != s_panel->version) {
        refresh_values();
    }
}

void add_row(lv_obj_t* list, const SettingInfo& info)
{
    lv_obj_t* row = plain(list);
    lv_obj_set_size(row, LV_PCT(100), 46);
    lv_obj_set_style_bg_color(row, lv_color_hex(kRow), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);

    // Long labels wrap to a second line; two lines of montserrat 16 fit the row.
    lv_obj_t* name = label(row, info.label, &lv_font_montserrat_16, kText);
    lv_obj_set_width(name, info.type == SettingType::Bool ? 220 : 130);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    s_panel->rows.push_back({info.id});
    Row& r = s_panel->rows.back();
    if (info.type == SettingType::Bool) {
        r.toggle = lv_switch_create(row);
        lv_obj_set_size(r.toggle, 52, 28);
        lv_obj_align(r.toggle, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(r.toggle, on_toggle, LV_EVENT_VALUE_CHANGED, &r);
        return;
    }
    lv_obj_t* minus = button(row, "-", 40, 34, kButton);
    lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -112, 0);
    r.value = label(row, "", &lv_font_montserrat_16, kText);
    lv_obj_set_width(r.value, 72);
    lv_obj_set_style_text_align(r.value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(r.value, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_t* plus = button(row, "+", 40, 34, kButton);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
    for (auto [btn, dir] : {std::pair<lv_obj_t*, int>{minus, -1}, {plus, 1}}) {
        s_panel->steps.push_back({&r, dir});
        lv_obj_add_event_cb(btn, on_step, LV_EVENT_SHORT_CLICKED, &s_panel->steps.back());
        lv_obj_add_event_cb(btn, on_step, LV_EVENT_LONG_PRESSED_REPEAT, &s_panel->steps.back());
    }
}

}  // namespace

bool settings_panel_open()
{
    return s_panel != nullptr;
}

void close_settings_panel()
{
    if (!s_panel) {
        return;
    }
    ScSettings::get().save();
    lv_timer_delete(s_panel->timer);
    lv_timer_delete(s_panel->poll);
    // Usually called from an event of a child: hide now, delete once the event is over.
    lv_obj_add_flag(s_panel->root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(s_panel->root);
    s_panel.reset();
    mclog::tagInfo(_tag, "closed");
}

void open_settings_panel()
{
    if (s_panel) {
        return;
    }
    mclog::tagInfo(_tag, "opened");
    s_panel        = std::make_unique<Panel>();
    lv_obj_t* root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 320, 240);
    lv_obj_set_style_bg_color(root, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);  // swallow taps meant for the face below
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_panel->root = root;

    lv_obj_t* title = label(root, "Settings", &lv_font_montserrat_20, kText);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_t* close = button(root, LV_SYMBOL_CLOSE, 48, 34, kButton);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 320, 198);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_obj_set_style_pad_hor(list, 8, 0);
    lv_obj_set_style_pad_ver(list, 4, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);

    // Timers before any callback that may touch them.
    s_panel->timer = lv_timer_create(on_idle, kIdleMs, nullptr);
    s_panel->poll  = lv_timer_create(on_poll, kPollMs, nullptr);
    lv_obj_add_event_cb(
        list, [](lv_event_t*) { touch_activity(); }, LV_EVENT_SCROLL, nullptr);

    const char* group = nullptr;
    for (const auto* info : ScSettings::get().available()) {
        if (!group || std::strcmp(group, info->group) != 0) {
            group        = info->group;
            lv_obj_t* hd = label(list, group, &lv_font_montserrat_16, kMuted);
            lv_obj_set_style_pad_top(hd, 4, 0);
        }
        add_row(list, *info);
    }
    s_panel->reset = button(list, "Reset to defaults", LV_PCT(100), 40, kWarn);
    lv_obj_set_style_text_font(lv_obj_get_child(s_panel->reset, 0), &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(s_panel->reset, on_reset, LV_EVENT_CLICKED, nullptr);

    refresh_values();
}

}  // namespace stackchan::sc

#endif  // CONFIG_SC_SETTINGS_PANEL

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <mooncake.h>
#include <mooncake_log.h>
#include <cstdint>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <smooth_lvgl.hpp>
#include <assets/assets.h>
#include <hal/hal.h>
#include <memory>
#include <lvgl.h>

using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

/**
 * @brief
 *
 */
class HomeGesture {
public:
    std::function<void(void)> onGesture;

    HomeGesture() : _is_tracking(false), _last_state(LV_INDEV_STATE_REL)
    {
    }

    void init()
    {
        _is_tracking      = false;
        _last_state       = LV_INDEV_STATE_REL;
        _screen_height    = 240;
        _bottom_threshold = 20;  // 距离底部 20 像素内触发
        _swipe_min_dist   = 50;  // 向上滑动至少 50 像素才触发
    }

    void update()
    {
        lv_indev_t* indev = GetHAL().lvTouchpad;
        if (!indev) {
            return;
        }

        lv_indev_state_t state = lv_indev_get_state(indev);
        lv_point_t curr_point;
        lv_indev_get_point(indev, &curr_point);

        // 1. 按下瞬间 (Transition: Released -> Pressed)
        if (state == LV_INDEV_STATE_PR && _last_state == LV_INDEV_STATE_REL) {
            // 只有在按下那一刻就在底部，才标记为追踪开始
            if (curr_point.y >= (_screen_height - _bottom_threshold) && curr_point.y >= 0) {
                _start_point = curr_point;
                _is_tracking = true;
            } else {
                _is_tracking = false;  // 按下位置不对，此次滑动全程忽略
            }
        }
        // 2. 抬起瞬间 (Transition: Pressed -> Released)
        else if (state == LV_INDEV_STATE_REL && _last_state == LV_INDEV_STATE_PR) {
            if (_is_tracking) {
                int delta_y = _start_point.y - curr_point.y;  // 向上滑为正
                int delta_x = abs(curr_point.x - _start_point.x);

                // 判断标准：向上位移足够，且角度偏垂直
                if (delta_y > _swipe_min_dist && delta_y > delta_x) {
                    mclog::tagInfo("HomeIndicator", "home gesture ({},{}) -> ({},{})", _start_point.x,
                                   _start_point.y, curr_point.x, curr_point.y);  // [StackChan fork]
                    if (onGesture) {
                        onGesture();
                    }
                }
                _is_tracking = false;  // 重置追踪状态
            }
        }

        _last_state = state;  // 更新状态机
    }

private:
    bool _is_tracking;
    lv_indev_state_t _last_state;  // 记录上一帧的状态
    lv_point_t _start_point;
    int _screen_height;
    int _bottom_threshold;
    int _swipe_min_dist;
};

/**
 * @brief
 *
 */
class HomeButton {
public:
    HomeButton(lv_obj_t* parent, uint32_t colorButton, uint32_t colorBorder)
    {
        _bg_mask = std::make_unique<Container>(parent);
        _bg_mask->align(LV_ALIGN_CENTER, 0, 0);
        _bg_mask->setSize(320, 240);
        _bg_mask->setRadius(0);
        _bg_mask->setBgOpa(0);
        _bg_mask->setBorderWidth(0);
        // _bg_mask->setBgColor(lv_color_white());
        _bg_mask->addFlag(LV_OBJ_FLAG_CLICKABLE);
        _bg_mask->addFlag(LV_OBJ_FLAG_FLOATING);
        _bg_mask->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        _bg_mask->setPadding(0, 0, 0, 0);
        _bg_mask->onClick().connect([&]() { hide(); });

        _btn = std::make_unique<Button>(_bg_mask->get());
        _btn->setSize(152, 73);
        _btn->setAlign(LV_ALIGN_BOTTOM_MID);
        _btn->setBgColor(lv_color_hex(colorButton));
        _btn->setBorderWidth(2);
        _btn->setBorderColor(lv_color_hex(colorBorder));
        _btn->setShadowWidth(0);
        _btn->setRadius(18);
        _btn->addFlag(LV_OBJ_FLAG_FLOATING);
        _btn->onClick().connect([&]() {
            mclog::tagInfo("HomeIndicator", "home button clicked");  // [StackChan fork]
            _is_clicked = true;
        });

        _icon = std::make_unique<Image>(_btn->get());
        _icon->align(LV_ALIGN_CENTER, 0, -10);
        _icon_home = assets::get_image("icon_home.bin");
        _icon->setSrc(&_icon_home);
        _icon->setImageRecolorOpa(LV_OPA_COVER);
        _icon->setImageRecolor(lv_color_hex(colorBorder));

        _btn->setPos(0, _pos_y_hide);
        _pos_y_anim.springOptions().bounce         = 0.1;
        _pos_y_anim.springOptions().visualDuration = 0.3;
        // _pos_y_anim.easingOptions().duration = 0.3;
        _pos_y_anim.teleport(_pos_y_hide);
    }

    // [StackChan fork] Gear button right of Home, opens the settings panel (REQ-006).
    void addSettingsButton(uint32_t colorButton, uint32_t colorBorder)
    {
        if (_gear) {
            return;
        }
        _gear = std::make_unique<Button>(_bg_mask->get());
        _gear->setSize(73, 73);
        _gear->setAlign(LV_ALIGN_BOTTOM_MID);
        _gear->setBgColor(lv_color_hex(colorButton));
        _gear->setBorderWidth(2);
        _gear->setBorderColor(lv_color_hex(colorBorder));
        _gear->setShadowWidth(0);
        _gear->setRadius(18);
        _gear->addFlag(LV_OBJ_FLAG_FLOATING);
        _gear->setPos(kGearX, _pos_y_anim.directValue());
        _gear->onClick().connect([&]() {
            mclog::tagInfo("HomeIndicator", "settings button clicked");
            _is_settings_clicked = true;
        });
        lv_obj_t* icon = lv_label_create(_gear->get());
        lv_label_set_text(icon, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(colorBorder), 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);
    }

    bool isSettingsClicked()
    {
        if (_is_settings_clicked) {
            _is_settings_clicked = false;
            return true;
        }
        return false;
    }

    void update()
    {
        _pos_y_anim.update();
        if (!_pos_y_anim.done()) {
            _btn->setPos(0, _pos_y_anim.directValue());
            if (_gear) {
                _gear->setPos(kGearX, _pos_y_anim.directValue());  // [StackChan fork]
            }
        } else {
            if (_hide_mask_flag) {
                _hide_mask_flag = false;
                _bg_mask->setHidden(true);
            }
        }
    }

    void show()
    {
        _bg_mask->moveForeground();
        _bg_mask->setHidden(false);
        _pos_y_anim = _pos_y_show;
        _is_hidden  = false;
    }

    void hide()
    {
        _pos_y_anim     = _pos_y_hide;
        _is_hidden      = true;
        _hide_mask_flag = true;
    }

    bool isHidden() const
    {
        return _is_hidden;
    }

    // [StackChan fork] Hide at once, without the slide-out animation (used right before a restart).
    void hideNow()
    {
        _btn->setHidden(true);
        if (_gear) {
            _gear->setHidden(true);
        }
        _bg_mask->setHidden(true);
        _is_hidden = true;
    }

    bool isClicked()
    {
        if (_is_clicked) {
            _is_clicked = false;
            return true;
        }
        return false;
    }

private:
    const int _pos_y_show = 22;
    const int _pos_y_hide = 75;
    static constexpr int kGearX = 120;  // [StackChan fork] centre offset: right of the 152 px Home button

    std::unique_ptr<Container> _bg_mask;
    std::unique_ptr<Button> _btn;
    std::unique_ptr<Image> _icon;
    AnimateValue _pos_y_anim;
    bool _is_hidden      = false;
    bool _is_clicked     = false;
    bool _hide_mask_flag = false;
    lv_image_dsc_t _icon_home;
    std::unique_ptr<Button> _gear;  // [StackChan fork]
    bool _is_settings_clicked = false;
};

/**
 * @brief
 *
 */
class HomeIndicator {
public:
    std::function<void(void)> onGoHome;
    std::function<bool(void)> willRestart;  // [StackChan fork] nullptr = going home always restarts

#if CONFIG_SC_TOUCH_FIXES
    ~HomeIndicator()
    {
        if (_going_home) {
            lv_obj_delete(_going_home);  // never outlive the indicator (Settings goes home without a restart)
        }
    }
#endif

    void init(lv_obj_t* parent, uint32_t colorButton, uint32_t colorBorder)
    {
        _home_gesture            = std::make_unique<HomeGesture>();
        _home_gesture->onGesture = [&]() { handle_home_gesture(); };
        _home_gesture->init();

        _home_button = std::make_unique<HomeButton>(parent, colorButton, colorBorder);
        _color_button = colorButton;  // [StackChan fork]
        _color_border = colorBorder;
        _home_button->show();
        _button_show_tick = GetHAL().millis();
        _is_first_show    = true;
    }

    void update()
    {
#if CONFIG_SC_TOUCH_FIXES
        // [StackChan fork] The "Going home..." overlay is drawn by the LVGL task; go home once it had time
        // to show. onGoHome may destroy this object, so return right after it.
        if (_go_home_at) {
            if (GetHAL().millis() >= _go_home_at && onGoHome) {
                _go_home_at = 0;
                onGoHome();
            }
            return;
        }
        _home_gesture->update();
        _home_button->update();
        // Judge a pending click before the button times out, so a click made just before is kept.
        if (check_go_home()) {
            return;
        }
        check_open_settings();
        update_home_button_visibility();
#else
        _home_gesture->update();
        _home_button->update();
        check_open_settings();  // [StackChan fork]
        update_home_button_visibility();
        check_go_home();
#endif
    }

    // [StackChan fork] Show a gear next to the Home button that calls onSettings.
    void enableSettings(std::function<void(void)> onSettings)
    {
        _on_settings = std::move(onSettings);
        _home_button->addSettingsButton(_color_button, _color_border);
    }

private:
    std::unique_ptr<HomeGesture> _home_gesture;
    std::unique_ptr<HomeButton> _home_button;
    uint32_t _button_show_tick = 0;
    bool _is_first_show        = true;
    // [StackChan fork]
    std::function<void(void)> _on_settings;
    uint32_t _color_button = 0;
    uint32_t _color_border = 0;
#if CONFIG_SC_TOUCH_FIXES
    lv_obj_t* _going_home = nullptr;
    uint32_t _go_home_at  = 0;
#endif

    // [StackChan fork] Same guard as the Home click: ignore clicks while the buttons slide.
    void check_open_settings()
    {
        if (!_home_button->isSettingsClicked() || !_on_settings) {
            return;
        }
        if (_home_button->isHidden() || GetHAL().millis() - _button_show_tick < 300) {
            mclog::tagInfo("HomeIndicator", "settings click ignored (button sliding)");
            return;
        }
        _home_button->hide();
        _on_settings();
    }

    void handle_home_gesture()
    {
        _home_button->show();
        _button_show_tick = GetHAL().millis();
    }

    void update_home_button_visibility()
    {
        if (!_home_button->isHidden()) {
            if (GetHAL().millis() - _button_show_tick > (_is_first_show ? 1600 : 3000)) {
                _is_first_show = false;
                _home_button->hide();
            }
        }
    }

#if CONFIG_SC_TOUCH_FIXES
    // [StackChan fork] Returns true if onGoHome ran (this object may be gone) or is scheduled.
    bool check_go_home()
    {
        if (!_home_button->isClicked()) {
            return false;
        }
        // The button stays clickable while it slides in or out: ignore those clicks, they usually come
        // from the same touch as the gesture.
        if (_home_button->isHidden() || GetHAL().millis() - _button_show_tick < 300) {
            mclog::tagInfo("HomeIndicator", "home click ignored (button sliding)");
            return false;
        }
        if (!willRestart || willRestart()) {
            show_going_home();
            _go_home_at = GetHAL().millis() + 300;
            return true;
        }
        if (onGoHome) {
            onGoHome();
        }
        return true;
    }

    // Going home restarts the device in the AI Agent (xiaozhi cannot be stopped in place) and in most
    // apps. Say so, otherwise the restart looks like a crash.
    void show_going_home()
    {
        _home_button->hideNow();
        _going_home = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(_going_home);
        lv_obj_remove_flag(_going_home, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(_going_home, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(_going_home, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(_going_home, LV_OPA_70, 0);
        lv_obj_t* label = lv_label_create(_going_home);
        lv_label_set_text(label, "Going home...");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_center(label);
    }
#else
    void check_go_home()
    {
        if (_home_button->isClicked()) {
            if (onGoHome) {
                onGoHome();
            }
        }
    }
#endif
};

/**
 * @brief
 *
 */
namespace view {

static std::unique_ptr<HomeIndicator> _home_indicator;

void create_home_indicator(std::function<void(void)> onGoHome, uint32_t colorButton, uint32_t colorBorder,
                           lv_obj_t* parent)
{
    _home_indicator           = std::make_unique<HomeIndicator>();
    _home_indicator->onGoHome = onGoHome;
    _home_indicator->init(parent, colorButton, colorBorder);
}

void update_home_indicator()
{
    if (_home_indicator) {
        _home_indicator->update();
    }
}

bool is_home_indicator_created()
{
    return _home_indicator != nullptr;
}

void destroy_home_indicator()
{
    _home_indicator.reset();
}

// [StackChan fork]
void set_home_indicator_will_restart(std::function<bool(void)> willRestart)
{
    if (_home_indicator) {
        _home_indicator->willRestart = std::move(willRestart);
    }
}

// [StackChan fork]
void set_home_indicator_settings(std::function<void(void)> onSettings)
{
    if (_home_indicator) {
        _home_indicator->enableSettings(std::move(onSettings));
    }
}

}  // namespace view

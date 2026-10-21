/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <functional>

namespace view {

void create_home_indicator(std::function<void(void)> onGoHome, uint32_t colorButton = 0xB8D3FD,
                           uint32_t colorBorder = 0x26206A, lv_obj_t* parent = lv_screen_active());
void update_home_indicator();
bool is_home_indicator_created();
void destroy_home_indicator();
// [StackChan fork] Whether going home restarts the device (shows "Going home..."); default: always.
void set_home_indicator_will_restart(std::function<bool(void)> willRestart);
// [StackChan fork] Add a gear next to the Home button; onSettings runs in update_home_indicator().
void set_home_indicator_settings(std::function<void(void)> onSettings);

}  // namespace view

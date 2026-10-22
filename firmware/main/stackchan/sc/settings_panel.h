/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace stackchan::sc {

/**
 * @brief Full-screen settings panel for the fork features (REQ-006), opened from the gear next to the Home
 *        button in AI Agent mode. Changes apply at once and are stored 1 s after the last change or on
 *        close; the panel closes itself after 60 s without a touch. Call under the LVGL lock. Defined with
 *        CONFIG_SC_SETTINGS_PANEL only.
 */
void open_settings_panel();
void close_settings_panel();
bool settings_panel_open();

}  // namespace stackchan::sc

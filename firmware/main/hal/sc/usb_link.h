/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace sc::usb_link {

/**
 * @brief Start the USB link task (REQ-003, USB link protocol v1).
 *
 * Checksummed JSON lines on the native USB-Serial/JTAG port, next to the normal log output:
 * RX through the usb_serial_jtag driver, replies through the same VFS path as the log so lines do
 * not interleave. Commands from the PC agent update stackchan::sc::GetClaudeStatus().
 */
void start();

/**
 * @brief True once after the PC sent "agent" (start the AI Agent, e.g. after sc-flash), within 30 s.
 *
 * The launcher polls it from its own update and opens the AI Agent like a tap on its icon, so the
 * launcher view is closed under the LVGL lock before mooncake is torn down.
 */
bool consume_agent_request();

}  // namespace sc::usb_link

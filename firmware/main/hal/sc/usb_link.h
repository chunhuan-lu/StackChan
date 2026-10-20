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

}  // namespace sc::usb_link

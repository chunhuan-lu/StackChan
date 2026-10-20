/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>

bool Hal::isOnExternalPower()
{
    return hal_bridge::board_is_on_external_power();
}

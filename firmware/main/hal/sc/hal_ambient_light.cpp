/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "ltr553.h"
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <mooncake_log.h>
#include <memory>
#include <mutex>

static const std::string_view _tag = "HAL-ALS";

static constexpr uint32_t kRetryIntervalMs = 60000;

static std::mutex _mutex;
static std::unique_ptr<sc::Ltr553> _sensor;
static uint32_t _last_init_attempt = 0;
static bool _init_attempted        = false;

bool Hal::getAmbientLux(float& lux)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_sensor) {
        const uint32_t now = millis();
        if (_init_attempted && now - _last_init_attempt < kRetryIntervalMs) {
            return false;
        }
        _init_attempted    = true;
        _last_init_attempt = now;

        auto sensor = std::make_unique<sc::Ltr553>();
        if (!sensor->begin(hal_bridge::board_get_i2c_bus())) {
            mclog::tagWarn(_tag, "LTR-553 not found or init failed, retry in {} s", kRetryIntervalMs / 1000);
            return false;
        }
        mclog::tagInfo(_tag, "LTR-553 ready");
        _sensor = std::move(sensor);
        return false;  // no completed measurement yet (the first read returned 0 lux on the device)
    }

    return _sensor->readLux(lux);
}

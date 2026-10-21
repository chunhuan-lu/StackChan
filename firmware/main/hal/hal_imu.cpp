/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/bmi270/bmi270.h"
#include "utils/motion_detector/motion_detector.h"
#include <mooncake_log.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>

static const std::string_view _tag = "HAL-IMU";

static std::unique_ptr<BMI270> _bmi270;

// [StackChan fork] Latest accelerometer sample for Hal::getImuAccel().
static std::mutex _accel_mutex;
static float _accel[3]    = {0, 0, 0};
static bool _accel_valid = false;

// [StackChan fork] watchImuTurn(): window end (ms, 0 = off) and the extremes seen so far.
static std::atomic<uint32_t> _turn_until{0};
static float _turn_min = 0, _turn_max = 0;

static void _watch_turn(const BMI270_Data& d)
{
    const uint32_t until = _turn_until;
    if (until == 0) {
        return;
    }
    // At rest the accelerometer reads +1 g pointing up; a turn counter-clockwise seen from above has its rotation
    // vector pointing up too (right-hand rule), whatever way the sensor sits on the board.
    const float g = std::sqrt(d.accel_x * d.accel_x + d.accel_y * d.accel_y + d.accel_z * d.accel_z);
    if (g > 1.0f) {
        const float turn = (d.gyro_x * d.accel_x + d.gyro_y * d.accel_y + d.gyro_z * d.accel_z) / g;
        _turn_min        = std::min(_turn_min, turn);
        _turn_max        = std::max(_turn_max, turn);
    }
    if (static_cast<int32_t>(GetHAL().millis() - until) >= 0) {
        _turn_until = 0;
        mclog::tagInfo(_tag, "turn about the vertical: min {:.1f}, max {:.1f} deg/s (+ = counter-clockwise from above)",
                       _turn_min, _turn_max);
    }
}

static void _imu_task(void* param)
{
    auto motion_detector = std::make_unique<MotionDetector>();
    motion_detector->setShakeThreshold(16.0f);

    while (1) {
        if (_bmi270 && _bmi270->update()) {
            auto& data = _bmi270->getData();
            // mclog::debug("IMU Accel: {:.2f}\t{:.2f}\t{:.2f}", data.accel_x, data.accel_y, data.accel_z);
            {
                std::lock_guard<std::mutex> lock(_accel_mutex);
                _accel[0]    = data.accel_x;
                _accel[1]    = data.accel_y;
                _accel[2]    = data.accel_z;
                _accel_valid = true;
            }

            motion_detector->update(data.accel_x, data.accel_y, data.accel_z);
            _watch_turn(data);

            if (motion_detector->isShakeDetected()) {
                mclog::tagInfo(_tag, "Shake Detected!");
                GetHAL().onImuMotionEvent.emit(ImuMotionEvent::Shake);
            }
            // if (motion_detector->isPickUpDetected()) {
            //     mclog::tagInfo(_tag, "Pick Up Detected!");
            //     GetHAL().onImuMotionEvent.emit(ImuMotionEvent::PickUp);
            // }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void Hal::imu_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus = hal_bridge::board_get_i2c_bus();

    _bmi270 = std::make_unique<BMI270>(i2c_bus, 0x69);
    if (!_bmi270->begin()) {
        _bmi270.reset();
        mclog::tagError(_tag, "BMI270 init failed");
        return;
    }
    mclog::tagInfo(_tag, "BMI270 init ok");

    // xTaskCreateWithCaps(_imu_task, "imu", 4096, NULL, 2, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(_imu_task, "imu", 4096, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

// [StackChan fork]
void Hal::watchImuTurn(uint32_t ms)
{
    _turn_min = _turn_max = 0;
    _turn_until           = (millis() + ms) | 1;
}

// [StackChan fork]
bool Hal::getImuAccel(float& x, float& y, float& z)
{
    std::lock_guard<std::mutex> lock(_accel_mutex);
    if (!_accel_valid) {
        return false;
    }
    x = _accel[0];
    y = _accel[1];
    z = _accel[2];
    return true;
}

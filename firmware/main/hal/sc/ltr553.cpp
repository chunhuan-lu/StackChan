/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "ltr553.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace sc {

// Register map, see docs/datasheets/LTR-553ALS-WA_proximity_light.pdf
static constexpr uint8_t kRegAlsContr    = 0x80;
static constexpr uint8_t kRegAlsMeasRate = 0x85;
static constexpr uint8_t kRegPartId      = 0x86;
static constexpr uint8_t kRegManufacId   = 0x87;
static constexpr uint8_t kRegAlsDataCh1  = 0x88;  // 0x88..0x8B: CH1 low, CH1 high, CH0 low, CH0 high

static constexpr uint8_t kPartId      = 0x92;
static constexpr uint8_t kManufacId   = 0x05;
static constexpr uint8_t kAlsActive8x = (0b011 << 2) | 0x01;  // gain 8x, active mode
static constexpr uint8_t kMeasRate    = (0b000 << 3) | 0b011;  // 100 ms integration, 500 ms repeat
static constexpr float kGain          = 8.0f;
static constexpr float kIntegration   = 1.0f;  // in units of 100 ms
static constexpr int kTimeoutMs       = 50;

Ltr553::~Ltr553()
{
    if (_dev) {
        i2c_master_bus_rm_device(_dev);
    }
}

bool Ltr553::begin(i2c_master_bus_handle_t bus, uint8_t address)
{
    if (!bus) {
        return false;
    }

    i2c_device_config_t config = {};
    config.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    config.device_address      = address;
    config.scl_speed_hz        = 400000;
    if (i2c_master_bus_add_device(bus, &config, &_dev) != ESP_OK) {
        _dev = nullptr;
        return false;
    }

    uint8_t ids[2] = {0, 0};
    if (!read_regs(kRegPartId, ids, sizeof(ids)) || ids[0] != kPartId || ids[1] != kManufacId) {
        return false;
    }

    if (!write_reg(kRegAlsMeasRate, kMeasRate) || !write_reg(kRegAlsContr, kAlsActive8x)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // wake-up time from standby
    return true;
}

bool Ltr553::readLux(float& lux)
{
    uint8_t raw[4];
    // CH1 must be read first; reading 0x8B latches the next sample.
    if (!read_regs(kRegAlsDataCh1, raw, sizeof(raw))) {
        return false;
    }

    const float ch1 = static_cast<float>(raw[0] | (raw[1] << 8));
    const float ch0 = static_cast<float>(raw[2] | (raw[3] << 8));
    if (ch0 + ch1 <= 0.0f) {
        lux = 0.0f;
        return true;
    }

    // Lux equation from the LTR-553 application note.
    const float ratio = ch1 / (ch0 + ch1);
    float value       = 0.0f;
    if (ratio < 0.45f) {
        value = 1.7743f * ch0 + 1.1059f * ch1;
    } else if (ratio < 0.64f) {
        value = 4.2785f * ch0 - 1.9548f * ch1;
    } else if (ratio < 0.85f) {
        value = 0.5926f * ch0 + 0.1185f * ch1;
    }
    lux = value > 0.0f ? value / kGain / kIntegration : 0.0f;
    return true;
}

bool Ltr553::write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t buffer[2] = {reg, value};
    return _dev && i2c_master_transmit(_dev, buffer, sizeof(buffer), kTimeoutMs) == ESP_OK;
}

bool Ltr553::read_regs(uint8_t reg, uint8_t* data, size_t len)
{
    return _dev && i2c_master_transmit_receive(_dev, &reg, 1, data, len, kTimeoutMs) == ESP_OK;
}

}  // namespace sc

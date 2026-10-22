/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "ina226.h"

namespace sc {

// Register map, see the TI INA226 datasheet
static constexpr uint8_t kRegShuntVoltage = 0x01;  // signed, 2.5 uV/LSB
static constexpr uint8_t kRegBusVoltage   = 0x02;  // 1.25 mV/LSB
static constexpr uint8_t kRegManufId      = 0xFE;
static constexpr uint16_t kManufTi        = 0x5449;  // "TI"
static constexpr float kShuntOhm          = 0.01f;   // R18 on the StackChan power board
static constexpr int kTimeoutMs           = 50;

Ina226::~Ina226()
{
    if (_dev) {
        i2c_master_bus_rm_device(_dev);
    }
}

bool Ina226::begin(i2c_master_bus_handle_t bus, uint8_t address)
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

    uint16_t manuf = 0;
    return read_reg(kRegManufId, manuf) && manuf == kManufTi;
}

bool Ina226::read(float& volts, float& milliamps)
{
    uint16_t shunt = 0;
    uint16_t bus   = 0;
    if (!read_reg(kRegShuntVoltage, shunt) || !read_reg(kRegBusVoltage, bus)) {
        return false;
    }
    const float shunt_volts = static_cast<int16_t>(shunt) * 2.5e-6f;
    milliamps               = shunt_volts / kShuntOhm * 1000.0f;
    volts                   = bus * 1.25e-3f;
    return true;
}

bool Ina226::read_reg(uint8_t reg, uint16_t& value)
{
    if (!_dev) {
        return false;
    }
    uint8_t data[2] = {0, 0};
    if (i2c_master_transmit_receive(_dev, &reg, 1, data, sizeof(data), kTimeoutMs) != ESP_OK) {
        return false;
    }
    value = static_cast<uint16_t>(data[0] << 8 | data[1]);  // big-endian
    return true;
}

}  // namespace sc

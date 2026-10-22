/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <driver/i2c_master.h>
#include <cstdint>

namespace sc {

/**
 * @brief Read-only driver for the INA226 battery monitor in the StackChan body (0x41).
 *
 * The 10 mOhm shunt sits between the cell connector and BAT+, the node that feeds both the servo
 * boost converter and the CoreS3 AXP2101, so it sees the real cell current including servo load.
 * The chip is left in its power-on configuration (continuous shunt and bus conversion), no
 * calibration register is needed for raw shunt and bus readings. Errors are returned, never asserted.
 */
class Ina226 {
public:
    static constexpr uint8_t kDefaultAddress = 0x41;

    ~Ina226();

    bool begin(i2c_master_bus_handle_t bus, uint8_t address = kDefaultAddress);
    // Cell voltage (V) and current through the shunt (mA). The sign convention follows the shunt
    // wiring and has not been confirmed on the device yet.
    bool read(float& volts, float& milliamps);

private:
    bool read_reg(uint8_t reg, uint16_t& value);

    i2c_master_dev_handle_t _dev = nullptr;
};

}  // namespace sc

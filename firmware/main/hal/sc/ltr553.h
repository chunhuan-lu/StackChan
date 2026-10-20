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
 * @brief Minimal ambient light driver for the LTR-553ALS-WA on the CoreS3 internal I2C bus.
 *
 * Only the ALS part is used (proximity stays in standby). Errors are returned, never asserted,
 * because the bus is shared with the audio codecs and the touch controller.
 */
class Ltr553 {
public:
    static constexpr uint8_t kDefaultAddress = 0x23;

    ~Ltr553();

    bool begin(i2c_master_bus_handle_t bus, uint8_t address = kDefaultAddress);
    // Lux estimate from the latest completed measurement (gain 8x, 100 ms integration).
    bool readLux(float& lux);

private:
    bool write_reg(uint8_t reg, uint8_t value);
    bool read_regs(uint8_t reg, uint8_t* data, size_t len);

    i2c_master_dev_handle_t _dev = nullptr;
};

}  // namespace sc

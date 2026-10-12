/**
 * @file pm1.h
 * @brief M5PM1 power management chip driver (M5StickS3)
 * @details Register map taken from M5Unified M5PM1_Class
 */
#pragma once

#include <cstdint>
#include "driver/i2c_master.h"

#define PM1_I2C_ADDR 0x6E
#define PM1_I2C_FREQ 100000
#define PM1_I2C_TIMEOUT_MS 100

// PM1 GPIO assignment on M5StickS3
#define PM1_GPIO_CHG_STAT 0 // input, low = charging
#define PM1_GPIO_LCD_EN 2   // output, high = LCD power on
#define PM1_GPIO_SPK_EN 3   // output, high = speaker amplifier on

namespace HAL
{
    class Hal;

    class PM1
    {
    public:
        PM1(Hal* hal);
        ~PM1();

        bool init();
        bool is_initialized() const { return _dev_handle != nullptr; }

        bool gpio_set_output(uint8_t pin, bool high);
        bool gpio_set_input(uint8_t pin);
        bool gpio_write(uint8_t pin, bool high);
        bool gpio_read(uint8_t pin);

        /// Battery voltage in millivolts, 0 on read error
        uint16_t get_battery_mv();
        bool is_charging();
        void power_off();

    private:
        Hal* _hal;
        i2c_master_dev_handle_t _dev_handle = nullptr;

        bool _write_reg(uint8_t reg, uint8_t val);
        bool _read_reg(uint8_t reg, uint8_t* data, size_t len);
        bool _update_bits(uint8_t reg, uint8_t mask, uint8_t val);
    };

} // namespace HAL

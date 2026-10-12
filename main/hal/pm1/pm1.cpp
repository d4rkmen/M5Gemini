/**
 * @file pm1.cpp
 * @brief M5PM1 power management chip driver (M5StickS3)
 */
#include "pm1.h"
#include "hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "PM1";

#define PM1_REG_DEVICE_ID 0x00
#define PM1_REG_I2C_CFG 0x09
#define PM1_REG_WDT_CNT 0x0A
#define PM1_REG_SYS_CMD 0x0C
#define PM1_REG_GPIO_MODE 0x10
#define PM1_REG_GPIO_OUT 0x11
#define PM1_REG_GPIO_IN 0x12
#define PM1_REG_GPIO_DRV 0x13
#define PM1_REG_GPIO_FUNC0 0x16
#define PM1_REG_GPIO_FUNC1 0x17
#define PM1_REG_VBAT_L 0x22

#define PM1_SYS_CMD_SHUTDOWN 0xA1

namespace HAL
{
    PM1::PM1(Hal* hal) : _hal(hal) {}

    PM1::~PM1()
    {
        if (_dev_handle && _hal && _hal->i2c())
        {
            _hal->i2c()->remove_device(_dev_handle);
            _dev_handle = nullptr;
        }
    }

    bool PM1::init()
    {
        if (!_hal || !_hal->i2c() || !_hal->i2c()->is_initialized())
        {
            ESP_LOGE(TAG, "I2C not available");
            return false;
        }

        esp_err_t err = _hal->i2c()->add_device(PM1_I2C_ADDR, PM1_I2C_FREQ, nullptr, &_dev_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
            return false;
        }

        // The chip may be in I2C idle sleep (it keeps its settings while on battery),
        // so the first transfers can be NACKed until it wakes up
        uint8_t id[2] = {0, 0};
        bool found = false;
        for (int i = 0; i < 5 && !found; i++)
        {
            found = _read_reg(PM1_REG_DEVICE_ID, id, sizeof(id));
            if (!found)
                vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!found)
        {
            ESP_LOGE(TAG, "Not responding");
            _hal->i2c()->remove_device(_dev_handle);
            _dev_handle = nullptr;
            return false;
        }

        _write_reg(PM1_REG_I2C_CFG, 0x00); // disable I2C idle sleep
        _write_reg(PM1_REG_WDT_CNT, 0x00); // disable watchdog

        ESP_LOGI(TAG, "Initialized, device id 0x%02X%02X", id[1], id[0]);
        return true;
    }

    bool PM1::gpio_set_output(uint8_t pin, bool high)
    {
        if (pin > 4)
            return false;
        const uint8_t reg = pin < 4 ? PM1_REG_GPIO_FUNC0 : PM1_REG_GPIO_FUNC1;
        const uint8_t shift = (pin < 4 ? pin : pin - 4) * 2;
        const uint8_t mask = 1 << pin;
        // Latch the level first so the pin does not glitch when it becomes an output
        return _update_bits(reg, 0x03 << shift, 0) &&            // plain GPIO function
               _update_bits(PM1_REG_GPIO_DRV, mask, 0) &&        // push-pull
               _update_bits(PM1_REG_GPIO_OUT, mask, high ? mask : 0) &&
               _update_bits(PM1_REG_GPIO_MODE, mask, mask);      // output
    }

    bool PM1::gpio_set_input(uint8_t pin)
    {
        if (pin > 4)
            return false;
        const uint8_t reg = pin < 4 ? PM1_REG_GPIO_FUNC0 : PM1_REG_GPIO_FUNC1;
        const uint8_t shift = (pin < 4 ? pin : pin - 4) * 2;
        return _update_bits(reg, 0x03 << shift, 0) && _update_bits(PM1_REG_GPIO_MODE, 1 << pin, 0);
    }

    bool PM1::gpio_write(uint8_t pin, bool high)
    {
        if (pin > 4)
            return false;
        const uint8_t mask = 1 << pin;
        return _update_bits(PM1_REG_GPIO_OUT, mask, high ? mask : 0);
    }

    bool PM1::gpio_read(uint8_t pin)
    {
        uint8_t val = 0;
        if (pin > 4 || !_read_reg(PM1_REG_GPIO_IN, &val, 1))
            return false;
        return val & (1 << pin);
    }

    uint16_t PM1::get_battery_mv()
    {
        uint8_t buf[2] = {0, 0};
        if (!_read_reg(PM1_REG_VBAT_L, buf, sizeof(buf)))
            return 0;
        return (buf[1] << 8) | buf[0];
    }

    bool PM1::is_charging()
    {
        if (!_dev_handle)
            return false;
        return !gpio_read(PM1_GPIO_CHG_STAT);
    }

    void PM1::power_off() { _write_reg(PM1_REG_SYS_CMD, PM1_SYS_CMD_SHUTDOWN); }

    // ── Internals ──────────────────────────────────────────────

    bool PM1::_write_reg(uint8_t reg, uint8_t val)
    {
        if (!_dev_handle)
            return false;
        const uint8_t buf[2] = {reg, val};
        esp_err_t err = i2c_master_transmit(_dev_handle, buf, 2, PM1_I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Reg 0x%02X write failed: %s", reg, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    bool PM1::_read_reg(uint8_t reg, uint8_t* data, size_t len)
    {
        if (!_dev_handle)
            return false;
        esp_err_t err = i2c_master_transmit_receive(_dev_handle, &reg, 1, data, len, PM1_I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGD(TAG, "Reg 0x%02X read failed: %s", reg, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    bool PM1::_update_bits(uint8_t reg, uint8_t mask, uint8_t val)
    {
        uint8_t cur = 0;
        if (!_read_reg(reg, &cur, 1))
            return false;
        const uint8_t next = (cur & ~mask) | (val & mask);
        return next == cur ? true : _write_reg(reg, next);
    }

} // namespace HAL

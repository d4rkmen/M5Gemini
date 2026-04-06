/**
 * @file es8311.cpp
 * @brief ES8311 audio codec driver (shared between speaker and mic)
 */
#include "es8311.h"
#include "hal.h"
#include "esp_log.h"

static const char* TAG = "ES8311";

namespace HAL
{
    ES8311::ES8311(Hal* hal) : _hal(hal) {}

    ES8311::~ES8311()
    {
        if (_dev_handle && _hal && _hal->i2c())
        {
            _hal->i2c()->remove_device(_dev_handle);
            _dev_handle = nullptr;
        }
    }

    bool ES8311::init()
    {
        if (!_hal || !_hal->i2c() || !_hal->i2c()->is_initialized())
        {
            ESP_LOGE(TAG, "I2C not available");
            return false;
        }

        esp_err_t err = _hal->i2c()->add_device(ES8311_I2C_ADDR, 400000, nullptr, &_dev_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
            return false;
        }

        ESP_LOGI(TAG, "Initialized");
        return true;
    }

    // ── Speaker (DAC) ──────────────────────────────────────────────

    bool ES8311::speaker_enable()
    {
        static const uint8_t regs[][2] = {
            {0x00, 0x80}, // RESET / CSM power on
            {0x01, 0xB5}, // CLOCK_MANAGER / MCLK=BCLK
            {0x02, 0x18}, // CLOCK_MANAGER / MULT_PRE=3
            {0x0D, 0x01}, // SYSTEM / power up analog circuitry
            {0x12, 0x00}, // SYSTEM / power-up DAC
            {0x13, 0x10}, // SYSTEM / enable output to HP drive
            {0x32, 0x00}, // DAC volume muted (unmuted later after I2S starts)
            {0x37, 0x08}, // DAC / bypass DAC equalizer
        };

        if (!_write_regs(regs, sizeof(regs) / sizeof(regs[0])))
        {
            ESP_LOGE(TAG, "speaker_enable failed");
            return false;
        }
        ESP_LOGD(TAG, "Speaker enabled");
        return true;
    }

    void ES8311::speaker_disable()
    {
        static const uint8_t regs[][2] = {
            {0x32, 0x00}, // DAC volume mute
            {0x12, 0x02}, // SYSTEM / power-down DAC
        };
        _write_regs(regs, sizeof(regs) / sizeof(regs[0]));
        ESP_LOGD(TAG, "Speaker disabled");
    }

    void ES8311::speaker_mute()
    {
        _write_reg(0x32, 0x00);
    }

    void ES8311::speaker_unmute()
    {
        _write_reg(0x32, 0xBF);
    }

    // ── Mic (ADC) ──────────────────────────────────────────────────

    bool ES8311::mic_enable()
    {
        static const uint8_t regs[][2] = {
            {0x00, 0x80}, // RESET / CSM power on
            {0x01, 0xBA}, // CLOCK_MANAGER / MCLK=BCLK
            {0x02, 0x18}, // CLOCK_MANAGER / MULT_PRE=3
            {0x0D, 0x01}, // SYSTEM / power up analog circuitry
            {0x0E, 0x02}, // SYSTEM / enable analog PGA, enable ADC modulator
            {0x14, 0x10}, // ADC / select Mic1p-Mic1n, PGA gain minimum
            {0x17, 0xBF}, // ADC / volume 0 dB
            {0x1C, 0x6A}, // ADC / equalizer bypass, cancel DC offset
        };

        if (!_write_regs(regs, sizeof(regs) / sizeof(regs[0])))
        {
            ESP_LOGE(TAG, "mic_enable failed");
            return false;
        }
        ESP_LOGI(TAG, "Mic enabled");
        return true;
    }

    void ES8311::mic_disable()
    {
        static const uint8_t regs[][2] = {
            {0x0D, 0xFC}, // SYSTEM / power down analog circuitry
            {0x0E, 0x6A}, // SYSTEM
            {0x00, 0x00}, // RESET / CSM power down
        };
        _write_regs(regs, sizeof(regs) / sizeof(regs[0]));
        ESP_LOGI(TAG, "Mic disabled");
    }

    // ── Internals ──────────────────────────────────────────────────

    bool ES8311::_write_reg(uint8_t reg, uint8_t val)
    {
        if (!_dev_handle)
            return false;
        const uint8_t buf[2] = {reg, val};
        esp_err_t err = i2c_master_transmit(_dev_handle, buf, 2, ES8311_I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Reg 0x%02X write failed: %s", reg, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    bool ES8311::_write_regs(const uint8_t (*pairs)[2], size_t count)
    {
        for (size_t i = 0; i < count; i++)
        {
            if (!_write_reg(pairs[i][0], pairs[i][1]))
                return false;
        }
        return true;
    }

} // namespace HAL

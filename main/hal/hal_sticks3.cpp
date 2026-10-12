/**
 * @file hal_sticks3.cpp
 * @brief HAL for M5StickS3
 */
#include "hal_sticks3.h"
#include "hal_config.h"
#include "common_define.h"
#if HAL_USE_DISPLAY
#include "display/display_sticks3.hpp"
#endif
#include "esp_log.h"

static const char* TAG = "HAL";

using namespace HAL;

void HalStickS3::_init_power()
{
    ESP_LOGI(TAG, "init power");
    _pm1 = new PM1(this);
    if (!_pm1->init())
    {
        ESP_LOGE(TAG, "M5PM1 not found, LCD and speaker amplifier stay off");
        return;
    }
    _pm1->gpio_set_output(PM1_GPIO_SPK_EN, false);
    _pm1->gpio_set_input(PM1_GPIO_CHG_STAT);
    _pm1->gpio_set_output(PM1_GPIO_LCD_EN, true);
    // let the LCD supply settle before the panel is reset
    delay(100);
    ESP_LOGI(TAG, "battery %d mV, %s", _pm1->get_battery_mv(), _pm1->is_charging() ? "charging" : "not charging");
}

#if HAL_USE_DISPLAY
void HalStickS3::_init_display()
{
    ESP_LOGI(TAG, "init display");

    _display = new LGFX_StickS3;
    _canvas = new LGFX_Sprite(_display);
    _canvas->createSprite(_display->width(), _display->height());
}
#endif

#if HAL_USE_MIC
void HalStickS3::_init_mic()
{
    ESP_LOGI(TAG, "init mic");
    _mic = new Mic(this);

    // same codec setup as CardPuter ADV, different pins
    auto cfg = _mic->config();
    cfg.pin_bck = 17;
    cfg.pin_ws = 15;
    cfg.pin_data_in = 16;
    cfg.over_sampling = 1;
    cfg.magnification = 220;
    _mic->config(cfg);
    ESP_LOGI(TAG, "StickS3: mic uses ES8311 I2S codec (bck=17, ws=15, din=16)");
}
#endif

void HalStickS3::init()
{
    ESP_LOGI(TAG, "HAL init");

#if HAL_USE_I2C
    ESP_LOGI(TAG, "init i2c");
    _i2c = new I2CMaster();
#endif
    _init_power();
#if HAL_USE_DISPLAY
    _init_display();
#endif
#if HAL_USE_KEYBOARD
    ESP_LOGI(TAG, "init buttons");
    _keyboard = new KEYBOARD::Keyboard(this);
#endif
    _es8311 = new ES8311(this);
    _es8311->init();
#if HAL_USE_SPEAKER
    ESP_LOGI(TAG, "init speaker");
    _speaker = new Speaker(this);
    // the gain follows the power source from here on
    _update_speaker_gain(true);
    xTaskCreate(_power_monitor_task, "pwr_mon", 3072, this, 3, nullptr);
    // amplifier wake-up plus the codec volume ramp; without this the boot sound is swallowed
    delay(200);
#endif
#if HAL_USE_MIC
    _init_mic();
#endif
#if HAL_USE_BUTTON
    // GPIO0 is only a boot strap here, so this never reads as pressed
    _homeButton = new Button(0);
#endif
#if HAL_USE_WIFI
    ESP_LOGI(TAG, "init wifi");
    _wifi = new WiFi(_settings);
#endif
}

#if HAL_USE_BAT
uint8_t HalStickS3::getBatLevel(float voltage) { return Battery::get_level(voltage); }

float HalStickS3::getBatVoltage() { return _pm1 ? _pm1->get_battery_mv() / 1000.0f : 0.0f; }

bool HalStickS3::isCharging() { return _pm1 && _pm1->is_charging(); }
#endif

#if HAL_USE_SPEAKER
// Speaker output gain by power source.
//
// The speaker driver's default x16 is the Cardputer's. Here the ES8311 already plays
// at 0 dB into the amplifier, so high gain clips hard and the amplifier draws its peak
// current for long stretches. The 250 mAh cell cannot supply that together with the
// WiFi bursts: its voltage sags and the chip takes a brownout reset. Measured on
// battery at full volume: x4 is stable on a near-full cell, x6 resets.
// A weaker cell sags sooner, so the gain steps down as it empties.
static constexpr uint8_t GAIN_USB = 8;
static constexpr struct
{
    uint16_t min_mv;
    uint8_t gain;
} GAIN_BATTERY[] = {{3850, 4}, {3700, 3}, {0, 2}};
static constexpr uint16_t GAIN_HYSTERESIS_MV = 50;

void HalStickS3::_update_speaker_gain(bool log_change)
{
    if (!_speaker || !_pm1 || !_pm1->is_initialized())
        return;

    static uint16_t battery_mv = 0;
    static uint32_t next_battery_read = 0;

    const bool usb = _pm1->is_usb_powered();
    uint8_t gain = GAIN_USB;
    if (!usb)
    {
        // the cell voltage moves slowly: read it now and then, the power source every time
        const uint32_t now = millis();
        if (battery_mv == 0 || now >= next_battery_read)
        {
            const uint16_t mv = _pm1->get_battery_mv();
            if (mv > 0)
                battery_mv = mv;
            next_battery_read = now + 5000;
        }

        gain = GAIN_BATTERY[0].gain;
        for (const auto& step : GAIN_BATTERY)
        {
            gain = step.gain;
            // going back up to a louder step needs the voltage clearly above its threshold
            const uint16_t threshold = step.min_mv + (step.gain > _speaker_gain ? GAIN_HYSTERESIS_MV : 0);
            if (battery_mv >= threshold)
                break;
        }
    }

    if (gain != _speaker_gain)
    {
        _speaker_gain = gain;
        _speaker->setMagnification(gain);
        if (log_change)
            ESP_LOGI(TAG, "speaker gain x%d (%s, battery %d mV)", gain, usb ? "USB" : "battery", battery_mv);
    }
}

void HalStickS3::_power_monitor_task(void* arg)
{
    auto* self = static_cast<HalStickS3*>(arg);
    while (true)
    {
        // short interval: when the cable is pulled the gain has to come down before
        // the next loud passage does it for us
        delay(250);
        self->_update_speaker_gain(true);
    }
}
#endif

void HalStickS3::setSpeakerAmp(bool enabled)
{
    if (_pm1 && _pm1->is_initialized())
        _pm1->gpio_write(PM1_GPIO_SPK_EN, enabled);
}

void HalStickS3::powerOff()
{
    ESP_LOGW(TAG, "Power off...");
    setSpeakerAmp(false);
#if HAL_USE_DISPLAY
    _display->setBrightness(0);
#endif
    delay(100);
    if (_pm1)
        _pm1->power_off();
}

void HalStickS3::reboot()
{
    ESP_LOGW(TAG, "Rebooting...");
#if HAL_USE_WIFI
    wifi()->set_status_callback(nullptr);
#endif
    setSpeakerAmp(false);
    delay(100);
    esp_restart();
}

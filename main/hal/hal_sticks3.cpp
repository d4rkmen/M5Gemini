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

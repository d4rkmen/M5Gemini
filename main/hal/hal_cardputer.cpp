/**
 * @file hal_cardputer.cpp
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-22
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "hal_cardputer.h"
#include "app/utils/common_define.h"
#ifdef HAVE_BATTERY
#include "bat/adc_read.h"
#endif
#include "esp_log.h"

static const char* TAG = "HAL";

using namespace HAL;

void HalCardputer::_init_display()
{
    ESP_LOGI(TAG, "init display");

    // Display
    _display = new M5GFX;
    _display->init();
    // Canvas
    _canvas = new LGFX_Sprite(_display);
    _canvas->createSprite(_display->width(), _display->height());
    //_canvas->setFont(&fonts::efontEN_10);
}

void HalCardputer::_init_keyboard()
{
    _keyboard = new KEYBOARD::Keyboard;
    _keyboard->init();
}
#ifdef HAVE_MIC
void HalCardputer::_init_mic()
{
    ESP_LOGI(TAG, "init mic");

    _mic = new m5::Mic_Class;

    // Configs
    auto cfg = _mic->config();
    cfg.pin_data_in = 46;
    cfg.pin_ws = 43;
    cfg.magnification = 4;

    cfg.task_priority = 15;

    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 256;
    cfg.stereo = false;
    cfg.sample_rate = 8000;
    cfg.over_sampling = 2;

    cfg.i2s_port = i2s_port_t::I2S_NUM_0;
    _mic->config(cfg);
}
#endif
#ifdef HAVE_SPEAKER
void HalCardputer::_init_speaker()
{
    ESP_LOGI(TAG, "init speaker");

    _speaker = new m5::Speaker_Class;

    auto cfg = _speaker->config();
    cfg.pin_data_out = 42;
    cfg.pin_bck = 41;
    cfg.pin_ws = 43;
    cfg.i2s_port = i2s_port_t::I2S_NUM_1;
    cfg.task_priority = 15;
    _speaker->config(cfg);
}
#endif
void HalCardputer::_init_button() { _homeButton = new Button(0); }
#ifdef HAVE_BATTERY
void HalCardputer::_init_bat() { adc_read_init(); }
#endif
#ifdef HAVE_SDCARD
void HalCardputer::_init_sdcard() { _sdcard = new SDCard; }
#endif
#ifdef HAVE_USB
void HalCardputer::_init_usb() { _usb = new USB(this); }
#endif
#ifdef HAVE_WIFI
void HalCardputer::_init_wifi() { _wifi = new WiFi(_settings); }
#endif
void HalCardputer::init()
{
    ESP_LOGI(TAG, "HAL init");

    _init_display();
    _init_keyboard();
#ifdef HAVE_SPEAKER
    _init_speaker();
#endif
#ifdef HAVE_MIC
    _init_mic();
#endif
    _init_button();
#ifdef HAVE_BATTERY
    _init_bat();
#endif
#ifdef HAVE_SDCARD
    _init_sdcard();
#endif
#ifdef HAVE_USB
    _init_usb();
#endif
#ifdef HAVE_WIFI
    _init_wifi();
#endif
}

#ifdef HAVE_BATTERY
uint8_t HalCardputer::getBatLevel()
{
    // https://docs.m5stack.com/zh_CN/core/basic_v2.7
    double bat_v = static_cast<double>(adc_read_get_value()) * 2 / 1000;
    uint8_t result = 0;
    if (bat_v >= 4.12)
        result = 100;
    else if (bat_v >= 3.88)
        result = 75;
    else if (bat_v >= 3.61)
        result = 50;
    else if (bat_v >= 3.40)
        result = 25;
    else
        result = 0;
    return result;
}

double HalCardputer::getBatVoltage() { return static_cast<double>(adc_read_get_value()) * 2 / 1000; }
#endif
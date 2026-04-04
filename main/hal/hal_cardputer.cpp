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
#include "hal_config.h"
#include "common_define.h"
#if HAL_USE_DISPLAY
#include "display/display.hpp"
#endif
#include "esp_log.h"
#include "driver/gpio.h"

static const char* TAG = "HAL";

using namespace HAL;

#if HAL_USE_I2C
void HalCardputer::_init_i2c()
{
    ESP_LOGI(TAG, "init i2c");
    _i2c = new I2CMaster();
}
#endif

#if HAL_USE_DISPLAY
static constexpr size_t EMOJI_CACHE_CAP = 10;
static struct EmojiCacheEntry
{
    uint32_t code = 0;
    uint8_t* data = nullptr;
    uint32_t len = 0;
    int16_t png_w = 0;
    int16_t png_h = 0;
} s_emoji_cache[EMOJI_CACHE_CAP];
static uint8_t s_emoji_cache_n = 0;

static const EmojiCacheEntry* emoji_cache_lookup(uint32_t code)
{
    for (uint8_t i = 0; i < s_emoji_cache_n; i++)
    {
        if (s_emoji_cache[i].code == code)
            return &s_emoji_cache[i];
    }

    char path[48];
    snprintf(path, sizeof(path), "/sdcard/emoji/u%lX.png", code);

    EmojiCacheEntry entry;
    entry.code = code;

    FILE* f = fopen(path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz > 24 && sz < 64 * 1024)
        {
            entry.data = (uint8_t*)malloc(sz);
            if (entry.data)
            {
                fseek(f, 0, SEEK_SET);
                if ((long)fread(entry.data, 1, sz, f) == sz)
                {
                    entry.len = (uint32_t)sz;
                    entry.png_w = (int16_t)((entry.data[18] << 8) | entry.data[19]);
                    entry.png_h = (int16_t)((entry.data[22] << 8) | entry.data[23]);
                }
                else
                {
                    free(entry.data);
                    entry.data = nullptr;
                }
            }
        }
        fclose(f);
    }

    if (s_emoji_cache_n < EMOJI_CACHE_CAP)
    {
        s_emoji_cache[s_emoji_cache_n++] = entry;
    }
    else
    {
        free(s_emoji_cache[0].data);
        memmove(&s_emoji_cache[0], &s_emoji_cache[1], sizeof(s_emoji_cache[0]) * (EMOJI_CACHE_CAP - 1));
        s_emoji_cache[EMOJI_CACHE_CAP - 1] = entry;
    }
    return &s_emoji_cache[s_emoji_cache_n - 1];
}

static int32_t emoji_draw_callback(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint32_t code, int32_t font_height)
{
    auto* e = emoji_cache_lookup(code);
    if (!e->data || e->png_h <= 0)
        return 0;

    float scale = (float)font_height / (float)e->png_h;
    if (!gfx->drawPng(e->data, e->len, x, y - (int32_t)((font_height * 90.0f) / 100.0f), 0, 0, 0, 0, scale, 0))
        return 0;

    return (int32_t)(e->png_w * scale);
}

void HalCardputer::_init_display()
{
    ESP_LOGI(TAG, "init display");

    _display = new LGFX;
    _canvas = new LGFX_Sprite(_display);
    _canvas->createSprite(_display->width(), _display->height());

    _display->setEmojiCallback(emoji_draw_callback);
    _canvas->setEmojiCallback(emoji_draw_callback);
}
#endif

#if HAL_USE_KEYBOARD
void HalCardputer::_init_keyboard()
{
    ESP_LOGI(TAG, "init keyboard");
    _keyboard = new KEYBOARD::Keyboard(this);
    _board_type = _keyboard->boardType();
}
#endif

#if HAL_USE_SPEAKER
void HalCardputer::_init_speaker()
{
    ESP_LOGI(TAG, "init speaker");
    _speaker = new Speaker(this);
}
#endif

#if HAL_USE_MIC
void HalCardputer::_init_mic()
{
    ESP_LOGI(TAG, "init mic");
    _mic = new Mic();
}
#endif

#if HAL_USE_BUTTON
void HalCardputer::_init_button()
{
    ESP_LOGI(TAG, "init button");
    _homeButton = new Button(0);
}
#endif

#if HAL_USE_BAT
void HalCardputer::_init_bat()
{
    ESP_LOGI(TAG, "init battery");
    _battery = new Battery();
}
#endif

#if HAL_USE_SDCARD
void HalCardputer::_init_sdcard()
{
    ESP_LOGI(TAG, "init sdcard");
    _sdcard = new SDCard;
}
#endif

#if HAL_USE_LED
void HalCardputer::_init_led()
{
    ESP_LOGI(TAG, "init led");
    _led = new LED(RGB_LED_GPIO);
    _led->off();
}
#endif

#if HAL_USE_WIFI
void HalCardputer::_init_wifi()
{
    ESP_LOGI(TAG, "init wifi");
    _wifi = new WiFi(_settings);
}
#endif

void HalCardputer::init()
{
    ESP_LOGI(TAG, "HAL init");

#if HAL_USE_I2C
    _init_i2c();
#endif
#if HAL_USE_DISPLAY
    _init_display();
#endif
#if HAL_USE_KEYBOARD
    _init_keyboard();
#endif
#if HAL_USE_SPEAKER
    _init_speaker();
#endif
#if HAL_USE_MIC
    _init_mic();
#endif
#if HAL_USE_BUTTON
    _init_button();
#endif
#if HAL_USE_BAT
    _init_bat();
#endif
    // CardPuter ADV has SX1262 radio sharing SPI2 with SD card;
    // deselect its CS so it doesn't drive MISO during SD transactions
    if (_board_type == BoardType::CARDPUTER_ADV)
    {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << GPIO_NUM_5);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(GPIO_NUM_5, 1);
    }

#if HAL_USE_SDCARD
    _init_sdcard();
#endif
#if HAL_USE_LED
    _init_led();
#endif
#if HAL_USE_WIFI
    _init_wifi();
#endif
}

#if HAL_USE_BAT
uint8_t HalCardputer::getBatLevel(float voltage)
{
    uint8_t result = 0;
    if (voltage >= 4.12)
        result = 100;
    else if (voltage >= 3.88)
        result = 75;
    else if (voltage >= 3.61)
        result = 50;
    else if (voltage >= 3.40)
        result = 25;
    else
        result = 0;
    return result;
}

float HalCardputer::getBatVoltage() { return static_cast<float>(_battery->get_voltage()); }
#endif

void HalCardputer::reboot()
{
    ESP_LOGW(TAG, "Rebooting...");
#if HAL_USE_WIFI
    wifi()->set_status_callback(nullptr);
#endif
    delay(100);
#if HAL_USE_LED
    led()->off();
#endif
    esp_restart();
}

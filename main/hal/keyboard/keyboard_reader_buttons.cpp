/**
 * @file keyboard_reader_buttons.cpp
 * @brief Two-button keyboard reader for M5StickS3
 */
#include "keyboard_reader_buttons.h"
#include <driver/gpio.h>
#include "common_define.h"

using namespace KEYBOARD;

void ButtonsKeyboardReader::init()
{
    for (auto& button : _buttons)
    {
        gpio_reset_pin((gpio_num_t)button.pin);
        gpio_set_direction((gpio_num_t)button.pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)button.pin, GPIO_PULLUP_ONLY);
    }
}

void ButtonsKeyboardReader::update()
{
    _key_list.clear();
    const uint32_t now = millis();

    for (auto& button : _buttons)
    {
        const bool down = gpio_get_level((gpio_num_t)button.pin) == 0;
        if (down != button.down && now >= button.ignore_until)
        {
            button.ignore_until = now + DEBOUNCE_MS;
            button.down = down;
            if (down)
            {
                button.down_since = now;
                button.long_fired = false;
            }
            else if (!button.long_fired)
            {
                _pulse_key = button.short_key;
                _pulse_until = now + SHORT_PULSE_MS;
            }
        }

        if (button.down && (now - button.down_since) >= LONG_PRESS_MS)
        {
            button.long_fired = true;
            _key_list.push_back(button.long_key);
        }
    }

    if (now < _pulse_until)
    {
        _key_list.push_back(_pulse_key);
    }
}

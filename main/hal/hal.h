/**
 * @file hal.h
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-18
 *
 * @copyright Copyright (c) 2023
 *
 */
#pragma once
#include "hal_config.h"
#include "board.h"
#if HAL_USE_DISPLAY
#include "LovyanGFX.h"
#endif
#include "settings/settings.h"
#include <iostream>
#include <string>

#if HAL_USE_I2C
#include "i2c/i2c_master.h"
#endif
#if HAL_USE_KEYBOARD
#include "keyboard/keyboard.h"
#endif
#if HAL_USE_BAT
#include "bat/battery.h"
#endif
#if HAL_USE_SDCARD
#include "sdcard/sdcard.h"
#endif
#if HAL_USE_BUTTON
#include "button/Button.h"
#endif
#if HAL_USE_SPEAKER
#include "speaker/speaker.h"
#endif
#if HAL_USE_MIC
#include "mic/mic.h"
#endif
#if HAL_USE_LED
#include "led/led.h"
#endif
#if HAL_USE_WIFI
#include "wifi/wifi.h"
#endif
#include "es8311/es8311.h"

namespace HAL
{
    /**
     * @brief Hal base class
     *
     */
    class Hal
    {
    protected:
        SETTINGS::Settings* _settings;
        BoardType _board_type;

#if HAL_USE_DISPLAY
        LGFX_Device* _display;
        LGFX_Sprite* _canvas;
        bool _display_sleeping = false;
#endif
#if HAL_USE_KEYBOARD
        KEYBOARD::Keyboard* _keyboard;
#endif
#if HAL_USE_I2C
        I2CMaster* _i2c;
#endif
#if HAL_USE_BAT
        Battery* _battery;
#endif
#if HAL_USE_SPEAKER
        Speaker* _speaker;
#endif
#if HAL_USE_MIC
        Mic* _mic;
#endif
#if HAL_USE_BUTTON
        Button* _homeButton;
#endif
#if HAL_USE_SDCARD
        SDCard* _sdcard;
#endif
#if HAL_USE_LED
        LED* _led;
#endif
#if HAL_USE_WIFI
        WiFi* _wifi;
#endif
        ES8311* _es8311 = nullptr;

    public:
        Hal(SETTINGS::Settings* settings)
            : _settings(settings), _board_type(BoardType::AUTO_DETECT)
#if HAL_USE_KEYBOARD
              ,
              _keyboard(nullptr)
#endif
#if HAL_USE_I2C
              ,
              _i2c(nullptr)
#endif
#if HAL_USE_BAT
              ,
              _battery(nullptr)
#endif
#if HAL_USE_SPEAKER
              ,
              _speaker(nullptr)
#endif
#if HAL_USE_MIC
              ,
              _mic(nullptr)
#endif
#if HAL_USE_BUTTON
              ,
              _homeButton(nullptr)
#endif
#if HAL_USE_SDCARD
              ,
              _sdcard(nullptr)
#endif
#if HAL_USE_LED
              ,
              _led(nullptr)
#endif
#if HAL_USE_WIFI
              ,
              _wifi(nullptr)
#endif
        {
        }

        // Getters
        inline SETTINGS::Settings* settings() { return _settings; }
        inline BoardType board_type() const { return _board_type; }

#if HAL_USE_DISPLAY
        inline LGFX_Device* display() { return _display; }
        inline LGFX_Sprite* canvas() { return _canvas; }
        inline void canvas_update()
        {
            if (!_display_sleeping)
                _canvas->pushSprite(0, 0);
        }
        inline bool isDisplaySleeping() const { return _display_sleeping; }
        inline void displaySleep()
        {
            if (!_display_sleeping)
            {
                _display_sleeping = true;
                _display->sleep();
            }
        }
        inline void displayWakeup()
        {
            if (_display_sleeping)
            {
                _display_sleeping = false;
                _display->wakeup();
            }
        }
#endif
#if HAL_USE_KEYBOARD
        inline KEYBOARD::Keyboard* keyboard() { return _keyboard; }
#endif
#if HAL_USE_I2C
        inline I2CMaster* i2c() { return _i2c; }
#endif
#if HAL_USE_BAT
        inline Battery* bat() { return _battery; }
#endif
#if HAL_USE_SPEAKER
        inline Speaker* speaker() { return _speaker; }
#endif
#if HAL_USE_MIC
        inline Mic* mic() { return _mic; }
#endif
#if HAL_USE_BUTTON
        inline Button* home_button() { return _homeButton; }
#endif
#if HAL_USE_SDCARD
        inline SDCard* sdcard() { return _sdcard; }
#endif
#if HAL_USE_LED
        inline LED* led() { return _led; }
#endif
#if HAL_USE_WIFI
        inline WiFi* wifi() { return _wifi; }
#endif
        inline ES8311* es8311() { return _es8311; }

        // Override
        virtual std::string type() { return "null"; }
        virtual void init() {}

#if HAL_USE_SPEAKER
        virtual void playLastSound() {}
        virtual void playNextSound() {}
        virtual void playKeyboardSound() {}
        virtual void playErrorSound() {}
        virtual void playDeviceConnectedSound() {}
        virtual void playDeviceDisconnectedSound() {}
#endif

#if HAL_USE_BAT
        virtual uint8_t getBatLevel(float voltage) { return 100; }
        virtual float getBatVoltage() { return 4.15; }
#endif
        virtual void reboot() {}
    };
} // namespace HAL

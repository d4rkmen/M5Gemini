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
#include "M5GFX.h"
#include "keyboard/keyboard.h"
#ifdef HAVE_SDCARD
#include "sdcard/sdcard.h"
#endif
#include "button/Button.h"
#include "M5Unified.h"
#ifdef HAVE_USB
#include "usb/usb.h"
#endif
#ifdef HAVE_WIFI
#include "wifi/wifi.h"
#endif
#ifdef HAVE_SETTINGS
#include "settings/settings.h"
#endif
#include <iostream>
#include <string>

namespace HAL
{
    /**
     * @brief Hal base for DI
     *
     */
    class Hal
    {
    protected:
        LGFX_Device* _display;
        LGFX_Sprite* _canvas;
        LGFX_Sprite* _canvas_system_bar;

#ifdef HAVE_SETTINGS
        SETTINGS::Settings* _settings;
#endif
        KEYBOARD::Keyboard* _keyboard;
#ifdef HAVE_MIC
        m5::Mic_Class* _mic;
#endif
#ifdef HAVE_SPEAKER
        m5::Speaker_Class* _speaker;
#endif
        Button* _homeButton;
#ifdef HAVE_SDCARD
        SDCard* _sdcard;
#endif
#ifdef HAVE_USB
        USB* _usb;
#endif
#ifdef HAVE_WIFI
        WiFi* _wifi;
#endif
    public:
        Hal(
#ifdef HAVE_SETTINGS
            SETTINGS::Settings* settings
#endif
            )
            : _display(nullptr), _canvas(nullptr), _canvas_system_bar(nullptr)
#ifdef HAVE_SETTINGS
              ,
              _settings(settings)
#endif
              ,
              _keyboard(nullptr)
#ifdef HAVE_MIC
              ,
              _mic(nullptr)
#endif
#ifdef HAVE_SPEAKER
              ,
              _speaker(nullptr)
#endif
              ,
              _homeButton(nullptr)
#ifdef HAVE_SDCARD
              ,
              _sdcard(nullptr)
#endif
#ifdef HAVE_USB
              ,
              _usb(nullptr)
#endif
#ifdef HAVE_WIFI
              ,
              _wifi(nullptr)
#endif
        {
            // constructor
        }

        // Getter
        inline LGFX_Device* display() { return _display; }
        inline LGFX_Sprite* canvas() { return _canvas; }
        inline LGFX_Sprite* canvas_system_bar() { return _canvas_system_bar; }
#ifdef HAVE_SETTINGS
        inline SETTINGS::Settings* settings() { return _settings; }
#endif
        inline KEYBOARD::Keyboard* keyboard() { return _keyboard; }
#ifdef HAVE_SDCARD
        inline SDCard* sdcard() { return _sdcard; }
#endif
#ifdef HAVE_USB
        inline USB* usb() { return _usb; }
#endif
        inline Button* homeButton() { return _homeButton; }
#ifdef HAVE_MIC
        inline m5::Mic_Class* mic() { return _mic; }
#endif
#ifdef HAVE_SPEAKER
        inline m5::Speaker_Class* speaker() { return _speaker; }
#endif
#ifdef HAVE_WIFI
        inline WiFi* wifi() { return _wifi; }
#endif
        // Canvas
        inline void canvas_system_bar_update() { _canvas_system_bar->pushSprite(0, 0); }
        inline void canvas_update() { _canvas->pushSprite(0, 0); }

        // Override
        virtual std::string type() { return "null"; }
        virtual void init() {}

#ifdef HAVE_SPEAKER
        virtual void playLastSound() {}
        virtual void playNextSound() {}
        virtual void playKeyboardSound() {}
        virtual void playErrorSound() {}
        virtual void playDeviceConnectedSound() {}
        virtual void playDeviceDisconnectedSound() {}
#endif
#ifdef HAVE_BATTERY
        virtual uint8_t getBatLevel() { return 100; }
        virtual double getBatVoltage() { return 4.15; }
#endif
    };
} // namespace HAL

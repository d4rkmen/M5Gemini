/**
 * @file hal_cardputer.h
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-22
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "hal.h"
#ifdef HAVE_SETTINGS
#include "settings/settings.h"
#endif

extern const uint8_t error_wav_start[] asm("_binary_error_wav_start");
extern const uint8_t error_wav_end[] asm("_binary_error_wav_end");

namespace HAL
{
    class HalCardputer : public Hal
    {
    private:
        void _init_display();
        void _init_keyboard();
#ifdef HAVE_MIC
        void _init_mic();
#endif
#ifdef HAVE_SPEAKER
#define SYSTEM_CHANNEL 0
#define AUDIO_CHANNEL 1
        void _init_speaker();
#endif
        void _init_button();
#ifdef HAVE_BATTERY
        void _init_bat();
#endif
#ifdef HAVE_SDCARD
        void _init_sdcard();
#endif
#ifdef HAVE_USB
        void _init_usb();
#endif
#ifdef HAVE_WIFI
        void _init_wifi();
#endif

    public:
        HalCardputer(
#ifdef HAVE_SETTINGS
            SETTINGS::Settings* settings
#endif
            )
            : Hal(
#ifdef HAVE_SETTINGS
                  settings
#endif
              )
        {
        }
        std::string type() override { return "cardputer"; }
        void init() override;
#ifdef HAVE_SPEAKER
        void setSystemtVolume()
        {
            _speaker->setVolume(255);
            _speaker->setChannelVolume(SYSTEM_CHANNEL, _settings->getNumber("system", "volume"));
        }
        void playErrorSound() override
        {
            setSystemtVolume();
            _speaker->playWav(error_wav_start, error_wav_end - error_wav_start, 1, SYSTEM_CHANNEL);
        }
        void playKeyboardSound() override
        {
            setSystemtVolume();
            _speaker->tone(5000, 20, SYSTEM_CHANNEL);
        }
        void playLastSound() override
        {
            setSystemtVolume();
            _speaker->tone(6000, 20, SYSTEM_CHANNEL);
        }
        void playNextSound() override
        {
            setSystemtVolume();
            _speaker->tone(7000, 20, SYSTEM_CHANNEL);
        }
        void playDeviceConnectedSound()
        {
            setSystemtVolume();
            _speaker->tone(1000, 100, SYSTEM_CHANNEL);
            vTaskDelay(50);
            _speaker->tone(1500, 200, SYSTEM_CHANNEL);
        }
        void playDeviceDisconnectedSound()
        {
            setSystemtVolume();
            _speaker->tone(1500, 100, SYSTEM_CHANNEL);
            vTaskDelay(50);
            _speaker->tone(1000, 200, SYSTEM_CHANNEL);
        }
#endif
#ifdef HAVE_BATTERY
        uint8_t getBatLevel() override;
        double getBatVoltage() override;
#endif
#ifdef HAVE_USB
        void _init_usb();
#endif
    public:
    };
} // namespace HAL

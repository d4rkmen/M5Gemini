/**
 * @file hal_sticks3.h
 * @brief HAL for M5StickS3 (ESP32-S3-PICO-1, M5PM1 PMIC, ES8311 codec, two buttons)
 */
#pragma once
#include "hal.h"
#include "common_define.h"
#include "pm1/pm1.h"

#if HAL_USE_SPEAKER
extern const uint8_t error_wav_start[] asm("_binary_error_wav_start");
extern const uint8_t error_wav_end[] asm("_binary_error_wav_end");
#endif

namespace HAL
{
    class HalStickS3 : public Hal
    {
    private:
        PM1* _pm1 = nullptr;

        void _init_power();
#if HAL_USE_DISPLAY
        void _init_display();
#endif
#if HAL_USE_MIC
        void _init_mic();
#endif

    public:
        HalStickS3(SETTINGS::Settings* settings) : Hal(settings) { _board_type = BoardType::STICK_S3; }

        std::string type() override { return "StickS3"; }
        void init() override;

        inline PM1* pm1() { return _pm1; }

#if HAL_USE_SPEAKER
        void playErrorSound() override { _speaker->playWav(error_wav_start, error_wav_end - error_wav_start); }
        void playKeyboardSound() override { _speaker->tone(5000, 20); }
        void playLastSound() override { _speaker->tone(6000, 20); }
        void playNextSound() override { _speaker->tone(7000, 20); }
        void playDeviceConnectedSound() override
        {
            _speaker->tone(1000, 100);
            delay(50);
            _speaker->tone(1500, 200);
        }
        void playDeviceDisconnectedSound() override
        {
            _speaker->tone(1500, 100);
            delay(50);
            _speaker->tone(1000, 200);
        }
#endif
#if HAL_USE_BAT
        uint8_t getBatLevel(float voltage) override;
        float getBatVoltage() override;
        bool isCharging() override;
#endif
        void setSpeakerAmp(bool enabled) override;
        void powerOff() override;
        void reboot() override;
    };
} // namespace HAL

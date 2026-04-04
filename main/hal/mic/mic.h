/**
 * @file mic.h
 * @brief Microphone class for ESP32S3 using ESP-IDF I2S driver
 * @details Standalone implementation based on M5Unified Mic_Class logic
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"

#define MIC_PIN_DATA_IN 46
#define MIC_PIN_WS 43
#define MIC_PIN_BCK -1
#define MIC_I2S_PORT I2S_NUM_0

namespace HAL
{
    struct mic_config_t
    {
        int pin_data_in = MIC_PIN_DATA_IN;
        int pin_bck = MIC_PIN_BCK;
        int pin_ws = MIC_PIN_WS;

        uint32_t sample_rate = 8000;

        bool stereo = false;
        bool left_channel = false;

        uint8_t over_sampling = 2;
        uint8_t magnification = 4;
        uint8_t noise_filter_level = 0;

        size_t dma_buf_len = 256;
        size_t dma_buf_count = 4;

        uint8_t task_priority = 15;
        uint8_t task_pinned_core = -1;

        i2s_port_t i2s_port = MIC_I2S_PORT;
    };

    class Mic
    {
    public:
        Mic();
        ~Mic();

        mic_config_t config(void) const { return _cfg; }
        void config(const mic_config_t& cfg) { _cfg = cfg; }

        bool begin(void);
        void end(void);

        bool isRunning(void) const { return _task_running; }
        bool isEnabled(void) const { return _cfg.pin_data_in >= 0; }

        /**
         * @brief Check recording state
         * @return 0=not recording / 1=recording (queue has room) / 2=recording (queue full)
         */
        size_t isRecording(void) const { return _is_recording ? ((bool)_rec_info[0].length) + ((bool)_rec_info[1].length) : 0; }

        void setSampleRate(uint32_t sample_rate) { _cfg.sample_rate = sample_rate; }

        bool record(int16_t* rec_data, size_t array_len, uint32_t sample_rate, bool stereo = false)
        {
            return _rec_raw(rec_data, array_len, true, sample_rate, stereo);
        }

        bool record(uint8_t* rec_data, size_t array_len, uint32_t sample_rate, bool stereo = false)
        {
            return _rec_raw(rec_data, array_len, false, sample_rate, stereo);
        }

        bool record(int16_t* rec_data, size_t array_len)
        {
            return _rec_raw(rec_data, array_len, true, _cfg.sample_rate, false);
        }

        bool record(uint8_t* rec_data, size_t array_len)
        {
            return _rec_raw(rec_data, array_len, false, _cfg.sample_rate, false);
        }

    private:
        struct recording_info_t
        {
            void* data = nullptr;
            size_t length = 0;
            size_t index = 0;
            bool is_stereo = false;
            bool is_16bit = false;
        };

        recording_info_t _rec_info[2];
        volatile bool _rec_flip = false;

        static void mic_task(void* args);

        uint32_t _calc_rec_rate(void) const;
        bool _setup_i2s(void);
        bool _rec_raw(void* recdata, size_t array_len, bool flg_16bit, uint32_t sample_rate, bool stereo);

        mic_config_t _cfg;
        uint32_t _rec_sample_rate = 0;

        int32_t _offset = 0;
        volatile bool _task_running = false;
        volatile bool _is_recording = false;
        TaskHandle_t _task_handle = nullptr;
        volatile SemaphoreHandle_t _task_semaphore = nullptr;

        i2s_chan_handle_t _rx_chan = nullptr;
    };

} // namespace HAL

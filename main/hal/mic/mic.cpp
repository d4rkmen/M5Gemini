/**
 * @file mic.cpp
 * @brief Microphone implementation for ESP32S3 using ESP-IDF I2S driver
 * @details Based on M5Unified Mic_Class logic, adapted to standalone HAL
 */
#include "mic.h"
#include "common_define.h"
#include <cstring>
#include <algorithm>
#include "esp_log.h"
#include "soc/i2s_struct.h"

static const char* TAG = "MIC";

namespace HAL
{
    Mic::Mic()
    {
    }

    Mic::~Mic()
    {
        end();
    }

    uint32_t Mic::_calc_rec_rate(void) const
    {
        return _cfg.sample_rate * _cfg.over_sampling;
    }

    bool Mic::_setup_i2s(void)
    {
        if (_cfg.pin_data_in < 0)
        {
            return false;
        }

        if (_rx_chan != nullptr)
        {
            i2s_channel_disable(_rx_chan);
            i2s_del_channel(_rx_chan);
            _rx_chan = nullptr;
        }

        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(_cfg.i2s_port, I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = _cfg.dma_buf_count;
        chan_cfg.dma_frame_num = _cfg.dma_buf_len;

        esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &_rx_chan);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to create I2S channel: %d", err);
            return false;
        }

        i2s_std_config_t i2s_config;
        memset(&i2s_config, 0, sizeof(i2s_std_config_t));
        i2s_config.clk_cfg.clk_src = i2s_clock_src_t::I2S_CLK_SRC_PLL_160M;
        i2s_config.clk_cfg.sample_rate_hz = 48000;
        i2s_config.clk_cfg.mclk_multiple = i2s_mclk_multiple_t::I2S_MCLK_MULTIPLE_128;
        i2s_config.slot_cfg.data_bit_width = i2s_data_bit_width_t::I2S_DATA_BIT_WIDTH_16BIT;
        i2s_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
        i2s_config.slot_cfg.slot_mode =
            _cfg.stereo ? i2s_slot_mode_t::I2S_SLOT_MODE_STEREO : i2s_slot_mode_t::I2S_SLOT_MODE_MONO;
        i2s_config.slot_cfg.slot_mask =
            _cfg.stereo ? i2s_std_slot_mask_t::I2S_STD_SLOT_BOTH
                        : (_cfg.left_channel ? i2s_std_slot_mask_t::I2S_STD_SLOT_LEFT
                                             : i2s_std_slot_mask_t::I2S_STD_SLOT_RIGHT);
        i2s_config.slot_cfg.ws_width = 16;
        i2s_config.slot_cfg.bit_shift = true;
        i2s_config.slot_cfg.left_align = true;
        i2s_config.slot_cfg.big_endian = false;
        i2s_config.slot_cfg.bit_order_lsb = false;
        i2s_config.gpio_cfg.bclk = (gpio_num_t)_cfg.pin_bck;
        i2s_config.gpio_cfg.ws = (gpio_num_t)_cfg.pin_ws;
        i2s_config.gpio_cfg.dout = (gpio_num_t)I2S_GPIO_UNUSED;
        i2s_config.gpio_cfg.mclk = (gpio_num_t)I2S_GPIO_UNUSED;
        i2s_config.gpio_cfg.din = (gpio_num_t)_cfg.pin_data_in;

        err = i2s_channel_init_std_mode(_rx_chan, &i2s_config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to init I2S std mode: %d", err);
            i2s_del_channel(_rx_chan);
            _rx_chan = nullptr;
            return false;
        }

        return true;
    }

    bool Mic::begin(void)
    {
        if (_task_running)
        {
            auto rate = _calc_rec_rate();
            if (_rec_sample_rate == rate)
            {
                return true;
            }
            do
            {
                vTaskDelay(1);
            } while (isRecording());
            end();
            _rec_sample_rate = rate;
        }

        if (_task_semaphore == nullptr)
        {
            _task_semaphore = xSemaphoreCreateBinary();
        }

        if (!_setup_i2s())
        {
            return false;
        }

        size_t stack_size = 2048 + (_cfg.dma_buf_len * sizeof(uint16_t));
        _task_running = true;

#if portNUM_PROCESSORS > 1
        if (_cfg.task_pinned_core < portNUM_PROCESSORS)
        {
            xTaskCreatePinnedToCore(mic_task, "mic_task", stack_size, this, _cfg.task_priority, &_task_handle,
                                    _cfg.task_pinned_core);
        }
        else
#endif
        {
            xTaskCreate(mic_task, "mic_task", stack_size, this, _cfg.task_priority, &_task_handle);
        }

        return true;
    }

    void Mic::end(void)
    {
        if (!_task_running)
        {
            return;
        }
        _task_running = false;
        if (_task_handle)
        {
            xTaskNotifyGive(_task_handle);
            do
            {
                vTaskDelay(1);
            } while (_task_handle);
        }

        if (_rx_chan)
        {
            i2s_channel_disable(_rx_chan);
            i2s_del_channel(_rx_chan);
            _rx_chan = nullptr;
        }
    }

    void Mic::mic_task(void* args)
    {
        auto self = (Mic*)args;
        int oversampling = self->_cfg.over_sampling;
        if (oversampling < 1)
        {
            oversampling = 1;
        }
        else if (oversampling > 8)
        {
            oversampling = 8;
        }

        // Configure I2S clock dividers for the desired sample rate
        // For ESP32-S3 with I2S standard mode, set the clock registers directly
        static constexpr uint32_t PLL_D2_CLK = 120 * 1000 * 1000; // 240 MHz / 2 for ESP32-S3
        uint32_t bits = 16;
        uint32_t div_m = 8;

        // Calculate clock dividers
        uint32_t target_freq = self->_cfg.sample_rate * oversampling;
        uint32_t base_clock = PLL_D2_CLK / (bits * div_m);

        uint32_t div_n = base_clock / target_freq;
        uint32_t div_a = 128;
        uint32_t div_b = (uint32_t)((uint64_t)(base_clock % target_freq) * div_a / target_freq);

        auto dev = &I2S0;
        if (self->_cfg.i2s_port == I2S_NUM_1)
        {
#if defined(I2S1I_BCK_OUT_IDX)
            dev = &I2S1;
#endif
        }

        dev->rx_conf.rx_pdm_en = 0;
        dev->rx_conf.rx_tdm_en = 1;
        dev->rx_conf.rx_update = 1;
        dev->rx_conf1.rx_bck_div_num = div_m - 1;

        bool yn1 = (div_b > (div_a >> 1));
        if (yn1)
        {
            div_b = div_a - div_b;
        }
        int div_y = 1;
        int div_x = 0;
        if (div_b)
        {
            div_x = div_a / div_b - 1;
            div_y = div_a % div_b;
            if (div_y == 0)
            {
                div_y = 1;
                div_b = 511;
            }
        }

        dev->rx_clkm_div_conf.rx_clkm_div_x = div_x;
        dev->rx_clkm_div_conf.rx_clkm_div_y = div_y;
        dev->rx_clkm_div_conf.rx_clkm_div_z = div_b;
        dev->rx_clkm_div_conf.rx_clkm_div_yn1 = yn1;
        dev->rx_clkm_conf.rx_clkm_div_num = div_n;
        dev->rx_clkm_conf.rx_clk_sel = 1; // PLL_240M_CLK
        dev->tx_clkm_conf.clk_en = 1;
        dev->rx_clkm_conf.rx_clk_active = 1;
        dev->rx_conf.rx_update = 1;
        dev->rx_conf.rx_update = 0;

        i2s_channel_enable(self->_rx_chan);

        int32_t gain = self->_cfg.magnification;
        const float f_gain = (float)gain / (oversampling << 1);
        size_t src_idx = ~0u;
        size_t src_len = 0;
        int32_t sum_value[4] = {0, 0};
        int32_t prev_value[2] = {0, 0};
        const bool in_stereo = self->_cfg.stereo;
        int32_t os_remain = oversampling;
        const size_t dma_buf_len = self->_cfg.dma_buf_len;
        int16_t* src_buf = (int16_t*)alloca(dma_buf_len * sizeof(int16_t));
        memset(src_buf, 0, dma_buf_len * sizeof(int16_t));

        // Flush initial I2S data
        i2s_channel_read(self->_rx_chan, src_buf, dma_buf_len, &src_len, portTICK_PERIOD_MS);
        i2s_channel_read(self->_rx_chan, src_buf, dma_buf_len, &src_len, portTICK_PERIOD_MS);

        while (self->_task_running)
        {
            bool rec_flip = self->_rec_flip;
            recording_info_t* current_rec = &(self->_rec_info[!rec_flip]);
            recording_info_t* next_rec = &(self->_rec_info[rec_flip]);

            size_t dst_remain = current_rec->length;
            if (dst_remain == 0)
            {
                rec_flip = !rec_flip;
                self->_rec_flip = rec_flip;
                xSemaphoreGive(self->_task_semaphore);
                std::swap(current_rec, next_rec);
                dst_remain = current_rec->length;
                if (dst_remain == 0)
                {
                    self->_is_recording = false;
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                    src_idx = ~0u;
                    src_len = 0;
                    sum_value[0] = 0;
                    sum_value[1] = 0;
                    continue;
                }
            }
            self->_is_recording = true;

            for (;;)
            {
                if (src_idx >= src_len)
                {
                    i2s_channel_read(self->_rx_chan, src_buf, dma_buf_len, &src_len, 100 / portTICK_PERIOD_MS);
                    src_len >>= 1;
                    src_idx = 0;
                }

                do
                {
                    sum_value[0] += src_buf[src_idx];
                    sum_value[1] += src_buf[src_idx + 1];
                    src_idx += 2;
                } while (--os_remain && (src_idx < src_len));

                if (os_remain)
                {
                    continue;
                }
                os_remain = oversampling;

                auto sv0 = sum_value[0];
                auto sv1 = sum_value[1];

                auto value_tmp = (sv0 + sv1) << 3;
                int32_t offset = self->_offset;
                offset -= (value_tmp + offset + 16) >> 5;
                self->_offset = offset;
                offset = (offset + 8) >> 4;
                sum_value[0] = sv0 + offset;
                sum_value[1] = sv1 + offset;

                int32_t noise_filter = self->_cfg.noise_filter_level;
                if (noise_filter)
                {
                    for (int i = 0; i < 2; ++i)
                    {
                        int32_t v = (sum_value[i] * (256 - noise_filter) + prev_value[i] * noise_filter + 128) >> 8;
                        prev_value[i] = v;
                        sum_value[i] = v * f_gain;
                    }
                }
                else
                {
                    for (int i = 0; i < 2; ++i)
                    {
                        sum_value[i] *= f_gain;
                    }
                }

                int output_num = current_rec->is_stereo ? 2 : 1;

                if (in_stereo != current_rec->is_stereo)
                {
                    if (in_stereo)
                    {
                        sum_value[0] = (sum_value[0] + sum_value[1] + 1) >> 1;
                        output_num = 1;
                    }
                    else
                    {
                        auto tmp = sum_value[1];
                        sum_value[3] = tmp;
                        sum_value[2] = tmp;
                        sum_value[1] = sum_value[0];
                        output_num = 4;
                    }
                }
                for (int i = 0; i < output_num; ++i)
                {
                    auto value = sum_value[i];
                    if (current_rec->is_16bit)
                    {
                        if (value < INT16_MIN + 16)
                        {
                            value = INT16_MIN + 16;
                        }
                        else if (value > INT16_MAX - 16)
                        {
                            value = INT16_MAX - 16;
                        }
                        auto dst = (int16_t*)(current_rec->data);
                        *dst++ = value;
                        current_rec->data = dst;
                    }
                    else
                    {
                        value = ((value + 128) >> 8) + 128;
                        if (value < 0)
                        {
                            value = 0;
                        }
                        else if (value > 255)
                        {
                            value = 255;
                        }
                        auto dst = (uint8_t*)(current_rec->data);
                        *dst++ = value;
                        current_rec->data = dst;
                    }
                }
                sum_value[0] = 0;
                sum_value[1] = 0;
                dst_remain -= output_num;
                if ((int32_t)dst_remain <= 0)
                {
                    current_rec->length = 0;
                    break;
                }
            }
        }
        self->_is_recording = false;
        i2s_channel_disable(self->_rx_chan);

        self->_task_handle = nullptr;
        vTaskDelete(nullptr);
    }

    bool Mic::_rec_raw(void* recdata, size_t array_len, bool flg_16bit, uint32_t sample_rate, bool stereo)
    {
        recording_info_t info;
        info.data = recdata;
        info.length = array_len;
        info.is_16bit = flg_16bit;
        info.is_stereo = stereo;

        _cfg.sample_rate = sample_rate;

        if (!begin())
        {
            return false;
        }
        if (array_len == 0)
        {
            return true;
        }
        while (_rec_info[_rec_flip].length)
        {
            xSemaphoreTake(_task_semaphore, 1);
        }
        _rec_info[_rec_flip] = info;
        if (_task_handle)
        {
            xTaskNotifyGive(_task_handle);
        }
        return true;
    }

} // namespace HAL

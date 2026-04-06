/**
 * @file mic.cpp
 * @brief Microphone implementation for ESP32S3 using ESP-IDF I2S driver
 * @details Based on M5Unified Mic_Class logic, adapted to standalone HAL
 */
#include "mic.h"
#include "hal.h"
#include "common_define.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include "esp_log.h"
#include "soc/i2s_struct.h"
#include "driver/i2s_pdm.h"

static const char* TAG = "MIC";

static void calcClockDiv(uint32_t* div_a, uint32_t* div_b, uint32_t* div_n, uint32_t baseClock, uint32_t targetFreq)
{
    if (baseClock <= targetFreq << 1)
    {
        *div_n = 2;
        *div_a = 1;
        *div_b = 0;
        return;
    }
    uint32_t save_n = 255;
    uint32_t save_a = 63;
    uint32_t save_b = 62;
    if (targetFreq)
    {
        float fdiv = (float)baseClock / targetFreq;
        uint32_t n = (uint32_t)fdiv;
        if (n < 256)
        {
            fdiv -= n;

            float check_base = baseClock;
            while ((int32_t)targetFreq >= 0)
            {
                targetFreq <<= 1;
                check_base *= 2;
            }
            float check_target = targetFreq;

            uint32_t save_diff = UINT32_MAX;
            if (n < 255)
            {
                save_a = 1;
                save_b = 0;
                save_n = n + 1;
                save_diff = abs((int)(check_target - check_base / (float)save_n));
            }

            for (uint32_t a = 1; a < 64; ++a)
            {
                uint32_t b = roundf(a * fdiv);
                if (a <= b)
                {
                    continue;
                }
                uint32_t diff = abs((int)(check_target - ((check_base * a) / (n * a + b))));
                if (save_diff <= diff)
                {
                    continue;
                }
                save_diff = diff;
                save_a = a;
                save_b = b;
                save_n = n;
                if (!diff)
                {
                    break;
                }
            }
        }
    }
    *div_n = save_n;
    *div_a = save_a;
    *div_b = save_b;
}

namespace HAL
{
    Mic::Mic(Hal* hal) : _hal(hal), _board_type(hal->board_type()) {}

    Mic::~Mic() { end(); }

    uint32_t Mic::_calc_rec_rate(void) const { return _cfg.sample_rate * _cfg.over_sampling; }

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

        // PDM mic (no BCK pin) vs standard I2S mic
#if SOC_I2S_SUPPORTS_PDM_RX
        if (_cfg.pin_bck < 0)
        {
            i2s_pdm_rx_config_t pdm_config;
            memset(&pdm_config, 0, sizeof(i2s_pdm_rx_config_t));
            pdm_config.clk_cfg.clk_src = i2s_clock_src_t::I2S_CLK_SRC_PLL_160M;
            pdm_config.clk_cfg.sample_rate_hz = 48000;
            pdm_config.clk_cfg.mclk_multiple = i2s_mclk_multiple_t::I2S_MCLK_MULTIPLE_128;
            pdm_config.slot_cfg.data_bit_width = i2s_data_bit_width_t::I2S_DATA_BIT_WIDTH_16BIT;
            pdm_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
            pdm_config.slot_cfg.slot_mode =
                _cfg.stereo ? i2s_slot_mode_t::I2S_SLOT_MODE_STEREO : i2s_slot_mode_t::I2S_SLOT_MODE_MONO;
            pdm_config.slot_cfg.slot_mask = _cfg.stereo ? i2s_pdm_slot_mask_t::I2S_PDM_SLOT_BOTH
                                                        : (_cfg.left_channel ? i2s_pdm_slot_mask_t::I2S_PDM_SLOT_LEFT
                                                                             : i2s_pdm_slot_mask_t::I2S_PDM_SLOT_RIGHT);
            pdm_config.gpio_cfg.clk = (gpio_num_t)_cfg.pin_ws;
            pdm_config.gpio_cfg.din = (gpio_num_t)_cfg.pin_data_in;

            err = i2s_channel_init_pdm_rx_mode(_rx_chan, &pdm_config);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to init I2S PDM RX mode: %s", esp_err_to_name(err));
                i2s_del_channel(_rx_chan);
                _rx_chan = nullptr;
                return false;
            }
            ESP_LOGI(TAG, "I2S PDM RX mode initialized (din=%d, clk=%d)", _cfg.pin_data_in, _cfg.pin_ws);
        }
        else
#endif
        {
            i2s_std_config_t i2s_config;
            memset(&i2s_config, 0, sizeof(i2s_std_config_t));
            i2s_config.clk_cfg.clk_src = i2s_clock_src_t::I2S_CLK_SRC_PLL_160M;
            i2s_config.clk_cfg.sample_rate_hz = 48000;
            i2s_config.clk_cfg.mclk_multiple = i2s_mclk_multiple_t::I2S_MCLK_MULTIPLE_128;
            i2s_config.slot_cfg.data_bit_width = i2s_data_bit_width_t::I2S_DATA_BIT_WIDTH_16BIT;
            i2s_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
            i2s_config.slot_cfg.slot_mode =
                _cfg.stereo ? i2s_slot_mode_t::I2S_SLOT_MODE_STEREO : i2s_slot_mode_t::I2S_SLOT_MODE_MONO;
            i2s_config.slot_cfg.slot_mask = _cfg.stereo ? i2s_std_slot_mask_t::I2S_STD_SLOT_BOTH
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
                ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(err));
                i2s_del_channel(_rx_chan);
                _rx_chan = nullptr;
                return false;
            }
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

        if (_board_type == BoardType::CARDPUTER_ADV && _hal->es8311())
        {
            _hal->es8311()->mic_enable();
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
            xTaskCreatePinnedToCore(mic_task,
                                    "mic_task",
                                    stack_size,
                                    this,
                                    _cfg.task_priority,
                                    &_task_handle,
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

        if (_board_type == BoardType::CARDPUTER_ADV && _hal->es8311())
        {
            _hal->es8311()->mic_disable();
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

        bool use_pdm = (self->_cfg.pin_bck < 0);

        static constexpr uint32_t PLL_D2_CLK = 120 * 1000 * 1000; // 240 MHz / 2 for ESP32-S3
        uint32_t bits = 16;
        uint32_t div_a, div_b, div_n;
        uint32_t div_m = 8;

        if (use_pdm)
        {
            bits = 64;
            div_m = 2;
        }
        calcClockDiv(&div_a, &div_b, &div_n, PLL_D2_CLK / (bits * div_m), self->_cfg.sample_rate * oversampling);

        auto dev = &I2S0;
        if (self->_cfg.i2s_port == I2S_NUM_1)
        {
#if defined(I2S1I_BCK_OUT_IDX)
            dev = &I2S1;
#endif
        }

        // Set clock dividers and mode BEFORE enabling channel (matches M5Unified).
        // This ensures the ES8311 codec receives the correct BCLK from the start,
        // avoiding a frequency glitch that would trigger a long PLL re-lock period.
        dev->rx_conf.rx_pdm_en = use_pdm;
        dev->rx_conf.rx_tdm_en = !use_pdm;
#if defined(I2S_RX_PDM2PCM_CONF_REG)
        dev->rx_pdm2pcm_conf.rx_pdm2pcm_en = use_pdm;
        dev->rx_pdm2pcm_conf.rx_pdm_sinc_dsr_16_en = 1;
#elif defined(I2S_RX_PDM2PCM_EN)
        dev->rx_conf.rx_pdm2pcm_en = use_pdm;
        dev->rx_conf.rx_pdm_sinc_dsr_16_en = 1;
#endif
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
                    // Drain I2S DMA while idle — keeps codec startup zeros from
                    // accumulating and ensures fresh data when recording starts.
                    while (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20)))
                    {
                        if (!self->_task_running)
                            break;
                        i2s_channel_read(self->_rx_chan, src_buf, dma_buf_len, &src_len, 0);
                    }
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

                int output_num = 2;

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

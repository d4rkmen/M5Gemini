#pragma once

#define LGFX_USE_V1

#include <LovyanGFX.hpp>
#include "Panel_ST7789V2.hpp"
#include "Bus_IDFSPI.hpp"

// M5StickS3: ST7789P3 135x240, pins and offsets as in M5GFX board_M5StickS3.
// LCD power (M5PM1 GPIO2) must be switched on before this is constructed.
class LGFX_StickS3 : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789V2 _panel_instance;
    lgfx::Bus_IDFSPI _bus_instance;
    lgfx::Light_PWM _light_instance;

public:
    LGFX_StickS3(void)
    {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI3_HOST;
            cfg.freq_write = 40 * 1000 * 1000;
            cfg.freq_read = 4 * 1000 * 1000;
            cfg.pin_dc = 45;
            cfg.pin_cs = 41;
            cfg.pin_sclk = 40;
            cfg.pin_io0 = 39;
            cfg.spi_3wire = true;
            cfg.pin_io1 = -1;
            cfg.pin_io2 = -1;
            cfg.pin_io3 = -1;
            cfg.spi_mode = 0;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();

            cfg.pin_rst = 21;
            cfg.panel_height = 240;
            cfg.panel_width = 135;
            cfg.offset_x = 52;
            cfg.offset_y = 40;
            cfg.bus_shared = false;

            _panel_instance.config(cfg);
        }
        {
            auto cfg = _light_instance.config();

            cfg.pin_bl = 38;
            cfg.invert = false;
            cfg.freq = 256;
            cfg.offset = 16;
            cfg.pwm_channel = 7;

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        setPanel(&_panel_instance);
        // initialize the display
        init();
        setBrightness(0);
        setRotation(1);
    }
};

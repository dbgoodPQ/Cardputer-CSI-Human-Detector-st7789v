#ifndef EXT_PANEL_H
#define EXT_PANEL_H
//
// External Waveshare 2inch LCD module (ST7789V, 240x320, IPS) driver for the Cardputer ADV.
// Replaces the 2.8" ILI9341 panel of the original project.
//
// Wiring (EXT header): CS=GPIO5 RST=GPIO3 DC=GPIO6 MOSI=GPIO14 SCK=GPIO40 BL=GPIO4
// Bus: SPI2_HOST (FSPI), shared with SD (bus_shared=true).
// Backlight: PWM on GPIO4, driven by setBrightness() (EXT PANEL setting in the menu).
//
#include <M5Unified.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>
#include <lgfx/v1/platforms/esp32/Light_PWM.hpp>

#ifndef EXT_TFT_CS
#define EXT_TFT_CS   5
#endif
#ifndef EXT_TFT_DC
#define EXT_TFT_DC   6
#endif
#ifndef EXT_TFT_RST
#define EXT_TFT_RST  3
#endif
#ifndef EXT_TFT_BL
#define EXT_TFT_BL   4
#endif
#ifndef EXT_TFT_MOSI
#define EXT_TFT_MOSI 14
#endif
#ifndef EXT_TFT_SCK
#define EXT_TFT_SCK  40
#endif
// Landscape (320x240). 1 matches the Cardputer's built-in screen; use 3 to turn the picture 180°.
#ifndef EXT_ROTATION
#define EXT_ROTATION 1
#endif

class LGFX_ExtST7789 : public lgfx::v1::LGFX_Device {
    lgfx::v1::Panel_ST7789 _panel_instance;
    lgfx::v1::Bus_SPI      _bus_instance;
    lgfx::v1::Light_PWM    _light_instance;
public:
    LGFX_ExtST7789() {
        {   auto cfg = _bus_instance.config();
            cfg.spi_host    = SPI2_HOST;     // FSPI — SAME host as the SD card
            cfg.spi_mode    = 0;
            cfg.freq_write  = 27000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = true;
            cfg.use_lock    = true;
            cfg.dma_channel = 0;
            cfg.pin_sclk    = EXT_TFT_SCK;
            cfg.pin_mosi    = EXT_TFT_MOSI;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = EXT_TFT_DC;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {   auto cfg = _panel_instance.config();
            cfg.pin_cs           = EXT_TFT_CS;
            cfg.pin_rst          = EXT_TFT_RST;
            cfg.pin_busy         = -1;
            cfg.memory_width     = 240;
            cfg.memory_height    = 320;
            cfg.panel_width      = 240;
            cfg.panel_height     = 320;
            cfg.offset_x         = 0;        // Waveshare 2inch: full 240x320 GRAM, no offsets
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = true;     // IPS ST7789V needs colour inversion
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;     // shared with SD card
            _panel_instance.config(cfg);
        }
        {   auto cfg = _light_instance.config();
            cfg.pin_bl      = EXT_TFT_BL;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 3;             // channel 7 is used by the built-in screen's backlight
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }
        setPanel(&_panel_instance);
    }
};

#endif // EXT_PANEL_H

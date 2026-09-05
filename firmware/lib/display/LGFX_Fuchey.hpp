#pragma once
// ============================================================
// Fuchey — LGFX_Fuchey.hpp
// LovyanGFX device for the 1.54" 240x240 ST7789V2 IPS panel
// over 4-wire SPI. All pin assignments come from
// Fuchey::DisplayConfig in ../config/Config.hpp.
// ============================================================

#include <LovyanGFX.hpp>
#include "../config/Config.hpp"

class LGFX_Fuchey : public lgfx::LGFX_Device {
    lgfx::Bus_SPI      _bus;
    lgfx::Panel_ST7789 _panel;

public:
    LGFX_Fuchey() {
        // ── SPI bus ───────────────────────────────────────
        auto cfg = _bus.config();
        cfg.spi_host    = Fuchey::DisplayConfig::SPI_HOST;
        cfg.spi_mode    = 0;                                       // CPOL=0, CPHA=0
        cfg.freq_write  = Fuchey::DisplayConfig::SPI_FREQ_HZ;     // 40 MHz
        cfg.freq_read   = 16000000;                               // SPI reads are rare
        cfg.pin_sclk    = Fuchey::DisplayConfig::PIN_SCLK;
        cfg.pin_mosi    = Fuchey::DisplayConfig::PIN_MOSI;
        cfg.pin_miso    = -1;                                     // readback not used
        cfg.pin_dc      = Fuchey::DisplayConfig::PIN_DC;
        _bus.config(cfg);

        // ── ST7789 panel ──────────────────────────────────
        auto p = _panel.config();
        p.pin_cs        = Fuchey::DisplayConfig::PIN_CS;
        p.pin_rst       = Fuchey::DisplayConfig::PIN_RST;
        p.panel_width   = Fuchey::DisplayConfig::WIDTH;           // 240
        p.panel_height  = Fuchey::DisplayConfig::HEIGHT;          // 240
        p.offset_x      = 0;
        p.offset_y      = 0;
        p.invert        = false;
        p.rgb_order     = false;
        // TODO(config): this is a generic "ST7789V2 IPS 1.54" 240x240"
        // module. If the image ships shifted/ghosted/inverted on the
        // actual hardware, tune offset_x/offset_y, invert and rgb_order
        // above (common alternates: invert=true, rgb_order=true).
        _panel.config(p);

        setPanel(&_panel);
    }
};
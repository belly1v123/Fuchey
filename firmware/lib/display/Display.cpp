// ============================================================
// Fuchey — Display.cpp
// ST7789 240x240 TFT driver over SPI, implemented on LovyanGFX.
// ============================================================

#include "Display.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>

namespace Fuchey {

// ─── Font mapping ──────────────────────────────────────────
// Font0 ~6x8, Font2 ~8x16, Font4 ~16x32 (built-in LovyanGFX bitmaps).
// Takes the common LovyanGFX base so both the panel and sprites can use it.
void Display::apply_font(lgfx::LovyanGFX& gfx, FontSize size) {
    switch (size) {
        case FontSize::SMALL:  gfx.setFont(&lgfx::fonts::Font0); break;
        case FontSize::MEDIUM: gfx.setFont(&lgfx::fonts::Font2); break;
        case FontSize::LARGE:  gfx.setFont(&lgfx::fonts::Font4); break;
    }
    gfx.setTextSize(1);
}

// ─── Constructor ───────────────────────────────────────────
Display::Display() = default;

// ─── init ──────────────────────────────────────────────────
bool Display::init() {
    if (!m_lgfx.init()) {
        ESP_LOGE(TAG, "ST7789 init failed");
        return false;
    }

    m_ready = true;
    clear();
    draw_text_centered(100, "FUCHEY", Display::FontSize::LARGE);
    draw_text_centered(190, "Initializing...", Display::FontSize::SMALL);
    ESP_LOGI(TAG, "ST7789 240x240 ready on SPI2 (SCLK=%d MOSI=%d CS=%d DC=%d RST=%d)",
             Fuchey::DisplayConfig::PIN_SCLK, Fuchey::DisplayConfig::PIN_MOSI,
             Fuchey::DisplayConfig::PIN_CS,   Fuchey::DisplayConfig::PIN_DC,
             Fuchey::DisplayConfig::PIN_RST);
    return true;
}

// ─── Power ─────────────────────────────────────────────────
void Display::power_on()  { m_lgfx.powerSaveOff(); }
void Display::power_off() { m_lgfx.powerSaveOn(); }

// ─── Drawing ───────────────────────────────────────────────
void Display::clear(Color c) { m_lgfx.fillScreen(c); }

void Display::draw_hline(int x, int y, int len, Color c) {
    m_lgfx.drawFastHLine(x, y, len, c);
}

void Display::draw_rect(int x, int y, int w, int h, Color c) {
    m_lgfx.drawRect(x, y, w, h, c);
}

void Display::fill_rect(int x, int y, int w, int h, Color c) {
    m_lgfx.fillRect(x, y, w, h, c);
}

// ─── Text ──────────────────────────────────────────────────
void Display::draw_text(int x, int y, std::string_view text, FontSize size, Color c) {
    apply_font(m_lgfx, size);
    m_lgfx.setTextColor(c);
    std::string s(text);
    m_lgfx.drawString(s.c_str(), x, y);
}

void Display::draw_text_centered(int y, std::string_view text, FontSize size, Color c) {
    apply_font(m_lgfx, size);
    m_lgfx.setTextColor(c);
    std::string s(text);
    int w = m_lgfx.textWidth(s.c_str());
    int x = (WIDTH - w) / 2;
    if (x < 0) x = 0;
    m_lgfx.drawString(s.c_str(), x, y);
}

// ─── Bitmap (1-bpp XBM, MSB-first, byte-padded rows) ───────
void Display::draw_bitmap(int x, int y, int w, int h, const uint8_t* mask, Color fg) {
    if (!mask || w <= 0 || h <= 0) return;
    int bytes_per_row = (w + 7) / 8;
    m_lgfx.startWrite();
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            if (mask[row * bytes_per_row + col / 8] & (0x80u >> (col % 8))) {
                m_lgfx.writePixel(x + col, y + row, fg);
            }
        }
    }
    m_lgfx.endWrite();
}

void Display::draw_progress_bar(int x, int y, int w, int h, uint8_t percent, Color c) {
    if (percent > 100) percent = 100;
    m_lgfx.drawRect(x, y, w, h, c);
    int fill = (w - 2) * percent / 100;
    if (fill > 0) m_lgfx.fillRect(x + 1, y + 1, fill, h - 2, c);
}

// ─── Animated boot splash ──────────────────────────────────
// Each frame is built on an LGFX_Sprite and pushed once;
// the live panel is never cleared/redrawn mid-frame.
void Display::animate_boot(uint32_t duration_ms) {
    constexpr int BAR_X = 20;
    constexpr int BAR_Y = 196;
    constexpr int BAR_W = WIDTH - 40;
    constexpr int BAR_H = 16;
    constexpr int FRAMES = 100;

    LGFX_Sprite spr(&m_lgfx);
    spr.setColorDepth(16);  // RGB565
    if (!spr.createSprite(WIDTH, HEIGHT)) {
        ESP_LOGE(TAG, "animate_boot: sprite alloc failed (%dx%d)",
                 WIDTH, HEIGHT);
        return;
    }

    for (int frame = 0; frame <= FRAMES; ++frame) {
        spr.clear(TFT_BLACK);

        apply_font(spr, FontSize::LARGE);
        spr.setTextColor(TFT_WHITE);
        spr.drawString("FUCHEY", (WIDTH - spr.textWidth("FUCHEY")) / 2, 60);

        apply_font(spr, FontSize::SMALL);
        spr.setTextColor(TFT_WHITE);
        spr.drawString("Initializing", (WIDTH - spr.textWidth("Initializing")) / 2, 150);

        uint8_t pct = static_cast<uint8_t>(frame * 100 / FRAMES);
        spr.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, TFT_WHITE);
        int fill = (BAR_W - 2) * pct / 100;
        if (fill > 0) spr.fillRect(BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, TFT_GREEN);

        spr.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(duration_ms / FRAMES));
    }

    spr.deleteSprite();
}

// ─── flush ─────────────────────────────────────────────────
bool Display::flush() { return true; }

} // namespace Fuchey
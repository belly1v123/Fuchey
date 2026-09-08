// ============================================================
// Fuchey — Display.cpp
// ST7789 240x240 TFT over SPI (direct driver, no library).
// ============================================================

#include "Display.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>

namespace Fuchey {

// ─── Constructor ───────────────────────────────────────────
Display::Display() = default;

// ─── Lifecycle ─────────────────────────────────────────────
bool Display::init() {
    if (!m_lcd.init()) {
        ESP_LOGE(TAG, "ST7789 init failed");
        return false;
    }
    m_ready = true;
    ESP_LOGI(TAG, "ST7789 240x240 ready on SPI2 (SCLK=%d MOSI=%d CS=%d DC=%d RST=%d)",
             DisplayConfig::PIN_SCLK, DisplayConfig::PIN_MOSI,
             DisplayConfig::PIN_CS,   DisplayConfig::PIN_DC,
             DisplayConfig::PIN_RST);
    return true;
}

void Display::power_on()  { m_lcd.power_on(); }
void Display::power_off() { m_lcd.power_off(); }

// ─── Drawing ───────────────────────────────────────────────
void Display::clear(Color c) { m_lcd.fill_screen(c); }

void Display::draw_hline(int x, int y, int len, Color c) {
    m_lcd.draw_hline(x, y, len, c);
}

void Display::draw_rect(int x, int y, int w, int h, Color c) {
    m_lcd.draw_rect(x, y, w, h, c);
}

void Display::fill_rect(int x, int y, int w, int h, Color c) {
    m_lcd.fill_rect(x, y, w, h, c);
}

// ─── Text ──────────────────────────────────────────────────
void Display::draw_text(int x, int y, std::string_view text, FontSize size, Color c) {
    m_lcd.draw_text(x, y, text, static_cast<St7789::FontSize>(static_cast<int>(size)), c);
}

void Display::draw_text_centered(int y, std::string_view text, FontSize size, Color c) {
    auto fs = static_cast<St7789::FontSize>(static_cast<int>(size));
    int w = m_lcd.text_width(text, fs);
    int x = (WIDTH - w) / 2;
    if (x < 0) x = 0;
    m_lcd.draw_text(x, y, text, fs, c);
}

// ─── Bitmap (1-bpp XBM, MSB-first, byte-padded rows) ───────
void Display::draw_bitmap(int x, int y, int w, int h, const uint8_t* mask, Color fg) {
    m_lcd.draw_bitmap(x, y, w, h, mask, fg);
}

void Display::draw_sprite(int x, int y, int w, int h, const Color* data) {
    m_lcd.draw_rgb565_image(x, y, w, h, data);
}

void Display::draw_sprite_transparent(int x, int y, int w, int h, const Color* data,
                                      Color transparent) {
    m_lcd.draw_rgb565_image_transparent(x, y, w, h, data, transparent);
}

void Display::draw_progress_bar(int x, int y, int w, int h, uint8_t percent, Color c) {
    m_lcd.draw_progress_bar(x, y, w, h, percent, c);
}

// ─── Animated boot splash ──────────────────────────────────
// Framebuffer-backed: static parts are drawn once, then only the
// growing bar segment is updated in RAM. A full-frame push costs
// ~115 ms at 8 MHz, so flush every 10th frame (plus a final one)
// to keep the ~3 s boot time instead of ~15 s.
void Display::animate_boot(uint32_t duration_ms) {
    constexpr int BAR_X  = 20;
    constexpr int BAR_Y  = 196;
    constexpr int BAR_W  = WIDTH - 40;
    constexpr int BAR_H  = 16;
    constexpr int FRAMES = 100;

    clear(TFT_BLACK);
    draw_text_centered(60, "FUCHEY", FontSize::LARGE);
    draw_text_centered(150, "Initializing", FontSize::SMALL);
    m_lcd.draw_rect(BAR_X, BAR_Y, BAR_W, BAR_H, TFT_WHITE);
    flush();

    int prev_fill = 0;
    for (int frame = 0; frame <= FRAMES; ++frame) {
        int pct = frame * 100 / FRAMES;
        int fill = (BAR_W - 2) * pct / 100;
        if (fill > prev_fill) {
            m_lcd.fill_rect(BAR_X + 1 + prev_fill, BAR_Y + 1,
                            fill - prev_fill, BAR_H - 2, TFT_GREEN);
            prev_fill = fill;
        }
        if (frame % 10 == 0 || frame == FRAMES) flush();
        vTaskDelay(pdMS_TO_TICKS(duration_ms / FRAMES));
    }
    flush();
}

// ─── flush ─────────────────────────────────────────────────
// Pushes the RAM framebuffer to the panel in one SPI/DMA transfer.
// Everything above (clear/fill_rect/draw_text/...) only touches RAM.
bool Display::flush() { m_lcd.push_frame(); return true; }

bool Display::flush_window(int x, int y, int w, int h) { m_lcd.push_window(x, y, w, h); return true; }

} // namespace Fuchey
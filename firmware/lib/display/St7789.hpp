#pragma once
// ============================================================
// Fuchey — St7789.hpp
// Direct ST7789 240x240 SPI driver (no graphics library).
// Uses the ESP-IDF SPI master driver with a CS GPIO owned by
// spi_bus_add_device; DC and RST are driven as plain GPIOs.
// This is the same code path proven by the boot-time raw probe
// (RED/GREEN/WHITE fills), so it bypasses LovyanGFX entirely.
// ============================================================

#include <cstdint>
#include <string_view>

namespace Fuchey {

// ── Adafruit-GFX-compatible bitmap font (e.g. FreeSansBold9pt7b) ──
// Layout matches Adafruit_GFX.h exactly so stock font headers work.
struct GFXglyph {
    uint16_t bitmapOffset;
    uint8_t  width, height;
    uint8_t  xAdvance;
    int8_t   xOffset, yOffset;
};
struct GFXfont {
    uint8_t*  bitmap;
    GFXglyph* glyph;
    uint8_t   first, last, yAdvance;
};

class St7789 {
public:
    St7789() = default;
    St7789(const St7789&) = delete;
    St7789& operator=(const St7789&) = delete;

    bool init();
    bool is_ready() const { return m_ready; }

    // Pushes the RAM framebuffer to the panel in one SPI/DMA transfer.
    // All draw calls below only touch RAM until this is called.
    void push_frame();

    void power_on();
    void power_off();

    // ── Shapes (RGB565 colors) ────────────────────────────
    void fill_screen(uint16_t c);
    void fill_rect(int x, int y, int w, int h, uint16_t c);
    void draw_rect(int x, int y, int w, int h, uint16_t c);
    void draw_hline(int x, int y, int len, uint16_t c);
    void draw_vline(int x, int y, int len, uint16_t c);

    // ── Text (built-in 5x7 ASCII font, integer-scaled) ────
    enum class FontSize { SMALL = 1, MEDIUM = 2, LARGE = 3 };
    void draw_text(int x, int y, std::string_view text, FontSize size, uint16_t color);
    // Same with an arbitrary integer scale (for hero text bigger than LARGE).
    void draw_text_scaled(int x, int y, std::string_view text, int scale, uint16_t color);
    int  text_width(std::string_view text, FontSize size) const;

    // ── GFX bitmap-font text (transparent bg; caller prepares background) ─
    // y is the TOP of the string (top-aligned via min-yOffset scan).
    void draw_gfx_text(int x, int y, std::string_view text,
                       const GFXfont* font, int size, uint16_t color);
    // Tight pixel bounds of the string at the given size.
    void gfx_text_bounds(std::string_view text, const GFXfont* font, int size,
                         int* w_out, int* h_out) const;

    // ── Bitmap (1-bpp, row-major, MSB-first, byte-padded) ─
    void draw_bitmap(int x, int y, int w, int h, const uint8_t* mask, uint16_t fg);

    // ── RGB565 sprite blit (framebuffer-only, call push_* after) ─
    // data: w*h CPU-order RGB565 pixels (see scripts/convert_sprite.py).
    void draw_rgb565_image(int x, int y, int w, int h, const uint16_t* data);
    // Same, but skips pixels matching transparent (color-key).
    void draw_rgb565_image_transparent(int x, int y, int w, int h,
                                       const uint16_t* data, uint16_t transparent);
    // Blits a w*h crop taken at (sx,sy) out of a srcW-wide source image.
    // Used to restore background regions under animated overlays.
    void draw_rgb565_subimage(int x, int y, int srcW, int sx, int sy,
                              int w, int h, const uint16_t* data);
    // Same crop blit, but box-blurred (radius px) and dimmed (keep/256
    // brightness) for frosted-glass panels. Transient heap working copy,
    // freed before return; falls back to a sharp restore on alloc failure.
    void blit_blurred_subimage(int x, int y, int srcW, int sx, int sy,
                               int w, int h, const uint16_t* data,
                               int radius, uint8_t keep = 220);

    // Pushes only window (x,y,w,h). Faster than push_frame() for sprites.
    void push_window(int x, int y, int w, int h);

    // ── Progress bar ──────────────────────────────────────
    void draw_progress_bar(int x, int y, int w, int h, uint8_t percent, uint16_t c);

private:
    void hw_reset();
    void set_window(int x0, int y0, int x1, int y1);
    void cmd(uint8_t v);
    void data8(uint8_t v);
    void data16(uint16_t v);
    void push_pixels(const uint8_t* data, size_t bytes);
    void push_fill(size_t pixel_count, uint16_t c);
    inline void put_px(int x, int y, uint16_t c);

    void*       m_spi = nullptr;   // spi_device_handle_t
    int         m_dc  = -1;
    bool        m_ready = false;
    uint16_t*   m_fb = nullptr;    // WIDTH*HEIGHT RGB565 framebuffer, PSRAM+DMA capable
};

} // namespace Fuchey
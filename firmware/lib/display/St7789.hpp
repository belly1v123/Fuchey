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
    int  text_width(std::string_view text, FontSize size) const;

    // ── Bitmap (1-bpp, row-major, MSB-first, byte-padded) ─
    void draw_bitmap(int x, int y, int w, int h, const uint8_t* mask, uint16_t fg);

    // ── RGB565 sprite blit (framebuffer-only, call push_* after) ─
    // data: w*h RGB565 pixels in CPU order (use convert_sprite.py output
    // with FU_CG_SWAP=0). Clipped to 240x240. No transparency.
    void draw_rgb565_image(int x, int y, int w, int h, const uint16_t* data);
    // Same but skips pixels == transparent (color-key, e.g. 0xF81F magenta).
    void draw_rgb565_image_transparent(int x, int y, int w, int h,
                                       const uint16_t* data, uint16_t transparent);

    // Pushes only the window (x,y,w,h) to the panel. Much faster than
    // push_frame() for small sprites: 96x96 = ~18KB vs 115KB full frame.
    // Data is read back from the framebuffer, so draw_* first, then call this.
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
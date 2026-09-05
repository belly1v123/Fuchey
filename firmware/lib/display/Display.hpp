#pragma once
// ============================================================
// Fuchey — Display.hpp
// ST7789 240x240 TFT driver (direct SPI, St7789 class).
// Immediate-mode drawing; flush() is a no-op kept for API
// compatibility with the old double-buffered OLED driver.
// ============================================================

#include "St7789.hpp"
#include <cstdint>
#include <string_view>

namespace Fuchey {

using Color = uint16_t;  // RGB565

// ─── Basic palette ────────────────────────────────────────
inline constexpr Color TFT_WHITE  = 0xFFFF;
inline constexpr Color TFT_BLACK  = 0x0000;
inline constexpr Color TFT_RED    = 0xF800;
inline constexpr Color TFT_GREEN  = 0x07E0;
inline constexpr Color TFT_YELLOW = 0xFFE0;
inline constexpr Color TFT_BLUE   = 0x001F;

namespace Colors {
    inline constexpr Color WHITE  = TFT_WHITE;
    inline constexpr Color BLACK  = TFT_BLACK;
    inline constexpr Color RED    = TFT_RED;
    inline constexpr Color GREEN  = TFT_GREEN;
    inline constexpr Color YELLOW = TFT_YELLOW;
    inline constexpr Color BLUE   = TFT_BLUE;
} // namespace Colors

class Display {
public:
    static constexpr int WIDTH  = 240;
    static constexpr int HEIGHT = 240;

    // Font sizes
    enum class FontSize { SMALL = 1, MEDIUM = 2, LARGE = 3 };

    explicit Display();
    ~Display() = default;

    // Non-copyable, non-movable (owns the SPI device)
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // ── Lifecycle ────────────────────────────────────────
    bool init();
    void power_on();
    void power_off();
    bool is_ready() const { return m_ready; }

    // ── Drawing (immediate) ──────────────────────────────
    void clear(Color c = TFT_BLACK);

    void draw_hline(int x, int y, int len, Color c = TFT_WHITE);
    void draw_rect(int x, int y, int w, int h, Color c = TFT_WHITE);
    void fill_rect(int x, int y, int w, int h, Color c = TFT_WHITE);

    // Text rendering (built-in 5x7 font, scaled)
    void draw_text(int x, int y, std::string_view text,
                   FontSize size = FontSize::SMALL, Color c = TFT_WHITE);
    void draw_text_centered(int y, std::string_view text,
                            FontSize size = FontSize::SMALL, Color c = TFT_WHITE);

    // Bitmap rendering (1-bpp, row-major, MSB-first, byte-padded rows).
    // Set bits are drawn with `fg`; clear bits are left untouched.
    void draw_bitmap(int x, int y, int w, int h, const uint8_t* mask, Color fg = TFT_WHITE);

    // Progress bar
    void draw_progress_bar(int x, int y, int w, int h, uint8_t percent,
                           Color c = TFT_WHITE);

    // Animated boot splash (title + label + growing progress bar)
    void animate_boot(uint32_t duration_ms = 3000);

    // ── Output ───────────────────────────────────────────
    // No-op: displays draw directly. Kept for API compatibility.
    bool flush();

private:
    St7789 m_lcd;
    bool   m_ready{false};

    static constexpr const char* TAG = "Display";
};

} // namespace Fuchey
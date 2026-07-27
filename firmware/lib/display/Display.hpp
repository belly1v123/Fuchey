#pragma once
// ============================================================
// Fuchey — Display.hpp
// SSD1306/SH1106 128x64 OLED driver via I2C.
// RAII: open on construction, closes I2C on destruction.
// Double-buffer: all drawing goes to back buffer; call
// flush() to send to hardware atomically.
// ============================================================

#include <driver/i2c.h>
#include <cstdint>
#include <array>
#include <string_view>

namespace Fuchey {

class Display {
public:
    static constexpr int WIDTH  = 128;
    static constexpr int HEIGHT = 64;
    static constexpr int PAGES  = HEIGHT / 8;  // 8 pages of 8 rows each
    static constexpr int BUF_SIZE = WIDTH * PAGES;

    // Font sizes
    enum class FontSize { SMALL = 1, MEDIUM = 2, LARGE = 3 };

    explicit Display(i2c_port_t port, uint8_t addr,
                     int sda_pin, int scl_pin, uint32_t freq_hz = 400000);
    ~Display();

    // Non-copyable, non-movable (owns I2C port)
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // ── Lifecycle ────────────────────────────────────────
    bool init();
    void reset();
    void power_on();
    void power_off();
    bool is_ready() const { return m_ready; }

    // ── Drawing (operates on back buffer) ────────────────
    void clear();
    void fill();

    // Pixel operations
    void set_pixel(int x, int y, bool on = true);
    bool get_pixel(int x, int y) const;

    // Lines and rectangles
    void draw_hline(int x, int y, int len);
    void draw_vline(int x, int y, int len);
    void draw_rect(int x, int y, int w, int h);
    void fill_rect(int x, int y, int w, int h);

    // Text rendering (uses built-in 5x7 font)
    void draw_char(int x, int y, char c, FontSize size = FontSize::SMALL);
    void draw_text(int x, int y, std::string_view text, FontSize size = FontSize::SMALL);

    // Centered text helpers
    void draw_text_centered(int y, std::string_view text, FontSize size = FontSize::SMALL);

    // Progress bar
    void draw_progress_bar(int x, int y, int w, int h, uint8_t percent);

    // ── Output ───────────────────────────────────────────
    // Push back buffer to hardware
    bool flush();

    // ── Scrolling ────────────────────────────────────────
    void scroll_right(uint8_t start_page, uint8_t end_page);
    void scroll_stop();

private:
    i2c_port_t  m_port;
    uint8_t     m_addr;
    int         m_sda;
    int         m_scl;
    uint32_t    m_freq;
    bool        m_ready{false};

    // Back buffer: [page][column]
    std::array<uint8_t, BUF_SIZE> m_buffer{};

    // ── Low-level I2C helpers ─────────────────────────────
    esp_err_t send_cmd(uint8_t cmd);
    esp_err_t send_data(const uint8_t* data, size_t len);
    esp_err_t send_cmd_list(const uint8_t* cmds, size_t count);

    // ── SSD1306 initialization sequence ──────────────────
    esp_err_t ssd1306_init();

    // ── Built-in font ─────────────────────────────────────
    // 5x7 ASCII font, chars 0x20–0x7E
    static const uint8_t FONT_5X7[][5];

    static constexpr const char* TAG = "Display";
};

} // namespace Fuchey

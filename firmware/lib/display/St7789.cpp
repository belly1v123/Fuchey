// ============================================================
// Fuchey — St7789.cpp
// ST7789 driver over the ESP-IDF SPI master driver.
// Bus + device setup is identical to the boot-time raw probe
// that produced visible RED/GREEN/WHITE fills on this panel.
// ============================================================

#include "St7789.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/gpio.h>
#include <driver/spi_master.h>

namespace Fuchey {

namespace {
constexpr const char* TAG = "St7789";

// ─── Classic 5x7 ASCII font (Adafruit glcdfont, BSD) ──────
// 95 glyphs, chars 0x20..0x7E, 5 bytes each. Each byte is one
// glyph row; bit 7 = leftmost pixel. Drawn in a 6x8 cell.
constexpr uint8_t kFont5x7[95 * 5] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // ' '
    0x00, 0x00, 0x5F, 0x00, 0x00, // '!'
    0x00, 0x07, 0x00, 0x07, 0x00, // '"'
    0x14, 0x7F, 0x14, 0x7F, 0x14, // '#'
    0x24, 0x2A, 0x7F, 0x2A, 0x12, // '$'
    0x23, 0x13, 0x08, 0x64, 0x62, // '%'
    0x36, 0x49, 0x56, 0x20, 0x50, // '&'
    0x00, 0x08, 0x07, 0x03, 0x00, // '\''
    0x00, 0x1C, 0x22, 0x41, 0x00, // '('
    0x00, 0x41, 0x22, 0x1C, 0x00, // ')'
    0x2A, 0x1C, 0x7F, 0x1C, 0x2A, // '*'
    0x08, 0x08, 0x3E, 0x08, 0x08, // '+'
    0x00, 0x80, 0x70, 0x30, 0x00, // ','
    0x08, 0x08, 0x08, 0x08, 0x08, // '-'
    0x00, 0x00, 0x60, 0x60, 0x00, // '.'
    0x20, 0x10, 0x08, 0x04, 0x02, // '/'
    0x3E, 0x51, 0x49, 0x45, 0x3E, // '0'
    0x00, 0x42, 0x7F, 0x40, 0x00, // '1'
    0x72, 0x49, 0x49, 0x49, 0x46, // '2'
    0x21, 0x41, 0x49, 0x4D, 0x33, // '3'
    0x18, 0x14, 0x12, 0x7F, 0x10, // '4'
    0x27, 0x45, 0x45, 0x45, 0x39, // '5'
    0x3C, 0x4A, 0x49, 0x49, 0x31, // '6'
    0x41, 0x21, 0x11, 0x09, 0x07, // '7'
    0x36, 0x49, 0x49, 0x49, 0x36, // '8'
    0x46, 0x49, 0x49, 0x29, 0x1E, // '9'
    0x00, 0x00, 0x14, 0x00, 0x00, // ':'
    0x00, 0x40, 0x34, 0x00, 0x00, // ';'
    0x00, 0x08, 0x14, 0x22, 0x41, // '<'
    0x14, 0x14, 0x14, 0x14, 0x14, // '='
    0x00, 0x41, 0x22, 0x14, 0x08, // '>'
    0x02, 0x01, 0x59, 0x09, 0x06, // '?'
    0x3E, 0x41, 0x5D, 0x59, 0x4E, // '@'
    0x7C, 0x12, 0x11, 0x12, 0x7C, // 'A'
    0x7F, 0x49, 0x49, 0x49, 0x36, // 'B'
    0x3E, 0x41, 0x41, 0x41, 0x22, // 'C'
    0x7F, 0x41, 0x41, 0x41, 0x3E, // 'D'
    0x7F, 0x49, 0x49, 0x49, 0x41, // 'E'
    0x7F, 0x09, 0x09, 0x09, 0x01, // 'F'
    0x3E, 0x41, 0x41, 0x51, 0x73, // 'G'
    0x7F, 0x08, 0x08, 0x08, 0x7F, // 'H'
    0x00, 0x41, 0x7F, 0x41, 0x00, // 'I'
    0x20, 0x40, 0x41, 0x3F, 0x01, // 'J'
    0x7F, 0x08, 0x14, 0x22, 0x41, // 'K'
    0x7F, 0x40, 0x40, 0x40, 0x40, // 'L'
    0x7F, 0x02, 0x1C, 0x02, 0x7F, // 'M'
    0x7F, 0x04, 0x08, 0x10, 0x7F, // 'N'
    0x3E, 0x41, 0x41, 0x41, 0x3E, // 'O'
    0x7F, 0x09, 0x09, 0x09, 0x06, // 'P'
    0x3E, 0x41, 0x51, 0x21, 0x5E, // 'Q'
    0x7F, 0x09, 0x19, 0x29, 0x46, // 'R'
    0x26, 0x49, 0x49, 0x49, 0x32, // 'S'
    0x03, 0x01, 0x7F, 0x01, 0x03, // 'T'
    0x3F, 0x40, 0x40, 0x40, 0x3F, // 'U'
    0x1F, 0x20, 0x40, 0x20, 0x1F, // 'V'
    0x3F, 0x40, 0x38, 0x40, 0x3F, // 'W'
    0x63, 0x14, 0x08, 0x14, 0x63, // 'X'
    0x03, 0x04, 0x78, 0x04, 0x03, // 'Y'
    0x61, 0x59, 0x49, 0x4D, 0x43, // 'Z'
    0x00, 0x7F, 0x41, 0x41, 0x41, // '['
    0x02, 0x04, 0x08, 0x10, 0x20, // '\'
    0x00, 0x41, 0x41, 0x41, 0x7F, // ']'
    0x04, 0x02, 0x01, 0x02, 0x04, // '^'
    0x40, 0x40, 0x40, 0x40, 0x40, // '_'
    0x00, 0x03, 0x07, 0x08, 0x00, // '`'
    0x20, 0x54, 0x54, 0x78, 0x40, // 'a'
    0x7F, 0x28, 0x44, 0x44, 0x38, // 'b'
    0x38, 0x44, 0x44, 0x44, 0x28, // 'c'
    0x38, 0x44, 0x44, 0x28, 0x7F, // 'd'
    0x38, 0x54, 0x54, 0x54, 0x18, // 'e'
    0x00, 0x08, 0x7E, 0x09, 0x02, // 'f'
    0x18, 0xA4, 0xA4, 0x9C, 0x78, // 'g'
    0x7F, 0x08, 0x04, 0x04, 0x78, // 'h'
    0x00, 0x44, 0x7D, 0x40, 0x00, // 'i'
    0x20, 0x40, 0x40, 0x3D, 0x00, // 'j'
    0x7F, 0x10, 0x28, 0x44, 0x00, // 'k'
    0x00, 0x41, 0x7F, 0x40, 0x00, // 'l'
    0x7C, 0x04, 0x78, 0x04, 0x78, // 'm'
    0x7C, 0x08, 0x04, 0x04, 0x78, // 'n'
    0x38, 0x44, 0x44, 0x44, 0x38, // 'o'
    0xFC, 0x18, 0x24, 0x24, 0x18, // 'p'
    0x18, 0x24, 0x24, 0x18, 0xFC, // 'q'
    0x7C, 0x08, 0x04, 0x04, 0x08, // 'r'
    0x48, 0x54, 0x54, 0x54, 0x24, // 's'
    0x04, 0x04, 0x3F, 0x44, 0x24, // 't'
    0x3C, 0x40, 0x40, 0x20, 0x7C, // 'u'
    0x1C, 0x20, 0x40, 0x20, 0x1C, // 'v'
    0x3C, 0x40, 0x30, 0x40, 0x3C, // 'w'
    0x44, 0x28, 0x10, 0x28, 0x44, // 'x'
    0x4C, 0x90, 0x90, 0x90, 0x7C, // 'y'
    0x44, 0x64, 0x54, 0x4C, 0x44, // 'z'
    0x00, 0x08, 0x36, 0x41, 0x00, // '{'
    0x00, 0x00, 0x77, 0x00, 0x00, // '|'
    0x00, 0x41, 0x36, 0x08, 0x00, // '}'
    0x02, 0x01, 0x02, 0x04, 0x02, // '~'
};

inline void set_dc(int pin, bool high) {
    gpio_set_level(static_cast<gpio_num_t>(pin), high ? 1 : 0);
}
} // namespace

// ─── Low-level SPI ─────────────────────────────────────────
bool St7789::init() {
    constexpr int PIN_SCLK = DisplayConfig::PIN_SCLK;
    constexpr int PIN_MOSI = DisplayConfig::PIN_MOSI;
    constexpr int PIN_CS   = DisplayConfig::PIN_CS;
    constexpr int PIN_DC   = DisplayConfig::PIN_DC;
    constexpr int PIN_RST  = DisplayConfig::PIN_RST;

    gpio_config_t io{};
    io.mode            = GPIO_MODE_OUTPUT;
    io.pull_up_en      = GPIO_PULLUP_DISABLE;
    io.pull_down_en    = GPIO_PULLDOWN_DISABLE;
    io.pin_bit_mask    = (1ULL << PIN_DC) | (1ULL << PIN_RST);
    gpio_config(&io);
    set_dc(PIN_DC, false);
    gpio_set_level(static_cast<gpio_num_t>(PIN_RST), 1);
    m_dc = PIN_DC;

    spi_bus_config_t bus{};
    bus.mosi_io_num    = PIN_MOSI;
    bus.miso_io_num    = -1;
    bus.sclk_io_num    = PIN_SCLK;
    bus.quadwp_io_num  = -1;
    bus.quadhd_io_num  = -1;
    bus.max_transfer_sz = 4096;

    esp_err_t err = spi_bus_initialize(DisplayConfig::SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed (%s)", esp_err_to_name(err));
        return false;
    }

    spi_device_interface_config_t dev{};
    dev.clock_speed_hz = DisplayConfig::SPI_FREQ_HZ;
    dev.mode           = 0;
    dev.spics_io_num   = PIN_CS;
    dev.queue_size     = 8;
    err = spi_bus_add_device(DisplayConfig::SPI_HOST, &dev, reinterpret_cast<spi_device_handle_t*>(&m_spi));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed (%s)", esp_err_to_name(err));
        spi_bus_free(DisplayConfig::SPI_HOST);
        return false;
    }

    hw_reset();

    cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150)); // SWRESET
    cmd(0x11); vTaskDelay(pdMS_TO_TICKS(200)); // SLPOUT
    cmd(0x36); data8(0x00);                    // MADCTL
    cmd(0x3A); data8(0x55);                    // COLMOD 16bpp
    cmd(0x21);                                 // INVON
    cmd(0x13);                                 // NORON
    cmd(0x29);                                 // DISPON
    vTaskDelay(pdMS_TO_TICKS(80));

    m_ready = true;
    ESP_LOGI(TAG, "ST7789 ready (%dMHz, SCLK=%d MOSI=%d CS=%d DC=%d RST=%d)",
             DisplayConfig::SPI_FREQ_HZ / 1000000, PIN_SCLK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST);
    return true;
}

void St7789::hw_reset() {
    gpio_set_level(static_cast<gpio_num_t>(DisplayConfig::PIN_RST), 0);
    vTaskDelay(pdMS_TO_TICKS(40));
    gpio_set_level(static_cast<gpio_num_t>(DisplayConfig::PIN_RST), 1);
    vTaskDelay(pdMS_TO_TICKS(150));
}

void St7789::cmd(uint8_t v) {
    set_dc(m_dc, false);
    spi_transaction_t t{};
    t.length    = 8;
    t.tx_buffer = &v;
    spi_device_transmit(static_cast<spi_device_handle_t>(m_spi), &t);
}

void St7789::data8(uint8_t v) {
    set_dc(m_dc, true);
    spi_transaction_t t{};
    t.length    = 8;
    t.tx_buffer = &v;
    spi_device_transmit(static_cast<spi_device_handle_t>(m_spi), &t);
}

void St7789::data16(uint16_t v) {
    set_dc(m_dc, true);
    uint8_t buf[2] = { static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xFF) };
    spi_transaction_t t{};
    t.length    = 16;
    t.tx_buffer = buf;
    spi_device_transmit(static_cast<spi_device_handle_t>(m_spi), &t);
}

void St7789::set_window(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DisplayConfig::WIDTH - 1)  x1 = DisplayConfig::WIDTH - 1;
    if (y1 > DisplayConfig::HEIGHT - 1) y1 = DisplayConfig::HEIGHT - 1;
    cmd(0x2A); // CASET
    data16(static_cast<uint16_t>(x0)); data16(static_cast<uint16_t>(x1));
    cmd(0x2B); // PASET
    data16(static_cast<uint16_t>(y0)); data16(static_cast<uint16_t>(y1));
    cmd(0x2C); // RAMWR
}

void St7789::push_pixels(const uint8_t* data, size_t bytes) {
    set_dc(m_dc, true);
    spi_transaction_t t{};
    t.length    = static_cast<int>(bytes * 8);
    t.tx_buffer = data;
    spi_device_transmit(static_cast<spi_device_handle_t>(m_spi), &t);
}

void St7789::push_fill(size_t pixel_count, uint16_t c) {
    uint8_t buf[1024];
    const uint8_t hi = static_cast<uint8_t>(c >> 8);
    const uint8_t lo = static_cast<uint8_t>(c & 0xFF);
    for (size_t i = 0; i + 1 < sizeof(buf); i += 2) {
        buf[i] = hi;
        buf[i + 1] = lo;
    }
    set_dc(m_dc, true);
    while (pixel_count > 0) {
        size_t chunk = (pixel_count * 2 > sizeof(buf)) ? (sizeof(buf) / 2) : pixel_count;
        spi_transaction_t t{};
        t.length    = static_cast<int>(chunk * 16);
        t.tx_buffer = buf;
        spi_device_transmit(static_cast<spi_device_handle_t>(m_spi), &t);
        pixel_count -= chunk;
    }
}

// ─── Power ─────────────────────────────────────────────────
void St7789::power_on()  { cmd(0x11); vTaskDelay(pdMS_TO_TICKS(20)); }
void St7789::power_off() { cmd(0x10); } // SLEEPIN

// ─── Shapes ────────────────────────────────────────────────
void St7789::fill_screen(uint16_t c) {
    set_window(0, 0, DisplayConfig::WIDTH - 1, DisplayConfig::HEIGHT - 1);
    push_fill(static_cast<size_t>(DisplayConfig::WIDTH) * DisplayConfig::HEIGHT, c);
}

void St7789::fill_rect(int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    if (x >= DisplayConfig::WIDTH || y >= DisplayConfig::HEIGHT) return;
    if (x + w <= 0 || y + h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w > DisplayConfig::WIDTH - x)  w = DisplayConfig::WIDTH - x;
    if (h > DisplayConfig::HEIGHT - y) h = DisplayConfig::HEIGHT - y;
    set_window(x, y, x + w - 1, y + h - 1);
    push_fill(static_cast<size_t>(w) * h, c);
}

void St7789::draw_rect(int x, int y, int w, int h, uint16_t c) {
    if (w < 0 || h < 0) return;
    if (w == 0 || h == 0) return;
    draw_hline(x, y, w, c);
    draw_hline(x, y + h - 1, w, c);
    draw_vline(x, y, h, c);
    draw_vline(x + w - 1, y, h, c);
}

void St7789::draw_hline(int x, int y, int len, uint16_t c) { fill_rect(x, y, len, 1, c); }
void St7789::draw_vline(int x, int y, int len, uint16_t c) { fill_rect(x, y, 1, len, c); }

// ─── Text ──────────────────────────────────────────────────
void St7789::draw_text(int x, int y, std::string_view text, FontSize size, uint16_t color) {
    const int scale = static_cast<int>(size);
    if (scale <= 0) return;

    // glcdfont is column-major: each of the 5 bytes is one glyph
    // column, and bit k of that byte is the pixel at row k (bit0 =
    // top row). Reproduces Adafruit_GFX's classic drawChar exactly.
    int xpos = x;
    int ypos = y;
    for (char ch : text) {
        if (ch == '\n') {
            xpos = x;
            ypos += 8 * scale;
            continue;
        }
        uint8_t c = static_cast<uint8_t>(ch);
        if (c < 0x20 || c > 0x7E) c = ' ';
        const uint8_t* g = &kFont5x7[(c - 0x20) * 5];
        for (int col = 0; col < 5; ++col) {
            uint8_t b = g[col];
            int run_start = -1;
            for (int row = 0; row <= 7; ++row) {
                bool on = (b & (1u << row)) != 0;
                if (on && run_start < 0) run_start = row;
                if (!on && run_start >= 0) {
                    fill_rect(xpos + col * scale, ypos + run_start * scale,
                              scale, (row - run_start) * scale, color);
                    run_start = -1;
                }
            }
            if (run_start >= 0) {
                fill_rect(xpos + col * scale, ypos + run_start * scale,
                          scale, (8 - run_start) * scale, color);
            }
        }
        xpos += 6 * scale;
    }
}

int St7789::text_width(std::string_view text, FontSize size) const {
    return static_cast<int>(text.size()) * 6 * static_cast<int>(size);
}

// ─── Bitmap ────────────────────────────────────────────────
void St7789::draw_bitmap(int x, int y, int w, int h, const uint8_t* mask, uint16_t fg) {
    const int bytes_per_row = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const uint8_t* src = mask + row * bytes_per_row;
        int run_start = -1;
        for (int col = 0; col <= w; ++col) {
            bool on = (col < w) && (src[col / 8] & (0x80u >> (col % 8))) != 0;
            if (on && run_start < 0) run_start = col;
            if (!on && run_start >= 0) {
                fill_rect(x + run_start, y + row, col - run_start, 1, fg);
                run_start = -1;
            }
        }
    }
}

// ─── Progress bar ──────────────────────────────────────────
void St7789::draw_progress_bar(int x, int y, int w, int h, uint8_t percent, uint16_t c) {
    if (percent > 100) percent = 100;
    draw_rect(x, y, w, h, c);
    int fill = (w - 2) * percent / 100;
    if (fill > 0) fill_rect(x + 1, y + 1, fill, h - 2, c);
}

} // namespace Fuchey
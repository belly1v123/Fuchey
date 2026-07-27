// ============================================================
// Fuchey — Display.cpp
// SSD1306 128x64 OLED I2C driver implementation
// ============================================================

#include "Display.hpp"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

namespace Fuchey {

// ─── SSD1306 Commands ─────────────────────────────────────
namespace Cmd {
    static constexpr uint8_t DISPLAY_OFF         = 0xAE;
    static constexpr uint8_t DISPLAY_ON          = 0xAF;
    static constexpr uint8_t SET_CONTRAST        = 0x81;
    static constexpr uint8_t ENTIRE_ON_RESUME    = 0xA4;
    static constexpr uint8_t NORMAL_DISPLAY      = 0xA6;
    static constexpr uint8_t INVERT_DISPLAY      = 0xA7;
    // Not used: 0x20 (MEM_ADDR_MODE) — SH1106 doesn't support it.
    static constexpr uint8_t SET_DISP_CLK_DIV   = 0xD5;
    static constexpr uint8_t SET_MUX_RATIO       = 0xA8;
    static constexpr uint8_t SET_DISP_OFFSET     = 0xD3;
    static constexpr uint8_t SET_START_LINE      = 0x40;
    static constexpr uint8_t CHARGE_PUMP         = 0x8D;
    static constexpr uint8_t SEG_REMAP           = 0xA1;
    static constexpr uint8_t COM_SCAN_DEC        = 0xC8;
    static constexpr uint8_t SET_COM_PINS        = 0xDA;
    static constexpr uint8_t SET_PRECHARGE       = 0xD9;
    static constexpr uint8_t SET_VCOM_DESEL      = 0xDB;
    // Not used: 0x21 (SET_COL_ADDR), 0x22 (SET_PAGE_ADDR) — SH1106 doesn't support them.
}

// ─── Built-in 5x7 ASCII font (0x20 – 0x7E) ───────────────
// Each entry: 5 bytes, LSB = top row
const uint8_t Display::FONT_5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '\''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x08,0x2A,0x1C,0x2A,0x08}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x00,0x08,0x14,0x22,0x41}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x41,0x22,0x14,0x08,0x00}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x01,0x01}, // 'F'
    {0x3E,0x41,0x41,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x7F,0x20,0x18,0x20,0x7F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x03,0x04,0x78,0x04,0x03}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x00,0x7F,0x41,0x41}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x41,0x41,0x7F,0x00,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
    {0x00,0x01,0x02,0x04,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 'f'
    {0x08,0x14,0x54,0x54,0x3C}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 'j'
    {0x00,0x7F,0x10,0x28,0x44}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 'z'
    {0x00,0x08,0x36,0x41,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00}, // '}'
    {0x08,0x08,0x2A,0x1C,0x08}, // '~'
};

// ─── Constructor / Destructor ─────────────────────────────
Display::Display(i2c_port_t port, uint8_t addr, int sda, int scl, uint32_t freq)
    : m_port(port), m_addr(addr), m_sda(sda), m_scl(scl), m_freq(freq) {}

Display::~Display() {
    i2c_driver_delete(m_port);
    ESP_LOGI(TAG, "I2C driver released");
}

static bool probe_i2c_address(i2c_port_t port, uint8_t addr) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(h);
    esp_err_t err = i2c_master_cmd_begin(port, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return (err == ESP_OK);
}

// ─── init ─────────────────────────────────────────────────
bool Display::init() {
    struct PinPair { int sda; int scl; };
    const PinPair pin_pairs[] = {
        { m_sda, m_scl }, // User configured pins first (8, 9)
        { 17, 18 },
        { 4, 5 },
        { 21, 22 },
        { 1, 2 },
        { 5, 6 },
        { 41, 42 },
        { 15, 16 },
    };

    bool found = false;
    for (const auto& pair : pin_pairs) {
        i2c_driver_delete(m_port);
        i2c_config_t conf{};
        conf.mode             = I2C_MODE_MASTER;
        conf.sda_io_num       = static_cast<gpio_num_t>(pair.sda);
        conf.scl_io_num       = static_cast<gpio_num_t>(pair.scl);
        conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.master.clk_speed = m_freq;

        if (i2c_param_config(m_port, &conf) != ESP_OK) continue;
        if (i2c_driver_install(m_port, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) continue;

        if (probe_i2c_address(m_port, m_addr)) {
            m_sda = pair.sda;
            m_scl = pair.scl;
            found = true;
            break;
        }
        uint8_t alt_addr = (m_addr == 0x3C) ? 0x3D : 0x3C;
        if (probe_i2c_address(m_port, alt_addr)) {
            m_sda = pair.sda;
            m_scl = pair.scl;
            m_addr = alt_addr;
            found = true;
            break;
        }
    }

    if (!found) {
        ESP_LOGE(TAG, "No OLED display detected on tested I2C pins!");
        // Re-install configured pins anyway so system doesn't crash
        i2c_driver_delete(m_port);
        i2c_config_t conf{};
        conf.mode             = I2C_MODE_MASTER;
        conf.sda_io_num       = static_cast<gpio_num_t>(m_sda);
        conf.scl_io_num       = static_cast<gpio_num_t>(m_scl);
        conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.master.clk_speed = m_freq;
        i2c_param_config(m_port, &conf);
        i2c_driver_install(m_port, I2C_MODE_MASTER, 0, 0, 0);
        return false;
    }

    ESP_LOGI(TAG, "OLED detected on SDA=%d SCL=%d Addr=0x%02X", m_sda, m_scl, m_addr);

    esp_err_t err = ssd1306_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init failed: %s", esp_err_to_name(err));
        return false;
    }

    m_ready = true;
    clear();
    draw_text_centered(20, "FUCHEY", Display::FontSize::LARGE);
    draw_text_centered(50, "Initializing...", Display::FontSize::SMALL);
    flush();
    ESP_LOGI(TAG, "SSD1306 128x64 ready on I2C%d addr=0x%02X", m_port, m_addr);
    return true;
}

// ─── SH1106 init sequence ────────────────────────────────
esp_err_t Display::ssd1306_init() {
    vTaskDelay(pdMS_TO_TICKS(50));
    static const uint8_t init_cmds[] = {
        Cmd::DISPLAY_OFF,
        Cmd::SET_DISP_CLK_DIV, 0x80,
        Cmd::SET_MUX_RATIO,    0x3F,
        Cmd::SET_DISP_OFFSET,  0x00,
        Cmd::SET_START_LINE | 0x00,
        Cmd::CHARGE_PUMP,      0x14,
        Cmd::SEG_REMAP,
        Cmd::COM_SCAN_DEC,
        Cmd::SET_COM_PINS,     0x12,
        Cmd::SET_CONTRAST,     0x80,
        Cmd::SET_PRECHARGE,    0x22,
        Cmd::SET_VCOM_DESEL,   0x30,
        Cmd::ENTIRE_ON_RESUME,
        Cmd::NORMAL_DISPLAY,
        Cmd::DISPLAY_ON,
    };
    return send_cmd_list(init_cmds, sizeof(init_cmds));
}

// ─── Power ────────────────────────────────────────────────
void Display::power_on()  { send_cmd(Cmd::DISPLAY_ON);  }
void Display::power_off() { send_cmd(Cmd::DISPLAY_OFF); }

void Display::reset() {
    m_buffer.fill(0);
    flush();
}

// ─── Drawing ──────────────────────────────────────────────
void Display::clear() { m_buffer.fill(0x00); }
void Display::fill()  { m_buffer.fill(0xFF); }

void Display::set_pixel(int x, int y, bool on) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    int idx = x + (y / 8) * WIDTH;
    if (on) m_buffer[idx] |=  (1u << (y % 8));
    else    m_buffer[idx] &= ~(1u << (y % 8));
}

bool Display::get_pixel(int x, int y) const {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return false;
    return (m_buffer[x + (y / 8) * WIDTH] >> (y % 8)) & 1;
}

void Display::draw_hline(int x, int y, int len) {
    for (int i = 0; i < len; ++i) set_pixel(x + i, y);
}

void Display::draw_vline(int x, int y, int len) {
    for (int i = 0; i < len; ++i) set_pixel(x, y + i);
}

void Display::draw_rect(int x, int y, int w, int h) {
    draw_hline(x, y,         w);
    draw_hline(x, y + h - 1, w);
    draw_vline(x,         y, h);
    draw_vline(x + w - 1, y, h);
}

void Display::fill_rect(int x, int y, int w, int h) {
    for (int row = y; row < y + h; ++row)
        draw_hline(x, row, w);
}

// ─── Text ─────────────────────────────────────────────────
void Display::draw_char(int x, int y, char c, FontSize size) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t* glyph = FONT_5X7[c - 0x20];
    int s = static_cast<int>(size);
    for (int col = 0; col < 5; ++col) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if (line & (1 << row)) {
                for (int dy = 0; dy < s; ++dy)
                    for (int dx = 0; dx < s; ++dx)
                        set_pixel(x + col * s + dx, y + row * s + dy);
            }
        }
    }
}

void Display::draw_text(int x, int y, std::string_view text, FontSize size) {
    int s = static_cast<int>(size);
    int char_w = (5 + 1) * s; // 5 cols + 1 spacing
    int cx = x;
    for (char c : text) {
        if (cx + char_w > WIDTH) break;
        draw_char(cx, y, c, size);
        cx += char_w;
    }
}

void Display::draw_text_centered(int y, std::string_view text, FontSize size) {
    int s = static_cast<int>(size);
    int char_w = (5 + 1) * s;
    int total_w = static_cast<int>(text.size()) * char_w;
    int x = std::max(0, (WIDTH - total_w) / 2);
    draw_text(x, y, text, size);
}

void Display::draw_progress_bar(int x, int y, int w, int h, uint8_t percent) {
    draw_rect(x, y, w, h);
    int fill = static_cast<int>((w - 2) * percent / 100);
    fill_rect(x + 1, y + 1, fill, h - 2);
}

// ─── flush ────────────────────────────────────────────────
bool Display::flush() {
    if (!m_ready) return false;
    for (int page = 0; page < 8; ++page) {
        send_cmd(0xB0 | page);
        send_cmd(0x02);
        send_cmd(0x10);
        esp_err_t err = send_data(m_buffer.data() + page * 128, 128);
        if (err != ESP_OK) return false;
    }
    return true;
}

// ─── Scroll ───────────────────────────────────────────────
void Display::scroll_right(uint8_t start, uint8_t end) {
    uint8_t cmds[] = { 0x26, 0x00, start, 0x00, end, 0x00, 0xFF, 0x2F };
    send_cmd_list(cmds, sizeof(cmds));
}
void Display::scroll_stop() { send_cmd(0x2E); }

// ─── Low-level I2C ────────────────────────────────────────
esp_err_t Display::send_cmd(uint8_t cmd) {
    uint8_t buf[2] = { 0x00, cmd }; // Co=0, D/C=0
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (m_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(h, buf, 2, true);
    i2c_master_stop(h);
    esp_err_t err = i2c_master_cmd_begin(m_port, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
    return err;
}

esp_err_t Display::send_cmd_list(const uint8_t* cmds, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        esp_err_t err = send_cmd(cmds[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t Display::send_data(const uint8_t* data, size_t len) {
    // Send in chunks to avoid I2C buffer overflow
    static constexpr size_t CHUNK = 128;
    for (size_t offset = 0; offset < len; offset += CHUNK) {
        size_t chunk_len = std::min(CHUNK, len - offset);
        i2c_cmd_handle_t h = i2c_cmd_link_create();
        i2c_master_start(h);
        i2c_master_write_byte(h, (m_addr << 1) | I2C_MASTER_WRITE, true);
        uint8_t ctrl = 0x40; // Co=0, D/C=1 (data)
        i2c_master_write_byte(h, ctrl, true);
        i2c_master_write(h, data + offset, chunk_len, true);
        i2c_master_stop(h);
        esp_err_t err = i2c_master_cmd_begin(m_port, h, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(h);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

} // namespace Fuchey

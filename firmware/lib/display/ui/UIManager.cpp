// ============================================================
// Fuchey — UIManager.cpp
// UI state machine, OLED rendering, and setup wizard.
// ============================================================

#include "UIManager.hpp"
#include "../../config/Config.hpp"
#include "../../buttons/ButtonDriver.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "../../wallet/WalletCore.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <time.h>
extern "C" {
#include "qrcodegen.h"
}

namespace Fuchey {

static constexpr const char* TAG = "UIManager";

UIManager::UIManager(Display& display) : m_display(display) {}

bool UIManager::init() {
    m_last_idle_cycle_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "UIManager initialized");
    return true;
}

void UIManager::set_screen(UIScreen screen) {
    m_current_screen = screen;
    render();
}

// ─── Idle screen cycling ──────────────────────────────────
void UIManager::cycle_idle_screen() {
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now - m_last_idle_cycle_ms >= Timing::IDLE_SCREEN_CYCLE_MS) {
        m_last_idle_cycle_ms = now;
        switch (m_current_screen) {
            case UIScreen::IDLE_CLOCK:   m_current_screen = UIScreen::IDLE_WEATHER; break;
            case UIScreen::IDLE_WEATHER: m_current_screen = UIScreen::IDLE_PRICE;   break;
            case UIScreen::IDLE_PRICE:   m_current_screen = UIScreen::IDLE_MESSAGE; break;
            case UIScreen::IDLE_MESSAGE: m_current_screen = UIScreen::IDLE_CLOCK;   break;
            default: break; // Stay on active wallet/menu screens
        }
    }
}

// ─── Event processing ─────────────────────────────────────
void UIManager::process_event(const Events::Event& evt) {
    switch (evt.type) {
        case Events::EventType::WIFI_GOT_IP:
            ESP_LOGI(TAG, "WIFI_GOT_IP received — advancing setup stage");
            on_wifi_got_ip();
            break;

        case Events::EventType::WEATHER_UPDATED:
            m_weather_temp = evt.data.weather.temp_celsius;
            m_weather_city = evt.data.weather.city;
            ESP_LOGI(TAG, "Weather updated: %.1f C in %s", m_weather_temp, m_weather_city.c_str());
            break;

        case Events::EventType::PRICE_UPDATED:
            m_sol_price = evt.data.price.sol_usd;
            ESP_LOGI(TAG, "SOL price updated: $%.2f", m_sol_price);
            break;

        case Events::EventType::AI_RESPONSE_READY:
            m_last_ai_response = evt.data.chat.text;
            set_screen(UIScreen::CHAT_VIEW);
            break;

        case Events::EventType::TX_REQUEST:
            m_tx_amount_cents = evt.data.tx.amount_cents;
            ESP_LOGI(TAG, "TX request received: $%.2f — showing confirmation",
                     static_cast<double>(m_tx_amount_cents) / 100.0);
            set_screen(UIScreen::TX_CONFIRM);
            break;

        default:
            break;
    }
}

// ─── Setup wizard ─────────────────────────────────────────
void UIManager::set_setup_needed(bool wifi_missing, bool wallet_missing) {
    m_setup_needed = wifi_missing || wallet_missing;

    if (!m_setup_needed) {
        m_setup_stage = SetupStage::DONE;
        return;
    }

    // Determine starting stage
    if (wifi_missing) {
        m_setup_stage = SetupStage::WIFI_PROMPT;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "  SETUP WIZARD: Step 1 — Enter WiFi credentials");
        ESP_LOGI(TAG, "  Serial command:  w <SSID> <PASSWORD>");
        ESP_LOGI(TAG, "=================================================");
    } else {
        // WiFi already saved, skip to wallet
        m_setup_stage = SetupStage::WALLET_PROMPT;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "  SETUP WIZARD: WiFi OK — Step 2: Wallet setup");
        ESP_LOGI(TAG, "  wallet_create              Generate new wallet");
        ESP_LOGI(TAG, "  wallet_import <mnemonic>   Import BIP39 mnemonic");
        ESP_LOGI(TAG, "  wallet_import <key>        Import hex/base58 private key");
        ESP_LOGI(TAG, "=================================================");
    }
}

void UIManager::mark_wifi_configured(const char* ssid) {
    if (ssid) m_connecting_ssid = ssid;
    m_connecting_dots_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    m_connecting_dots = 0;
    // Advance to connecting state — wait for actual IP before moving to wallet setup
    if (m_setup_stage == SetupStage::WIFI_PROMPT) {
        m_setup_stage = SetupStage::WIFI_CONNECTING;
        ESP_LOGI(TAG, "-------------------------------------------------");
        ESP_LOGI(TAG, "  [WiFi] Connecting to '%s' ...", m_connecting_ssid.c_str());
        ESP_LOGI(TAG, "  Waiting for IP address...");
        ESP_LOGI(TAG, "-------------------------------------------------");
    }
}

void UIManager::on_wifi_got_ip() {
    if (m_setup_stage == SetupStage::WIFI_CONNECTING) {
        m_setup_stage = SetupStage::WALLET_PROMPT;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "  [WiFi] CONNECTED! IP obtained.");
        ESP_LOGI(TAG, "  SETUP WIZARD: Step 2 — Wallet setup");
        ESP_LOGI(TAG, "  wallet_create              Generate new wallet");
        ESP_LOGI(TAG, "  wallet_import <mnemonic>   Import BIP39 mnemonic");
        ESP_LOGI(TAG, "  wallet_import <key>        Import hex/base58 private key");
        ESP_LOGI(TAG, "=================================================");
    }
}

void UIManager::mark_wallet_configured(const char* address) {
    if (address) m_wallet_address = address;
    m_setup_needed = false;
    m_setup_stage  = SetupStage::DONE;
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  SETUP COMPLETE — Entering idle mode");
    if (!m_wallet_address.empty()) {
        ESP_LOGI(TAG, "  Wallet: %s", m_wallet_address.c_str());
    }
    ESP_LOGI(TAG, "=================================================");
    set_screen(UIScreen::IDLE_CLOCK);
}

// ─── Render dispatch ──────────────────────────────────────
void UIManager::render() {
    m_display.clear();

    if (m_setup_needed) {
        render_setup();
        m_display.flush();
        return;
    }

    switch (m_current_screen) {
        case UIScreen::IDLE_CLOCK:   render_clock();       break;
        case UIScreen::IDLE_WEATHER: render_weather();     break;
        case UIScreen::IDLE_PRICE:   render_price();       break;
        case UIScreen::IDLE_MESSAGE: render_message();     break;
        case UIScreen::MENU_MAIN:    render_menu();        break;
        case UIScreen::WALLET_INFO:  render_wallet_info(); break;
        case UIScreen::WALLET_QR:    render_wallet_qr();   break;
        case UIScreen::TX_CONFIRM:   render_tx_confirm();  break;
        case UIScreen::CHAT_VIEW:    render_chat();        break;
    }

    m_display.flush();
}

// ─── Idle screens ─────────────────────────────────────────
void UIManager::render_clock() {
    m_display.draw_text_centered(2, "FUCHEY WALLET", Display::FontSize::SMALL);
    m_display.draw_hline(0, 12, 128);

    char buf[16];
    time_t now = time(nullptr);
    struct tm timeinfo;
    if (now > 100000 && localtime_r(&now, &timeinfo)) {
        strftime(buf, sizeof(buf), "%I:%M %p", &timeinfo);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    m_display.draw_text_centered(22, buf, Display::FontSize::MEDIUM);

    char date_buf[20];
    if (now > 100000) {
        struct tm ti;
        localtime_r(&now, &ti);
        strftime(date_buf, sizeof(date_buf), "%a %b %d", &ti);
    } else {
        snprintf(date_buf, sizeof(date_buf), "Syncing...");
    }
    m_display.draw_text_centered(50, date_buf, Display::FontSize::SMALL);
}

void UIManager::render_weather() {
    m_display.draw_text_centered(2, "WEATHER", Display::FontSize::SMALL);
    m_display.draw_hline(0, 12, 128);

    char buf[32];
    if (m_weather_temp < -100.0f) {
        snprintf(buf, sizeof(buf), "--.- C");
    } else {
        snprintf(buf, sizeof(buf), "%.1f C", m_weather_temp);
    }
    m_display.draw_text_centered(22, buf, Display::FontSize::LARGE);
    m_display.draw_text_centered(50, m_weather_city.c_str(), Display::FontSize::SMALL);
}

void UIManager::render_price() {
    m_display.draw_text_centered(2, "SOLANA", Display::FontSize::SMALL);
    m_display.draw_hline(0, 12, 128);

    char buf[32];
    if (m_sol_price < 0.0f) {
        snprintf(buf, sizeof(buf), "$--.--");
    } else {
        snprintf(buf, sizeof(buf), "$%.2f", m_sol_price);
    }
    m_display.draw_text_centered(22, buf, Display::FontSize::LARGE);
    m_display.draw_text_centered(50, "CoinGecko", Display::FontSize::SMALL);
}

void UIManager::render_message() {
    m_display.draw_text_centered(2, "FUCHEY", Display::FontSize::SMALL);
    m_display.draw_hline(0, 12, 128);
    m_display.draw_text_centered(28, "Have a great day!", Display::FontSize::SMALL);
    m_display.draw_text_centered(44, "Solana hardware", Display::FontSize::SMALL);
    m_display.draw_text_centered(54, "wallet + AI desk", Display::FontSize::SMALL);
}

// ─── Menu / Wallet / Chat screens ─────────────────────────
void UIManager::render_menu() {
    m_display.draw_text_centered(2, "MAIN MENU", Display::FontSize::SMALL);
    m_display.draw_hline(0, 12, 128);

    m_display.draw_text(8, 16, m_menu_index == 0 ? "> Wallet Info" : "  Wallet Info", Display::FontSize::SMALL);
    m_display.draw_text(8, 28, m_menu_index == 1 ? "> AI Assistant" : "  AI Assistant", Display::FontSize::SMALL);
    m_display.draw_text(8, 40, m_menu_index == 2 ? "> SOL Price" : "  SOL Price", Display::FontSize::SMALL);

    m_display.draw_text(2, 54, "1x:Select 2x:Next", Display::FontSize::SMALL);
}

void UIManager::render_wallet_info() {
    m_display.draw_text_centered(2, "WALLET INFO", Display::FontSize::SMALL);
    m_display.draw_hline(0, 12, 128);

    // Auto-fetch cached address via global pointer set in app_main()
    if (m_wallet_address.empty()) {
        extern Fuchey::WalletCore* g_wallet_core_ptr;
        if (g_wallet_core_ptr) {
            auto addr = g_wallet_core_ptr->get_address();
            if (addr) m_wallet_address = *addr;
        }
    }

    if (!m_wallet_address.empty()) {
        // Split the full address across up to 3 lines of 15 chars each
        // SMALL font ~6px/char on 128px → ~21 chars fit; use 15 for safe centering
        static constexpr size_t CHUNK = 15;
        const char* p   = m_wallet_address.c_str();
        size_t      len = m_wallet_address.size();

        char line1[CHUNK + 1] = {};
        char line2[CHUNK + 1] = {};
        char line3[CHUNK + 1] = {};

        size_t l1 = (len >= CHUNK)        ? CHUNK : len;
        size_t l2 = (len >= CHUNK * 2)    ? CHUNK : (len > CHUNK ? len - CHUNK : 0);
        size_t l3 = (len >  CHUNK * 2)    ? len - CHUNK * 2 : 0;
        if (l3 > CHUNK) l3 = CHUNK;

        strncpy(line1, p,              l1); line1[l1] = '\0';
        strncpy(line2, p + CHUNK,      l2); line2[l2] = '\0';
        strncpy(line3, p + CHUNK * 2,  l3); line3[l3] = '\0';

        m_display.draw_text_centered(16, line1, Display::FontSize::SMALL);
        if (l2) m_display.draw_text_centered(27, line2, Display::FontSize::SMALL);
        if (l3) m_display.draw_text_centered(38, line3, Display::FontSize::SMALL);
        m_display.draw_text_centered(52, "b: Back", Display::FontSize::SMALL);
    } else {
        m_display.draw_text_centered(30, "No wallet setup", Display::FontSize::SMALL);
        m_display.draw_text_centered(52, "b: Back", Display::FontSize::SMALL);
    }
}

void UIManager::render_wallet_qr() {
    // Ensure we have the wallet address cached
    if (m_wallet_address.empty()) {
        extern Fuchey::WalletCore* g_wallet_core_ptr;
        if (g_wallet_core_ptr) {
            auto addr = g_wallet_core_ptr->get_address();
            if (addr) m_wallet_address = *addr;
        }
    }

    if (m_wallet_address.empty()) {
        m_display.draw_text_centered(28, "No wallet", Display::FontSize::SMALL);
        m_display.draw_text_centered(42, "b: Back",   Display::FontSize::SMALL);
        return;
    }

    // qrcodegen buffers (stack-allocated, no heap needed)
    // Version 10 max → (4*10+17)^2 = 57^2 = 3249 bits → 407 bytes per buffer
    static constexpr int MAX_VERSION = 10;
    static constexpr int BUF_LEN = qrcodegen_BUFFER_LEN_FOR_VERSION(MAX_VERSION);
    uint8_t qr_code[BUF_LEN];
    uint8_t tmp_buf[BUF_LEN];

    bool ok = qrcodegen_encodeText(
        m_wallet_address.c_str(),
        tmp_buf,
        qr_code,
        qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN,
        MAX_VERSION,
        qrcodegen_Mask_AUTO,
        true
    );

    if (!ok) {
        m_display.draw_text_centered(28, "QR gen failed", Display::FontSize::SMALL);
        m_display.draw_text_centered(42, "b: Back",       Display::FontSize::SMALL);
        return;
    }

    int qr_size = qrcodegen_getSize(qr_code);   // number of modules (e.g. 29 for V3)
    int px      = 2;                              // pixels per module
    int total   = qr_size * px;

    // Center the QR code; leave top-4px for the tiny "QR" label
    int x_off = (Display::WIDTH  - total) / 2;
    int y_off = 4 + (Display::HEIGHT - 4 - total) / 2;

    // Draw title
    m_display.draw_text_centered(0, "WALLET QR", Display::FontSize::SMALL);

    // Draw QR modules
    for (int row = 0; row < qr_size; row++) {
        for (int col = 0; col < qr_size; col++) {
            if (qrcodegen_getModule(qr_code, col, row)) {
                m_display.fill_rect(x_off + col * px, y_off + row * px, px, px);
            }
        }
    }
}

void UIManager::render_tx_confirm() {
    m_display.draw_text_centered(2, "CONFIRM TX?", Display::FontSize::SMALL);
    m_display.draw_hline(0, 12, 128);

    char buf[32];
    snprintf(buf, sizeof(buf), "$%.2f", static_cast<double>(m_tx_amount_cents) / 100.0);
    m_display.draw_text_centered(20, buf, Display::FontSize::LARGE);

    m_display.draw_hline(0, 44, 128);
    m_display.draw_text(4,  50, "[C] CONFIRM", Display::FontSize::SMALL);
    m_display.draw_text(72, 50, "[B] REJECT", Display::FontSize::SMALL);
}

void UIManager::render_chat() {
    m_display.draw_text_centered(2, "AI ASSISTANT", Display::FontSize::SMALL);
    m_display.draw_hline(0, 12, 128);

    // Wrap response text at ~20 chars per line
    const std::string& msg = m_last_ai_response;
    size_t pos = 0;
    int y = 18;
    while (pos < msg.size() && y < 58) {
        size_t end = std::min(pos + 20, msg.size());
        // Try to break at space
        if (end < msg.size() && msg[end] != ' ') {
            size_t sp = msg.rfind(' ', end);
            if (sp != std::string::npos && sp > pos) end = sp;
        }
        m_display.draw_text(4, y, msg.substr(pos, end - pos).c_str(), Display::FontSize::SMALL);
        pos = end;
        while (pos < msg.size() && msg[pos] == ' ') ++pos;
        y += 10;
    }
}

// ─── Setup wizard renderer ────────────────────────────────
void UIManager::render_setup() {
    switch (m_setup_stage) {

        case SetupStage::WIFI_PROMPT:
            // Step 1: Ask for WiFi
            m_display.draw_text_centered(0, "-- FUCHEY SETUP --", Display::FontSize::SMALL);
            m_display.draw_hline(0, 10, 128);
            m_display.draw_text_centered(14, "Step 1: WiFi", Display::FontSize::SMALL);
            m_display.draw_text_centered(26, "Open serial monitor", Display::FontSize::SMALL);
            m_display.draw_text_centered(37, "and type:", Display::FontSize::SMALL);
            m_display.draw_text_centered(49, "w SSID PASSWORD", Display::FontSize::SMALL);
            m_display.draw_rect(0, 48, 128, 16);
            break;

        case SetupStage::WIFI_CONNECTING: {
            // Animate connecting dots
            uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (now - m_connecting_dots_ms > 400) {
                m_connecting_dots_ms = now;
                m_connecting_dots = (m_connecting_dots + 1) % 4;
            }
            char dots[5] = {0};
            for (int i = 0; i < m_connecting_dots; ++i) dots[i] = '.';

            m_display.draw_text_centered(0,  "-- CONNECTING --", Display::FontSize::SMALL);
            m_display.draw_hline(0, 10, 128);

            // Truncate SSID to fit
            char ssid_buf[24];
            snprintf(ssid_buf, sizeof(ssid_buf), "%.18s", m_connecting_ssid.c_str());
            m_display.draw_text_centered(16, ssid_buf, Display::FontSize::SMALL);

            char dot_buf[20];
            snprintf(dot_buf, sizeof(dot_buf), "Waiting for IP%s", dots);
            m_display.draw_text_centered(30, dot_buf, Display::FontSize::SMALL);

            // Animated progress bar
            uint8_t pct = static_cast<uint8_t>((m_connecting_dots * 25) % 100);
            m_display.draw_progress_bar(4, 44, 120, 10, pct);
            break;
        }

        case SetupStage::WALLET_PROMPT:
            m_display.draw_text_centered(0,  "-- FUCHEY SETUP --", Display::FontSize::SMALL);
            m_display.draw_hline(0, 10, 128);
            m_display.draw_text(4, 14, "WiFi: OK", Display::FontSize::SMALL);
            m_display.draw_text(4, 24, "Step 2: Wallet", Display::FontSize::SMALL);
            m_display.draw_text(4, 36, "wallet_create", Display::FontSize::SMALL);
            m_display.draw_text(4, 46, "wallet_import", Display::FontSize::SMALL);
            m_display.draw_text(4, 56, "<mnemonic/key>", Display::FontSize::SMALL);
            break;

        case SetupStage::DONE:
        default:
            m_display.draw_text_centered(28, "Setup Complete!", Display::FontSize::SMALL);
            m_display.draw_text_centered(42, "Starting...", Display::FontSize::SMALL);
            break;
    }
}

// ─── FreeRTOS task ────────────────────────────────────────
void UIManager::task_entry(void* arg) {
    static_cast<UIManager*>(arg)->run();
}

void UIManager::run() {
    ESP_LOGI(TAG, "UIManager task running on Core %d", xPortGetCoreID());
    Events::Event evt{};

    while (true) {
        // Process UI queue events (weather, price, AI, TX, WiFi)
        if (Events::g_ui_queue &&
            xQueueReceive(Events::g_ui_queue, &evt, pdMS_TO_TICKS(10)) == pdTRUE) {
            process_event(evt);
        }

        // Process button queue
        ButtonState btn{};
        if (Events::g_button_queue &&
            xQueueReceive(Events::g_button_queue, &btn, pdMS_TO_TICKS(10)) == pdTRUE) {

            ESP_LOGI(TAG, "[BTN] id=%s event=%s",
                     btn.id == ButtonId::CONFIRM ? "CONFIRM" : "BACK",
                     btn.event == ButtonEvent::PRESS        ? "PRESS" :
                     btn.event == ButtonEvent::DOUBLE_PRESS ? "DOUBLE_PRESS" :
                     btn.event == ButtonEvent::LONG_PRESS   ? "LONG_PRESS" : "RELEASE");

            // Reset idle cycle timer on any button activity
            m_last_idle_cycle_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

            // Handle menu navigation & TX confirmation
            if (m_current_screen == UIScreen::TX_CONFIRM) {
                if (btn.event == ButtonEvent::PRESS && btn.id == ButtonId::CONFIRM) {
                    ESP_LOGI(TAG, "[TX] User CONFIRMED transaction ($%.2f)",
                             static_cast<double>(m_tx_amount_cents) / 100.0);
                    extern QueueHandle_t g_tx_confirm_queue;
                    Events::Event tx_evt{};
                    tx_evt.type = Events::EventType::TX_APPROVED;
                    tx_evt.data.tx.amount_cents = m_tx_amount_cents;
                    Events::post(Events::g_wallet_queue, tx_evt);
                    if (g_tx_confirm_queue) Events::post(g_tx_confirm_queue, tx_evt);
                    set_screen(UIScreen::IDLE_CLOCK);
                } else if (btn.event == ButtonEvent::DOUBLE_PRESS || btn.event == ButtonEvent::LONG_PRESS || btn.id == ButtonId::BACK) {
                    ESP_LOGI(TAG, "[TX] User REJECTED transaction");
                    extern QueueHandle_t g_tx_confirm_queue;
                    Events::Event tx_evt{};
                    tx_evt.type = Events::EventType::TX_REJECTED;
                    Events::post(Events::g_wallet_queue, tx_evt);
                    if (g_tx_confirm_queue) Events::post(g_tx_confirm_queue, tx_evt);
                    set_screen(UIScreen::IDLE_CLOCK);
                }
            } else if (m_current_screen == UIScreen::MENU_MAIN) {
                // NEXT option (Double Press or BACK button or 'n' serial command)
                if (btn.event == ButtonEvent::DOUBLE_PRESS || btn.id == ButtonId::BACK) {
                    m_menu_index = (m_menu_index + 1) % 3;
                    ESP_LOGI(TAG, "[Menu] Next option -> index: %d", m_menu_index);
                }
                // SELECT option (CONFIRM button / 'c' / '1' serial command)
                else if (btn.event == ButtonEvent::PRESS && btn.id == ButtonId::CONFIRM) {
                    if (m_menu_index == 0) {
                        ESP_LOGI(TAG, "Screen: WALLET_INFO");
                        set_screen(UIScreen::WALLET_INFO);
                    } else if (m_menu_index == 1) {
                        ESP_LOGI(TAG, "Screen: CHAT_VIEW");
                        set_screen(UIScreen::CHAT_VIEW);
                    } else if (m_menu_index == 2) {
                        ESP_LOGI(TAG, "Screen: IDLE_PRICE");
                        set_screen(UIScreen::IDLE_PRICE);
                    }
                }
            } else if (m_current_screen == UIScreen::WALLET_INFO) {
                if (btn.id == ButtonId::BACK) {
                    ESP_LOGI(TAG, "Screen: WALLET_INFO -> MENU_MAIN");
                    set_screen(UIScreen::MENU_MAIN);
                } else if (btn.event == ButtonEvent::PRESS && btn.id == ButtonId::CONFIRM) {
                    // 'q' / select on WALLET_INFO -> toggle to QR view
                    ESP_LOGI(TAG, "Screen: WALLET_INFO -> WALLET_QR");
                    set_screen(UIScreen::WALLET_QR);
                }
            } else if (m_current_screen == UIScreen::WALLET_QR) {
                // Any key returns to address text view
                if (btn.id == ButtonId::BACK || btn.event == ButtonEvent::PRESS) {
                    ESP_LOGI(TAG, "Screen: WALLET_QR -> WALLET_INFO");
                    set_screen(UIScreen::WALLET_INFO);
                }
            } else if (m_current_screen == UIScreen::CHAT_VIEW) {
                if (btn.id == ButtonId::BACK) {
                    ESP_LOGI(TAG, "Screen: Returning to MENU_MAIN from CHAT_VIEW");
                    set_screen(UIScreen::MENU_MAIN);
                }
            }
        }

        // Auto-timeout from Menu/subscreens back to Idle after 15s of inactivity
        if (m_current_screen == UIScreen::MENU_MAIN ||
            m_current_screen == UIScreen::WALLET_INFO ||
            m_current_screen == UIScreen::WALLET_QR) {
            uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (now - m_last_idle_cycle_ms >= 15000) {
                ESP_LOGI(TAG, "[Menu] Timeout after 15s inactivity -> Returning to Idle Cycle");
                set_screen(UIScreen::IDLE_CLOCK);
            }
        } else if (!m_setup_needed) {
            // Cycle ambient idle screens (Clock -> Weather -> Price -> Message -> Clock)
            cycle_idle_screen();
        }

        render();

        vTaskDelay(pdMS_TO_TICKS(Timing::DISPLAY_UPDATE_MS));
    }
}

} // namespace Fuchey

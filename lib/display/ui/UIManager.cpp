// ============================================================
// Fuchey — UIManager.cpp
// ============================================================

#include "UIManager.hpp"
#include "../../config/Config.hpp"
#include "../../buttons/ButtonDriver.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdio>

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

void UIManager::process_event(const Events::Event& evt) {
    switch (evt.type) {
        case Events::EventType::WEATHER_UPDATED:
            m_weather_temp = evt.data.weather.temp_celsius;
            break;
        case Events::EventType::PRICE_UPDATED:
            m_sol_price = evt.data.price.sol_usd;
            break;
        case Events::EventType::AI_RESPONSE_READY:
            m_last_ai_response = evt.data.chat.text;
            set_screen(UIScreen::CHAT_VIEW);
            break;
        case Events::EventType::TX_REQUEST:
            m_tx_amount_cents = evt.data.tx.amount_cents;
            set_screen(UIScreen::TX_CONFIRM);
            break;
        default:
            break;
    }
}

void UIManager::render() {
    m_display.clear();

    switch (m_current_screen) {
        case UIScreen::IDLE_CLOCK:   render_clock();       break;
        case UIScreen::IDLE_WEATHER: render_weather();     break;
        case UIScreen::IDLE_PRICE:   render_price();       break;
        case UIScreen::IDLE_MESSAGE: render_message();     break;
        case UIScreen::MENU_MAIN:    render_menu();        break;
        case UIScreen::WALLET_INFO:  render_wallet_info();  break;
        case UIScreen::TX_CONFIRM:   render_tx_confirm();  break;
        case UIScreen::CHAT_VIEW:    render_chat();        break;
    }

    m_display.flush();
}

void UIManager::render_clock() {
    m_display.draw_text_centered(10, "FUCHEY WALLET", Display::FontSize::SMALL);
    m_display.draw_text_centered(30, "12:34 PM", Display::FontSize::LARGE);
    m_display.draw_text_centered(54, "Desk Companion", Display::FontSize::SMALL);
}

void UIManager::render_weather() {
    m_display.draw_text_centered(10, "WEATHER", Display::FontSize::SMALL);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f C", m_weather_temp);
    m_display.draw_text_centered(30, buf, Display::FontSize::LARGE);
    m_display.draw_text_centered(54, "New York", Display::FontSize::SMALL);
}

void UIManager::render_price() {
    m_display.draw_text_centered(10, "SOLANA PRICE", Display::FontSize::SMALL);
    char buf[32];
    snprintf(buf, sizeof(buf), "$%.2f", m_sol_price);
    m_display.draw_text_centered(30, buf, Display::FontSize::LARGE);
    m_display.draw_text_centered(54, "CoinGecko", Display::FontSize::SMALL);
}

void UIManager::render_message() {
    m_display.draw_text_centered(10, "FUCHEY SAYS", Display::FontSize::SMALL);
    m_display.draw_text_centered(30, "Have a great day!", Display::FontSize::SMALL);
    m_display.draw_text_centered(50, "Press button for menu", Display::FontSize::SMALL);
}

void UIManager::render_menu() {
    m_display.draw_text(10, 5, "MAIN MENU", Display::FontSize::SMALL);
    m_display.draw_text(10, 25, "> 1. Wallet Info", Display::FontSize::SMALL);
    m_display.draw_text(10, 40, "  2. AI Assistant", Display::FontSize::SMALL);
}

void UIManager::render_wallet_info() {
    m_display.draw_text_centered(5, "WALLET ACTIVE", Display::FontSize::SMALL);
    m_display.draw_text_centered(25, "Solana Address:", Display::FontSize::SMALL);
    m_display.draw_text_centered(45, "7xKX...3b9Z", Display::FontSize::SMALL);
}

void UIManager::render_tx_confirm() {
    m_display.draw_text_centered(2, "CONFIRM TRANSACTION", Display::FontSize::SMALL);
    char buf[32];
    snprintf(buf, sizeof(buf), "Amount: $%.2f", static_cast<double>(m_tx_amount_cents)/100.0);
    m_display.draw_text_centered(22, buf, Display::FontSize::SMALL);

    m_display.draw_text_centered(42, "[BTN1] CONFIRM", Display::FontSize::SMALL);
    m_display.draw_text_centered(54, "[BTN2] REJECT", Display::FontSize::SMALL);
}

void UIManager::render_chat() {
    m_display.draw_text(5, 5, "AI ASSISTANT:", Display::FontSize::SMALL);
    m_display.draw_text(5, 25, m_last_ai_response.c_str(), Display::FontSize::SMALL);
}

void UIManager::task_entry(void* arg) {
    static_cast<UIManager*>(arg)->run();
}

void UIManager::run() {
    ESP_LOGI(TAG, "UIManager task running");
    Events::Event evt{};

    while (true) {
        if (Events::g_ui_queue &&
            xQueueReceive(Events::g_ui_queue, &evt, pdMS_TO_TICKS(50)) == pdTRUE) {
            process_event(evt);
        }

        ButtonState btn{};
        if (Events::g_button_queue &&
            xQueueReceive(Events::g_button_queue, &btn, pdMS_TO_TICKS(50)) == pdTRUE) {
            ESP_LOGI(TAG, "Button event received: id=%d event=%d", static_cast<int>(btn.id), static_cast<int>(btn.event));
            if (btn.id == ButtonId::CONFIRM && btn.event != ButtonEvent::LONG_PRESS) {
                switch (m_current_screen) {
                    case UIScreen::IDLE_CLOCK:
                    case UIScreen::IDLE_WEATHER:
                    case UIScreen::IDLE_PRICE:
                    case UIScreen::IDLE_MESSAGE:
                        set_screen(UIScreen::MENU_MAIN);
                        break;
                    case UIScreen::MENU_MAIN:
                        set_screen(UIScreen::WALLET_INFO);
                        break;
                    case UIScreen::WALLET_INFO:
                        set_screen(UIScreen::CHAT_VIEW);
                        break;
                    case UIScreen::CHAT_VIEW:
                    default:
                        set_screen(UIScreen::IDLE_CLOCK);
                        break;
                }
            } else if (btn.id == ButtonId::BACK || btn.event == ButtonEvent::LONG_PRESS) {
                set_screen(UIScreen::IDLE_CLOCK);
            }
        }

        cycle_idle_screen();
        render();
    }
}

} // namespace Fuchey

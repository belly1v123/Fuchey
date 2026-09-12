// ============================================================
// Fuchey — UIManager.cpp
// UI state machine, OLED rendering, and setup wizard.
// ============================================================

#include "UIManager.hpp"
#include "Animations.hpp"
#include "SpritePlayer.hpp"
#include "YetiAnim.hpp"
#include "FairPass.hpp"
#include "PassDesign.hpp"
#include "PassBlurTop.hpp"
#include "WeatherIcons.hpp"
#include "WalletInfoIcon.hpp"
#include "ViewBalanceIcon.hpp"
#include "QrIcon.hpp"
#include "SolPriceIcon.hpp"
#include "SolanaPixelArt.hpp"
#include "PomodoroIcon.hpp"
#include "BadgeIcon.hpp"
#include "BalanceSolIcon.hpp"
#include "BalanceUsdcIcon.hpp"
#include "FreeSans9pt7b.h"
#include "FreeMonoBold12pt7b.h"
#include "esp_heap_caps.h"
#include "FreeSansBold9pt7b.h"
#include "FreeSerifBold9pt7b.h"
#include "PoppinsRegular9pt7b.h"
#include "PoppinsBold9pt7b.h"
#include "../../config/Config.hpp"
#include "../../buttons/ButtonDriver.hpp"
#include "../../led_indicator/LedIndicator.hpp"
#include "../../price/PriceService.hpp"
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

// Extended palette (RGB565) for a richer TFT look — the built-in set only
// has white/black/red/green/yellow/blue.
inline constexpr Color TFT_CYAN   = 0x07FF;
inline constexpr Color TFT_GRAY   = 0x8410;
inline constexpr Color TFT_ORANGE = 0xFD20;
inline constexpr Color TFT_SILVER = 0xC618; // light gray body text
inline constexpr Color TFT_NAVY   = 0x10A2; // dark command-pill fill

namespace {
// ─── Setup-screen GFX helpers (Poppins mix, all size 1) ────
void gfx_centered(Display& d, int y, std::string_view t,
                  const GFXfont* f, Color c) {
    int w = 0, h = 0;
    d.gfx_text_bounds(t, f, 1, &w, &h);
    d.draw_gfx_text((Display::WIDTH - w) / 2, y, t, f, 1, c);
}

// Centered command pill: cyan-on-navy by default, auto-measured + padded.
void command_pill(Display& d, int y, std::string_view t,
                  const GFXfont* f, Color fg, Color bg = TFT_NAVY) {
    int w = 0, h = 0;
    d.gfx_text_bounds(t, f, 1, &w, &h);
    constexpr int kPadX = 8, kPadTop = 5, kPadBot = 5;
    const int px = (Display::WIDTH - w) / 2 - kPadX;
    d.fill_rect(px, y - kPadTop, w + 2 * kPadX, h + kPadTop + kPadBot, bg);
    d.draw_gfx_text((Display::WIDTH - w) / 2, y, t, f, 1, fg);
}

// Trim text with "..." until it fits max_w (for user data like SSIDs).
std::string fit_gfx(Display& d, std::string_view t,
                    const GFXfont* f, int max_w) {
    int w = 0, h = 0;
    d.gfx_text_bounds(t, f, 1, &w, &h);
    if (w <= max_w) return std::string(t);
    std::string head(t);
    while (!head.empty()) {
        head.pop_back();
        std::string cand = head + "...";
        d.gfx_text_bounds(cand, f, 1, &w, &h);
        if (w <= max_w) return cand;
    }
    return "...";
}

void setup_header(Display& d) {
    gfx_centered(d, 8, "First Boot Setup", &PoppinsBold9pt7b, Colors::WHITE);
    d.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
}

// Open-Meteo weathercode -> home icon (30x32 RGB565, null = no data yet).
const SpritePixel* home_weather_bits(uint8_t code) {
    switch (code) {
        case 0:
        case 1:  return image_weather_sunny_bits;       // clear / mainly clear
        case 2:  return image_weather_cloud_sunny_bits; // partly cloudy
        case 3:
        case 45:
        case 48: return image_weather_cloud_bits;       // overcast / fog
        default:
            if (code > 99) return nullptr;              // 255 = unknown
            return image_weather_rain_bits;             // drizzle / rain / snow / storm
    }
}
} // namespace

UIManager::UIManager(Display& display) : m_display(display) {}

void UIManager::set_price_service(PriceService* ps) {
    m_price_service = ps;
    if (m_price_service) {
        // Non-blocking sync of the latest cached market data (no HTTP here;
        // PriceService fetches on its own task). Until the first successful
        // fetch, keep the -1 sentinel so the screen shows placeholders.
        m_sol_price = m_price_service->has_data() ? m_price_service->get_sol_usd() : -1.0f;
        m_sol_high_24h = m_price_service->get_high_24h();
        m_sol_low_24h = m_price_service->get_low_24h();
        m_sol_change_pct = m_price_service->get_change_pct_24h();
    }
}

bool UIManager::init() {
    m_last_idle_cycle_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "UIManager initialized");
    return true;
}

void UIManager::set_screen(UIScreen screen) {
    m_current_screen = screen;
    if (screen == UIScreen::MENU_MAIN) {
        m_menu_anim_last_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        // No in-flight slide when (re-)entering the menu.
        m_menu_prev_index = -1;
    }
    if (screen == UIScreen::POMODORO_VIEW) {
        // Fresh setup session on every entry; countdown/buzzer state resets.
        m_pomo.reset();
        m_pomo_holding = false;
        m_pomo_last_sec = UINT32_MAX;
    }
    if (screen == UIScreen::ANIM_TEST) {
        // Restart animation from frame 0 on every entry.
        m_anim_started = false;
        m_anim_chrome_drawn = false;
        m_anim_last_frame = 255;
    }
    if (screen == UIScreen::HOME) {
        m_home_started = false;
        m_home_chrome = false;
        m_home_last_frame = 255;
        m_home_last_minute = -2;
        m_home_tx = m_home_ty = m_home_tw = m_home_th = 0;
    }
    if (screen == UIScreen::IDLE_PRICE && m_price_service) {
        // Sync cached market data on entry (no blocking HTTP on UI task).
        m_sol_price = m_price_service->has_data() ? m_price_service->get_sol_usd() : -1.0f;
        m_sol_high_24h = m_price_service->get_high_24h();
        m_sol_low_24h = m_price_service->get_low_24h();
        m_sol_change_pct = m_price_service->get_change_pct_24h();
        // Silently refresh in the background so the screen populates by
        // itself (no serial `p` needed). Non-blocking: only wakes the price
        // task. Throttled to one request per 15s against rapid re-entry.
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (m_price_req_last_ms == 0 || now_ms - m_price_req_last_ms >= 15000) {
            m_price_req_last_ms = now_ms;
            m_price_service->request_update();
        }
    }
    request_redraw();
}

// ─── Idle screen cycling ──────────────────────────────────
void UIManager::cycle_idle_screen() {
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now - m_last_idle_cycle_ms >= Timing::IDLE_SCREEN_CYCLE_MS) {
        m_last_idle_cycle_ms = now;
        switch (m_current_screen) {
            case UIScreen::HOME:         break; // home is the only idle screen
            case UIScreen::IDLE_WEATHER:
            case UIScreen::IDLE_PRICE:
            case UIScreen::IDLE_MESSAGE: set_screen(UIScreen::HOME); break; // retired; park on HOME
            default: break; // Stay on active wallet/menu screens
        }
        // Note: set_screen() (not direct assignment) so per-screen entry
        // state resets — e.g. HOME must fully repaint its background chrome
        // instead of resuming incremental window flushes over a stale frame.
        request_redraw();
    }
}

// ─── Event processing ─────────────────────────────────────
void UIManager::process_event(const Events::Event& evt) {
    request_redraw();
    switch (evt.type) {
        case Events::EventType::WIFI_GOT_IP:
            ESP_LOGI(TAG, "WIFI_GOT_IP received — advancing setup stage");
            on_wifi_got_ip();
            break;

        case Events::EventType::WEATHER_UPDATED:
            m_weather_temp = evt.data.weather.temp_celsius;
            m_weather_code = evt.data.weather.weather_code;
            m_weather_city = evt.data.weather.city;
            ESP_LOGI(TAG, "Weather updated: %.1f C code %u in %s",
                     m_weather_temp, m_weather_code, m_weather_city.c_str());
            break;

        case Events::EventType::PRICE_UPDATED:
            m_sol_price = evt.data.price.sol_usd;
            m_sol_high_24h = evt.data.price.high_24h;
            m_sol_low_24h = evt.data.price.low_24h;
            m_sol_change_pct = evt.data.price.change_pct_24h;
            ESP_LOGI(TAG, "SOL price updated: $%.2f hi=%.2f lo=%.2f chg=%+.2f%%",
                     m_sol_price, m_sol_high_24h, m_sol_low_24h, m_sol_change_pct);
            if (m_current_screen == UIScreen::IDLE_PRICE) {
                // Fresh data just landed while the user is reading it —
                // restart the hub-timeout window so the screen isn't yanked
                // away mid-read (console `p` doesn't count as button activity).
                m_last_idle_cycle_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            }
            break;

        case Events::EventType::BALANCE_UPDATED:
            m_bal_sol = evt.data.balance.sol;
            m_bal_usdc = evt.data.balance.usdc;
            m_bal_ok = evt.data.balance.ok;
            m_bal_fetched = true;
            m_bal_fetching = false;
            ESP_LOGI(TAG, "Balance updated: %.4f SOL ($%.2f USDC) ok=%d",
                     m_bal_sol, m_bal_usdc, m_bal_ok);
            break;

        case Events::EventType::AI_RESPONSE_READY:
            m_last_ai_response = evt.data.chat.text;
            set_screen(UIScreen::CHAT_VIEW);
            break;

        case Events::EventType::TX_REQUEST:
            m_tx_amount_cents = evt.data.tx.amount_cents;
            m_tx_pending_accept = false;
            ESP_LOGI(TAG, "TX request received: $%.2f — showing confirmation",
                     static_cast<double>(m_tx_amount_cents) / 100.0);
            set_screen(UIScreen::TX_CONFIRM);
            break;

        case Events::EventType::TX_BROADCAST_OK:
        case Events::EventType::TX_BROADCAST_FAIL: {
            m_tx_result_ok = (evt.type == Events::EventType::TX_BROADCAST_OK);
            m_tx_result_amount_cents = evt.data.tx.amount_cents;

            // Parse tx_data: "ASSET:amount_str:recipient" or "error message|recipient" or "error message"
            const char* data = reinterpret_cast<const char*>(evt.data.tx.tx_data);
            const char* bar = strchr(data, '|');
            const char* colon1 = strchr(data, ':');
            const char* colon2 = colon1 ? strchr(colon1 + 1, ':') : nullptr;

            m_tx_result_asset[0] = '\0';
            m_tx_result_recipient[0] = '\0';
            m_tx_result_msg[0] = '\0';

            if (bar) {
                // "error message|recipient" — split on '|'
                size_t msg_len = std::min<size_t>(bar - data, sizeof(m_tx_result_msg) - 1);
                memcpy(m_tx_result_msg, data, msg_len);
                m_tx_result_msg[msg_len] = '\0';

                const char* rec = bar + 1;
                size_t rec_len = std::min<size_t>(strlen(rec), sizeof(m_tx_result_recipient) - 1);
                memcpy(m_tx_result_recipient, rec, rec_len);
                m_tx_result_recipient[rec_len] = '\0';

                // Truncate recipient for display
                size_t rlen = strlen(m_tx_result_recipient);
                if (rlen > 12) {
                    memmove(m_tx_result_recipient + 4, m_tx_result_recipient + rlen - 4, 5);
                    memcpy(m_tx_result_recipient + 4, "...", 3);
                }
            } else if (colon1 && colon2) {
                size_t asset_len = std::min<size_t>(colon1 - data, sizeof(m_tx_result_asset) - 1);
                memcpy(m_tx_result_asset, data, asset_len);
                m_tx_result_asset[asset_len] = '\0';

                const char* rec = colon2 + 1;
                size_t rec_len = std::min<size_t>(strlen(rec), sizeof(m_tx_result_recipient) - 1);
                memcpy(m_tx_result_recipient, rec, rec_len);
                m_tx_result_recipient[rec_len] = '\0';

                // Truncate recipient for display
                size_t rlen = strlen(m_tx_result_recipient);
                if (rlen > 12) {
                    memmove(m_tx_result_recipient + 4, m_tx_result_recipient + rlen - 4, 5);
                    memcpy(m_tx_result_recipient + 4, "...", 3);
                }
            } else {
                // Error message in tx_data
                size_t msg_len = std::min<size_t>(strlen(data), sizeof(m_tx_result_msg) - 1);
                memcpy(m_tx_result_msg, data, msg_len);
                m_tx_result_msg[msg_len] = '\0';
            }

            m_tx_result_start_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            ESP_LOGI(TAG, "TX broadcast %s — showing result screen",
                     m_tx_result_ok ? "OK" : "FAIL");
            set_screen(m_tx_result_ok ? UIScreen::TX_SUCCESS : UIScreen::TX_FAIL);

            if (m_led_indicator) {
                if (m_tx_result_ok) {
                    m_led_indicator->blink_success();
                } else {
                    m_led_indicator->blink_failure();
                }
            }
            break;
        }

        default:
            break;
    }
}

// ─── TX approve / reject ──────────────────────────────────
void UIManager::approve_transaction() {
    m_tx_pending_accept = false;
    ESP_LOGI(TAG, "[TX] User CONFIRMED transaction ($%.2f)",
             static_cast<double>(m_tx_amount_cents) / 100.0);
    extern QueueHandle_t g_tx_confirm_queue;
    Events::Event tx_evt{};
    tx_evt.type = Events::EventType::TX_APPROVED;
    tx_evt.data.tx.amount_cents = m_tx_amount_cents;
    Events::post(Events::g_wallet_queue, tx_evt);
    if (g_tx_confirm_queue) Events::post(g_tx_confirm_queue, tx_evt);
    set_screen(UIScreen::HOME);
}

void UIManager::reject_transaction() {
    m_tx_pending_accept = false;
    ESP_LOGI(TAG, "[TX] User REJECTED transaction (double/long press)");
    extern QueueHandle_t g_tx_confirm_queue;
    Events::Event tx_evt{};
    tx_evt.type = Events::EventType::TX_REJECTED;
    Events::post(Events::g_wallet_queue, tx_evt);
    if (g_tx_confirm_queue) Events::post(g_tx_confirm_queue, tx_evt);
    set_screen(UIScreen::HOME);
}

// ─── Hierarchical Back (B1 outside TX_CONFIRM) ─────────────
// WALLET_QR / BALANCE_VIEW / IDLE_PRICE (wallet subs) -> WALLET_INFO hub
// (one level); hub and all other sub-screens -> MENU_MAIN;
// MENU_MAIN -> HOME. Idle/TX screens: no-op.
void UIManager::go_back() {
    switch (m_current_screen) {
        case UIScreen::WALLET_QR:
        case UIScreen::BALANCE_VIEW:
        case UIScreen::IDLE_PRICE:
            ESP_LOGI(TAG, "Screen: wallet sub -> WALLET_INFO hub");
            set_screen(UIScreen::WALLET_INFO);
            break;
        case UIScreen::WALLET_INFO:
        case UIScreen::CHAT_VIEW:
        case UIScreen::POMODORO_VIEW:
        case UIScreen::BADGE_VIEW:
        case UIScreen::FAIR_PASS:
        case UIScreen::ANIM_TEST:
            ESP_LOGI(TAG, "Screen: sub -> MENU_MAIN");
            set_screen(UIScreen::MENU_MAIN);
            break;
        case UIScreen::MENU_MAIN:
            ESP_LOGI(TAG, "Screen: MENU_MAIN -> HOME");
            set_screen(UIScreen::HOME);
            break;
        case UIScreen::TX_SUCCESS:
        case UIScreen::TX_FAIL:
            ESP_LOGI(TAG, "Screen: TX result -> HOME");
            set_screen(UIScreen::HOME);
            break;
        default:
            break;
    }
}

// ─── Menu helpers ──────────────────────────────────────────
// Main menu (horizontal icon carousel, 3 entries):
//   0 = Wallet Info (hub)  1 = Pomodoro  2 = Badge (opens the pass)
// View Balance and QR live under the Wallet Info hub, not top-level.
void UIManager::open_menu_index(uint8_t index) {
    m_menu_index = index % 3;
    m_menu_prev_index = -1; // direct open: no slide animation
    if (m_menu_index == 0) {
        ESP_LOGI(TAG, "Screen: WALLET_INFO hub");
        m_wallet_tab = 0;
        set_screen(UIScreen::WALLET_INFO);
    } else if (m_menu_index == 1) {
        ESP_LOGI(TAG, "Screen: POMODORO_VIEW");
        set_screen(UIScreen::POMODORO_VIEW);
    } else {
        ESP_LOGI(TAG, "Screen: FAIR_PASS (via Badge)");
        set_screen(UIScreen::FAIR_PASS);
    }
}

// Wallet Info hub sub-tabs: 0 = View Balance, 1 = Receive QR, 2 = SOL Price.
void UIManager::open_wallet_tab(uint8_t tab) {
    m_wallet_tab = tab % 3;
    if (m_wallet_tab == 0) {
        ESP_LOGI(TAG, "Screen: WALLET_INFO hub -> BALANCE_VIEW");
        m_bal_fetched = false;
        m_bal_ok = false;
        m_bal_fetching = false;
        m_bal_fetch_start_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        set_screen(UIScreen::BALANCE_VIEW);
    } else if (m_wallet_tab == 1) {
        ESP_LOGI(TAG, "Screen: WALLET_INFO hub -> WALLET_QR");
        set_screen(UIScreen::WALLET_QR);
    } else {
        ESP_LOGI(TAG, "Screen: WALLET_INFO hub -> IDLE_PRICE");
        set_screen(UIScreen::IDLE_PRICE);
    }
}

void UIManager::step_wallet_tab(int8_t dir) {
    // Hub-only: flip the focused tab and stay in the hub. Content screens
    // (Balance / QR / Price) deliberately do NOT flip — go back first.
    if (m_current_screen != UIScreen::WALLET_INFO) return;
    static constexpr const char* kNames[3] = {"Balance", "QR", "SOL Price"};
    m_wallet_tab = static_cast<uint8_t>((m_wallet_tab + dir + 3) % 3);
    ESP_LOGI(TAG, "[Wallet] hub tab -> %s", kNames[m_wallet_tab]);
    request_redraw();
}

void UIManager::step_menu(int8_t dir) {
    int idx = static_cast<int>(m_menu_index);
    idx = (idx + dir + 3) % 3;
    ESP_LOGI(TAG, "[Menu] %s -> index: %d", dir > 0 ? "Next" : "Prev", idx);
    if (m_current_screen == UIScreen::MENU_MAIN) {
        m_menu_index = static_cast<uint8_t>(idx);
        m_menu_prev_index = -1; // carousel is static; no slide animation
    } else {
        // In a sub-screen: carousel-jump directly to prev/next destination.
        open_menu_index(static_cast<uint8_t>(idx));
    }
}

// ─── Pomodoro input + tick ─────────────────────────────────
// Button contract: time phases: B3 (left) = +sec, B4 (right) = +min;
// SetLoops: B4 = +1 loop, B3 = -1 loop; B2 = confirm, B1 = back.
// B3/B4 auto-repeat while held (setup phases only).
void UIManager::handle_pomodoro_button(const ButtonState& btn) {
    using Phase = PomodoroTimer::Phase;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const Phase phase = m_pomo.phase();

    if (btn.id == ButtonId::B3_PREV || btn.id == ButtonId::B4_NEXT) {
        const bool is_right = (btn.id == ButtonId::B4_NEXT);
        if (btn.event == ButtonEvent::PRESS) {
            if (phase == Phase::SetWork || phase == Phase::SetBreak) {
                if (is_right) m_pomo.adjust_min(); else m_pomo.adjust_sec();
            } else if (phase == Phase::SetLoops) {
                m_pomo.adjust_loops(is_right ? +1 : -1);
            } else {
                return; // Ready/Done/running/paused ignore B3/B4
            }
            // Arm hold-to-repeat; RELEASE disarms (LONG_PRESS ignored so the
            // repeat keeps running while still held).
            m_pomo_holding = true;
            m_pomo_hold_id = btn.id;
            m_pomo_hold_start_ms = now;
            m_pomo_next_repeat_ms = now + Pomodoro::HOLD_REPEAT_START_MS;
            m_last_idle_cycle_ms = now;
            request_redraw();
        } else if (btn.event == ButtonEvent::RELEASE) {
            if (m_pomo_holding && m_pomo_hold_id == btn.id) m_pomo_holding = false;
        }
        return;
    }

    if (btn.id == ButtonId::B2_MENU_SELECT) {
        if (btn.event != ButtonEvent::PRESS) return;
        m_last_idle_cycle_ms = now;
        m_pomo_holding = false;
        switch (phase) {
            case Phase::SetWork:
            case Phase::SetBreak:
            case Phase::SetLoops:
                m_pomo.confirm();
                break;
            case Phase::Ready:
                if (m_pomo.start(now)) {
                    m_pomo_last_sec = UINT32_MAX;
                    if (m_buzzer) m_buzz.start(now, *m_buzzer, 1, 120, 0); // start blip
                }
                break;
            case Phase::RunWork:
            case Phase::RunBreak:
                m_pomo.pause(now);
                break;
            case Phase::Paused:
                m_pomo.resume(now);
                m_pomo_last_sec = UINT32_MAX;
                break;
            case Phase::Done:
                m_pomo.cancel(); // Done -> Ready (settings kept)
                break;
        }
        request_redraw();
        return;
    }

    if (btn.id == ButtonId::B1_TX_BACK) {
        if (btn.event != ButtonEvent::PRESS) return;
        m_last_idle_cycle_ms = now;
        m_pomo_holding = false;
        if (phase == Phase::RunWork || phase == Phase::RunBreak ||
            phase == Phase::Paused) {
            m_pomo.cancel(); // stop run, keep settings
            if (m_buzzer) m_buzz.stop(*m_buzzer);
        } else if (phase == Phase::Done) {
            go_back(); // Done -> MENU_MAIN
        } else if (!m_pomo.back()) {
            go_back(); // SetWork -> MENU_MAIN
        }
        request_redraw();
        return;
    }
}

// pomo_tick(): countdown + hold-repeat + per-second redraw.
// Called from run() while POMODORO_VIEW is open. Never blocks.
void UIManager::pomo_tick(uint32_t now_ms) {
    using Phase = PomodoroTimer::Phase;
    if (m_pomo_holding && m_pomo.is_setup()) {
        if (static_cast<int32_t>(now_ms - m_pomo_next_repeat_ms) >= 0) {
            const bool is_right = (m_pomo_hold_id == ButtonId::B4_NEXT);
            const auto phase = m_pomo.phase();
            if (phase == Phase::SetLoops) {
                m_pomo.adjust_loops(is_right ? +1 : -1);
            } else if (phase == Phase::SetWork || phase == Phase::SetBreak) {
                if (is_right) m_pomo.adjust_min(); else m_pomo.adjust_sec();
            } else {
                m_pomo_holding = false; // Ready: nothing to repeat
            }
            m_pomo_next_repeat_ms = now_ms + Pomodoro::HOLD_REPEAT_RATE_MS;
            m_last_idle_cycle_ms = now_ms;
            request_redraw();
        }
    }
    if (m_pomo.is_running()) {
        auto ev = m_pomo.tick(now_ms);
        if (ev != PomodoroTimer::TickEvent::None && m_buzzer) {
            m_buzz.start(now_ms, *m_buzzer,
                         Pomodoro::ALERT_BEEPS, Pomodoro::ALERT_ON_MS, Pomodoro::ALERT_OFF_MS);
        }
    }
    const uint32_t sec = now_ms / 1000;
    if (sec != m_pomo_last_sec) {
        m_pomo_last_sec = sec;
        request_redraw(); // countdown digit + colon blink advance
    }
}

// ─── Setup wizard ─────────────────────────────────────────
void UIManager::set_setup_needed(bool wifi_missing, bool wallet_missing) {
    m_setup_needed = wifi_missing || wallet_missing;
    request_redraw();

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
        request_redraw();
    }
}

void UIManager::on_wifi_got_ip() {
    if (m_setup_stage == SetupStage::WIFI_CONNECTING) {
        m_setup_stage = SetupStage::WALLET_PROMPT;
        request_redraw();
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
    set_screen(UIScreen::HOME);
}

// ─── Render dispatch ──────────────────────────────────────
void UIManager::render() {
    // ANIM_TEST and HOME are self-flushing (static chrome via full flush
    // once, then sprite/counter window flushes per changed frame). Keep them
    // out of the clear()+full-flush path below.
    if (!m_setup_needed && m_current_screen == UIScreen::ANIM_TEST) {
        render_anim_test();
        return;
    }
    if (!m_setup_needed && m_current_screen == UIScreen::HOME) {
        render_home();
        return;
    }

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
        case UIScreen::TX_SUCCESS:
        case UIScreen::TX_FAIL:      render_tx_result();   break;
        case UIScreen::CHAT_VIEW:    render_chat();        break;
        case UIScreen::BALANCE_VIEW: render_balance();     break;
        case UIScreen::POMODORO_VIEW: render_pomodoro();   break;
        case UIScreen::BADGE_VIEW:    render_badge();      break;
        case UIScreen::ANIM_TEST:    break; // handled by early-return above (self-flushing)
        case UIScreen::HOME:         break; // handled by early-return above (self-flushing)
        case UIScreen::FAIR_PASS:    render_fair_pass();   break;
    }

    m_display.flush();
}

// ─── Idle screens ─────────────────────────────────────────
// Layout system (240x240): MEDIUM header + divider at y=30, SMALL gray
// footer hints above y=218, hero values in LARGE centered in between.
void UIManager::render_clock() {
    m_display.draw_text_centered(8, "FUCHEY", Display::FontSize::MEDIUM);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);

    char buf[16];
    time_t now = time(nullptr);
    struct tm timeinfo;
    bool synced = now > 100000 && localtime_r(&now, &timeinfo);
    if (synced) {
        strftime(buf, sizeof(buf), "%I:%M %p", &timeinfo);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    // "08:34 PM" = 8 chars x 18px = 144px — fits centered
    m_display.draw_text_centered(86, buf, Display::FontSize::LARGE);

    char date_buf[20];
    if (synced) {
        struct tm ti;
        localtime_r(&now, &ti);
        strftime(date_buf, sizeof(date_buf), "%a %b %d", &ti);
    } else {
        snprintf(date_buf, sizeof(date_buf), "Syncing time...");
    }
    m_display.draw_text_centered(134, date_buf, Display::FontSize::MEDIUM, TFT_CYAN);
    m_display.draw_text_centered(200, "solana hardware wallet", Display::FontSize::SMALL, TFT_GRAY);
}

void UIManager::render_weather() {
    m_display.draw_text_centered(8, "WEATHER", Display::FontSize::MEDIUM);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);

    char buf[32];
    if (m_weather_temp < -100.0f) {
        snprintf(buf, sizeof(buf), "--.- C");
    } else {
        snprintf(buf, sizeof(buf), "%.1f C", m_weather_temp);
    }
    m_display.draw_text_centered(82, buf, Display::FontSize::LARGE, TFT_CYAN);
    m_display.draw_text_centered(136, m_weather_city.c_str(), Display::FontSize::MEDIUM);
    m_display.draw_text_centered(200, "open-meteo", Display::FontSize::SMALL, TFT_GRAY);
}

void UIManager::render_price() {
    // Lopaka SOL-price layout on the Fuchey dark theme. Positions match the
    // reference (5,6 icon; values at size 2, labels at size 1); colors are
    // remapped: labels SILVER, values WHITE, up GREEN / down RED.
    static constexpr Color kTransparent = 0xF81F;

    // Solana pixel-art icon (60x60 at 5,0).
    m_display.draw_sprite_transparent(5, 0, 60, 60, SolanaPixelArt_data, kTransparent);

    const bool up = (m_sol_change_pct >= 0.0f);
    const Color trend = up ? Colors::GREEN : Colors::RED;

    char price_buf[24], high_buf[24], low_buf[24], chg_buf[16];
    if (m_sol_price < 0.0f) {
        snprintf(price_buf, sizeof(price_buf), "$--.--");
    } else {
        snprintf(price_buf, sizeof(price_buf), "$ %.2f", static_cast<double>(m_sol_price));
    }
    if (m_sol_high_24h < 0.0f) {
        snprintf(high_buf, sizeof(high_buf), "$--.--");
    } else {
        snprintf(high_buf, sizeof(high_buf), "$ %.2f", static_cast<double>(m_sol_high_24h));
    }
    if (m_sol_low_24h < 0.0f) {
        snprintf(low_buf, sizeof(low_buf), "$--.--");
    } else {
        snprintf(low_buf, sizeof(low_buf), "$ %.2f", static_cast<double>(m_sol_low_24h));
    }
    if (m_sol_price < 0.0f && m_sol_high_24h < 0.0f) {
        snprintf(chg_buf, sizeof(chg_buf), "--.--%%");
    } else {
        snprintf(chg_buf, sizeof(chg_buf), "%+.2f%%", static_cast<double>(m_sol_change_pct));
    }

    // Labels (FreeSans regular, size 1) + "24h Change" in FreeSerif Bold.
    // Values in FreeSans Bold size 2; the % value uses FreeSans regular
    // size 2 exactly like the Lopaka reference.
    m_display.draw_gfx_text(76, 7, "Current Price", &FreeSans9pt7b, 1, TFT_SILVER);
    m_display.draw_gfx_text(12, 68, "24h High", &FreeSans9pt7b, 1, TFT_SILVER);
    m_display.draw_gfx_text(13, 124, "24h Low", &FreeSans9pt7b, 1, TFT_SILVER);
    m_display.draw_gfx_text(14, 185, "24h Change", &FreeSerifBold9pt7b, 1, TFT_SILVER);

    // Values (FreeSans Bold, size 2).
    m_display.draw_gfx_text(76, 25, price_buf, &FreeSansBold9pt7b, 2, Colors::WHITE);
    m_display.draw_gfx_text(11, 86, high_buf, &FreeSansBold9pt7b, 2, Colors::WHITE);
    m_display.draw_gfx_text(12, 142, low_buf, &FreeSansBold9pt7b, 2, Colors::WHITE);
    const bool chg_known = !(m_sol_price < 0.0f && m_sol_high_24h < 0.0f);
    m_display.draw_gfx_text(14, 203, chg_buf, &FreeSans9pt7b, 2,
                            chg_known ? trend : TFT_SILVER);

    // Current-price trend triangle: green ▲ when up, inverted red ▼ on drop.
    if (up) {
        m_display.fill_triangle(195, 8, 201, 19, 189, 19, Colors::GREEN);
    } else {
        m_display.fill_triangle(189, 8, 201, 8, 195, 19, Colors::RED);
    }
    // 24h high marker: fixed green ▲.
    m_display.fill_triangle(96, 69, 102, 81, 90, 81, Colors::GREEN);
    // 24h low marker: fixed inverted red ▼.
    m_display.fill_triangle(90, 125, 102, 125, 96, 137, Colors::RED);
}

void UIManager::render_message() {
    m_display.draw_text_centered(8, "FUCHEY", Display::FontSize::MEDIUM);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
    m_display.draw_text_centered(78, "Have a great day!", Display::FontSize::MEDIUM);
    m_display.draw_text_centered(112, "Solana hardware", Display::FontSize::MEDIUM, TFT_CYAN);
    m_display.draw_text_centered(146, "wallet + AI desk", Display::FontSize::MEDIUM, TFT_CYAN);
}

// ─── Menu / Wallet / Chat screens ─────────────────────────
// Main menu: horizontal icon carousel. One 96x96 icon is focused (centered,
// name below it); B4/next slides the next icon in from the right, B3/prev
// from the left. Entries without a PNG asset get a navy monogram tile.
void UIManager::render_menu() {
    static constexpr Color kTransparent = 0xF81F;
    static constexpr int kIcon = 96;
    static constexpr int kIconY = 52;
    static constexpr int kFocusX = (Display::WIDTH - kIcon) / 2; // 72;

    struct MenuEntry {
        const char* label;
        const SpritePixel* icon; // nullptr -> monogram tile
        const char* mono;        // tile text when icon == nullptr
    };
    static const MenuEntry kItems[3] = {
        {"Wallet Info",  WalletInfoIcon_data, nullptr},
        {"Pomodoro",     PomodoroIcon_data,   nullptr},
        {"Badge",        BadgeIcon_data,      nullptr},
    };

    auto draw_entry = [&](int x, uint8_t idx) {
        const MenuEntry& e = kItems[idx % 3];
        if (e.icon != nullptr) {
            m_display.draw_sprite_transparent(x, kIconY, kIcon, kIcon, e.icon, kTransparent);
        } else {
            m_display.fill_rect(x, kIconY, kIcon, kIcon, TFT_NAVY);
            m_display.draw_rect(x, kIconY, kIcon, kIcon, TFT_GRAY);
            const int mlen = static_cast<int>(strlen(e.mono));
            m_display.draw_text(x + (kIcon - mlen * 12) / 2, kIconY + 40, e.mono,
                                Display::FontSize::MEDIUM, TFT_CYAN);
        }
        // Name below the icon, clipped to the visible strip.
        const int cx = x + kIcon / 2;
        const int half = 110;
        const int x0 = std::max(0, cx - half);
        const int x1 = std::min(Display::WIDTH, cx + half);
        if (x1 > x0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s", e.label);
            // Center manually so off-screen entries slide out cleanly.
            const int len = static_cast<int>(strlen(buf));
            const int tx = cx - (len * 12) / 2; // MEDIUM ~= 12px/char
            m_display.draw_text(std::max(x0, std::min(tx, x1)), kIconY + kIcon + 12,
                                buf, Display::FontSize::MEDIUM, Colors::WHITE);
        }
    };

    m_display.draw_text_centered(8, "MENU", Display::FontSize::MEDIUM);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);

    // Focused icon is static and centered — no slide offset, so it never
    // shifts or jitters on button presses. Direction is shown by < / >.
    if (m_menu_prev_index >= 0) m_menu_prev_index = -1;
    draw_entry(kFocusX, m_menu_index);

    // Side arrows (carousel wraps, so both are always live).
    m_display.draw_text(6, kIconY + 36, "<", Display::FontSize::LARGE, TFT_GRAY);
    m_display.draw_text(Display::WIDTH - 24, kIconY + 36, ">", Display::FontSize::LARGE, TFT_GRAY);

    m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
    m_display.draw_text_centered(222, "B2:ok B3:< B4:> B1:back", Display::FontSize::SMALL, TFT_GRAY);
}

// Wallet Info hub: three sub-functions, one focused at a time.
//   [0] View Balance (ViewBalanceIcon + "View Balance" below it)
//   [1] Receive QR   (framed "QR" tile + "QR" below it)
//   [2] SOL Price    (SolPriceIcon + "SOL Price" below it)
// B3/B4 flips the focused tab, B2 opens it, B1 goes back to the menu.
void UIManager::render_wallet_info() {
    static constexpr Color kTransparent = 0xF81F;
    static constexpr int kIcon = 96;
    static constexpr int kIconY = 52;
    static constexpr int kFocusX = (Display::WIDTH - kIcon) / 2; // 72
    static constexpr const char* kNames[3] = {"View Balance", "Receive QR", "SOL Price"};

    m_display.draw_text_centered(8, "Wallet Info", Display::FontSize::MEDIUM);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);

    // Auto-fetch cached address via global pointer set in app_main()
    if (m_wallet_address.empty()) {
        extern Fuchey::WalletCore* g_wallet_core_ptr;
        if (g_wallet_core_ptr) {
            auto addr = g_wallet_core_ptr->get_address();
            if (addr) m_wallet_address = *addr;
        }
    }

    const uint8_t tab = m_wallet_tab % 3;
    if (tab == 0) {
        m_display.draw_sprite_transparent(kFocusX, kIconY, kIcon, kIcon,
                                          ViewBalanceIcon_data, kTransparent);
    } else if (tab == 1) {
        m_display.draw_sprite_transparent(kFocusX, kIconY, kIcon, kIcon,
                                          QrIcon_data, kTransparent);
    } else {
        m_display.draw_sprite_transparent(kFocusX, kIconY, kIcon, kIcon,
                                          SolPriceIcon_data, kTransparent);
    }
    m_display.draw_text_centered(kIconY + kIcon + 12, kNames[tab],
                                 Display::FontSize::MEDIUM, Colors::WHITE);

    // Side arrows (hub wraps across the 3 tabs, so both are always live).
    m_display.draw_text(6, kIconY + 36, "<", Display::FontSize::LARGE, TFT_GRAY);
    m_display.draw_text(Display::WIDTH - 24, kIconY + 36, ">", Display::FontSize::LARGE, TFT_GRAY);

    // Truncated address reminder (first 4 ... last 4), gray.
    if (!m_wallet_address.empty() && m_wallet_address.size() > 12) {
        char trunc[16];
        snprintf(trunc, sizeof(trunc), "%.4s...%.4s",
                 m_wallet_address.c_str(),
                 m_wallet_address.c_str() + m_wallet_address.size() - 4);
        m_display.draw_text_centered(192, trunc, Display::FontSize::SMALL, TFT_GRAY);
    }

    m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
    m_display.draw_text_centered(222, "B3:< B4:> B2:open B1:back", Display::FontSize::SMALL, TFT_GRAY);
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
        m_display.draw_text_centered(8, "WALLET QR", Display::FontSize::MEDIUM);
        m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
        m_display.draw_text_centered(100, "No wallet", Display::FontSize::MEDIUM);
        m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
        m_display.draw_text_centered(222, "B1:hub", Display::FontSize::SMALL, TFT_GRAY);
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
        m_display.draw_text_centered(8, "WALLET QR", Display::FontSize::MEDIUM);
        m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
        m_display.draw_text_centered(100, "QR gen failed", Display::FontSize::MEDIUM);
        m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
        m_display.draw_text_centered(222, "B1:hub", Display::FontSize::SMALL, TFT_GRAY);
        return;
    }

    int qr_size = qrcodegen_getSize(qr_code);   // number of modules (e.g. 33 for V4)
    // Largest module size that fits the 240px panel with a quiet zone
    int px = 196 / qr_size;
    if (px > 6) px = 6;
    if (px < 2) px = 2;
    int total = qr_size * px;

    // Center the QR code on the full panel (no header — maximum scan size)
    int x_off = (Display::WIDTH  - total) / 2;
    int y_off = (Display::HEIGHT - total) / 2;

    // Quiet zone: clear margin around the code
    m_display.draw_text_centered(222, "B1:hub", Display::FontSize::SMALL, TFT_GRAY);

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
    m_display.draw_text_centered(8, "CONFIRM?", Display::FontSize::MEDIUM, TFT_ORANGE);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);

    char buf[32];
    snprintf(buf, sizeof(buf), "$%.2f", static_cast<double>(m_tx_amount_cents) / 100.0);
    m_display.draw_text_centered(72, buf, Display::FontSize::LARGE);

    if (!m_tx_description.empty())
        m_display.draw_text_centered(124, m_tx_description.c_str(), Display::FontSize::MEDIUM, TFT_CYAN);

    m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
    m_display.draw_text_centered(222, "B1 1x:send 2x/hold:no", Display::FontSize::SMALL, TFT_GRAY);
}

void UIManager::render_tx_result() {
    bool ok = m_tx_result_ok;

    if (!ok) {
        // Paint-4 animation for failed transfers — centered, red on black
        m_display.draw_text_centered(8, "FAILED", Display::FontSize::MEDIUM, Colors::RED);
        m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
        m_display.draw_bitmap((Display::WIDTH - 123) / 2, 56, 123, 30,
                              image_tx_fail_bits, Colors::RED);

        if (m_tx_result_msg[0]) m_display.draw_text_centered(140, m_tx_result_msg, Display::FontSize::MEDIUM);
        if (m_tx_result_recipient[0]) m_display.draw_text_centered(192, m_tx_result_recipient, Display::FontSize::SMALL, TFT_GRAY);
        return;
    }

    // Paint success frame — centered, green on black
    m_display.draw_text_centered(8, "SENT", Display::FontSize::MEDIUM, Colors::GREEN);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
    m_display.draw_bitmap((Display::WIDTH - 120) / 2, 56, 120, 29,
                          image_tx_success_bits, Colors::GREEN);

    // ── Transaction details ──
    char line1[32];
    char line2[56];
    if (m_tx_result_asset[0]) {
        snprintf(line1, sizeof(line1), "$%.2f %s",
                 static_cast<double>(m_tx_result_amount_cents) / 100.0,
                 m_tx_result_asset);
        snprintf(line2, sizeof(line2), "-> %s", m_tx_result_recipient);
    } else {
        line1[0] = '\0';
        line2[0] = '\0';
    }

    if (line1[0]) m_display.draw_text_centered(140, line1, Display::FontSize::MEDIUM);
    if (line2[0]) m_display.draw_text_centered(184, line2, Display::FontSize::SMALL, TFT_GRAY);
}

void UIManager::render_balance() {
    static constexpr Color kTransparent = 0xF81F;

    // Hero title, left-aligned like the mock, drawn once for every state.
    m_display.draw_gfx_text(22, 2, "BALANCE", &FreeMonoBold12pt7b, 2, 0x53FE);

    if (!m_balance_monitor) {
        m_display.draw_text_centered(100, "No balance service", Display::FontSize::MEDIUM);
        return;
    }

    if (m_wallet_address.empty()) {
        extern Fuchey::WalletCore* g_wallet_core_ptr;
        if (g_wallet_core_ptr) {
            auto addr = g_wallet_core_ptr->get_address();
            if (addr) m_wallet_address = *addr;
        }
    }

    if (m_wallet_address.empty()) {
        m_display.draw_text_centered(100, "No wallet setup", Display::FontSize::MEDIUM);
        m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
        m_display.draw_text_centered(222, "B1:back", Display::FontSize::SMALL, TFT_GRAY);
        return;
    }

    if (!m_bal_fetched) {
        // Boot-style loading screen under the shared hero title: label +
        // left-to-right sliding block (sawtooth wrap) at the ~4fps throttle
        // below — progress is unknowable so it slides instead of filling.
        int fw = 0, fh = 0;
        m_display.gfx_text_bounds("Fetching...", &FreeSansBold9pt7b, 1, &fw, &fh);
        m_display.draw_gfx_text((Display::WIDTH - fw) / 2, 150,
                                "Fetching...", &FreeSansBold9pt7b, 1, TFT_CYAN);
        static constexpr int kBarX = 20, kBarY = 196;
        static constexpr int kBarW = Display::WIDTH - 40, kBarH = 16, kBlk = 40;
        m_display.draw_rect(kBarX, kBarY, kBarW, kBarH, Colors::WHITE);
        const uint32_t now =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const int range = kBarW - 2 - kBlk;
        // ~20px per 250ms frame => full sweep in ~2s, then wraps to the left.
        const int pos = static_cast<int>(((now / 250u) * 20u) % static_cast<uint32_t>(range));
        m_display.fill_rect(kBarX + 1 + pos, kBarY + 1, kBlk, kBarH - 2, TFT_CYAN);
        return;
    }

    if (!m_bal_ok) {
        m_display.draw_text_centered(8, "BALANCE", Display::FontSize::MEDIUM);
        m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
        m_display.draw_text_centered(100, "Fetch failed", Display::FontSize::MEDIUM, Colors::RED);
        m_display.draw_text_centered(140, "Check WiFi, go back", Display::FontSize::SMALL, TFT_GRAY);
        m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
        m_display.draw_text_centered(222, "B1:back", Display::FontSize::SMALL, TFT_GRAY);
        return;
    }

    // Lopaka "BALANCE" hero layout on the dark theme: large coin artwork
    // bleeding off the left edge, FreeMono title, FreeSans labels, and the
    // live on-chain balances at 6 decimals (no placeholders anywhere).
    char sol_buf[24], usdc_buf[24];
    snprintf(sol_buf, sizeof(sol_buf), "%.5f", m_bal_sol);
    snprintf(usdc_buf, sizeof(usdc_buf), "%.5f", m_bal_usdc);

    // Coin artwork first — mock sizes/positions (77px SOL at (8,48),
    // 81px USDC at (6,132)); rows shifted up ~5/18px vs the mock so the
    // 81px USDC art clears the 214 footer divider (mock has no footer).
    m_display.draw_sprite_transparent(8, 48, 77, 77, BalanceSolIcon_data, kTransparent);
    m_display.draw_sprite_transparent(6, 132, 81, 81, BalanceUsdcIcon_data, kTransparent);

    // Value renderer: size 2 right-aligned to x=236; only whale-sized
    // values that still overflow shrink to size 1 (right-aligned too).
    auto draw_fit_value = [&](const char* txt, int x, int y) {
        int vw = 0, vh = 0;
        m_display.gfx_text_bounds(txt, &FreeSans9pt7b, 2, &vw, &vh);
        int size = 2, vx = x;
        if (vx + vw > Display::WIDTH - 4) vx = Display::WIDTH - 4 - vw;
        if (vx < 0) {
            size = 1;
            m_display.gfx_text_bounds(txt, &FreeSans9pt7b, 1, &vw, &vh);
            vx = Display::WIDTH - 4 - vw;
            if (vx < 0) vx = 0;
        }
        m_display.draw_gfx_text(vx, y, txt, &FreeSans9pt7b, size, Colors::GREEN);
    };

    // SOL label + live value, always the same size as USDC.
    m_display.draw_gfx_text(89, 67, "SOL", &FreeSans9pt7b, 1, 0xF4E0);
    draw_fit_value(sol_buf, 86, 85);

    // USDC label + live value.
    m_display.draw_gfx_text(89, 149, "USDC", &FreeSans9pt7b, 1, 0x12B6);
    draw_fit_value(usdc_buf, 88, 167);

    m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
    m_display.draw_text_centered(222, "B1:hub", Display::FontSize::SMALL, TFT_GRAY);
}

void UIManager::render_chat() {
    m_display.draw_text_centered(8, "AI CHAT", Display::FontSize::MEDIUM);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);

    // Wrap response text at ~38 chars per line (38 x 6px = 228px)
    const std::string& msg = m_last_ai_response;
    size_t pos = 0;
    int y = 42;
    while (pos < msg.size() && y < 206) {
        size_t end = std::min(pos + 38, msg.size());
        // Try to break at space
        if (end < msg.size() && msg[end] != ' ') {
            size_t sp = msg.rfind(' ', end);
            if (sp != std::string::npos && sp > pos) end = sp;
        }
        m_display.draw_text(8, y, msg.substr(pos, end - pos).c_str(), Display::FontSize::SMALL);
        pos = end;
        while (pos < msg.size() && msg[pos] == ' ') ++pos;
        y += 13;
    }

    m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
    m_display.draw_text_centered(222, "B1:menu", Display::FontSize::SMALL, TFT_GRAY);
}

// ─── Pomodoro timer ─────────────────────────────────────────
// Poppins hero time (Bold, auto-fit size 3->2) + Regular labels.
// B3 (left) = +sec, B4 (right) = +min, B2 = confirm, B1 = back.
void UIManager::render_pomodoro() {
    using Phase = PomodoroTimer::Phase;
    const PomodoroSettings& s = m_pomo.settings();
    const Phase phase = m_pomo.phase();
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    // Colon blinks at 1Hz — flips only when the second changes, so no
    // extra redraws beyond the per-second pomo_tick() request.
    const bool colon = ((now / 1000) % 2) == 0;

    char hero[16]{};
    char step[24]{};
    char sub[40]{};
    const char* footer = "B1:back";
    Color hero_color = Colors::WHITE;
    uint8_t pct = 0;
    bool show_bar = false;

    auto fmt = [](char* out, size_t n, uint8_t m, uint8_t sec, bool c) {
        if (c) snprintf(out, n, "%02u:%02u", m, sec);
        else   snprintf(out, n, "%02u %02u", m, sec);
    };

    switch (phase) {
        case Phase::SetWork:
            snprintf(step, sizeof(step), "SET WORK TIME");
            fmt(hero, sizeof(hero), s.work_min, s.work_sec, colon);
            sub[0] = '\0'; // no sub-line on work setup
            footer = "B4:+min B3:+sec B2:ok";
            break;
        case Phase::SetBreak:
            snprintf(step, sizeof(step), "SET BREAK TIME");
            fmt(hero, sizeof(hero), s.brk_min, s.brk_sec, colon);
            snprintf(sub, sizeof(sub), "00:00 = Skip Break");
            footer = "B4:+min B3:+sec B2:ok";
            break;
        case Phase::SetLoops:
            snprintf(step, sizeof(step), "SET LOOPS");
            snprintf(hero, sizeof(hero), "x%u", s.loops);
            sub[0] = '\0'; // summary lives on the Ready screen
            footer = "B4:+ B3:- B2:ok";
            break;
        case Phase::Ready:
            snprintf(step, sizeof(step), "READY?");
            fmt(hero, sizeof(hero), s.work_min, s.work_sec, true);
            snprintf(sub, sizeof(sub), "Break %02u:%02u - x%u",
                     s.brk_min, s.brk_sec, s.loops);
            footer = "B2:start B1:back";
            break;
        case Phase::RunWork:
        case Phase::RunBreak: {
            const bool work = (phase == Phase::RunWork);
            const uint32_t total = work ? m_pomo.work_ms() : m_pomo.break_ms();
            const uint32_t rem = m_pomo.remaining_ms();
            snprintf(step, sizeof(step), "%s %u/%u",
                     work ? "WORK" : "BREAK", m_pomo.loop_index(), s.loops);
            fmt(hero, sizeof(hero),
                static_cast<uint8_t>(rem / 60000),
                static_cast<uint8_t>((rem / 1000) % 60), colon);
            snprintf(sub, sizeof(sub), work ? "STAY FOCUSED" : "relax");
            if (total > 0) pct = static_cast<uint8_t>(100 - (100 * rem / total));
            show_bar = true;
            hero_color = work ? Colors::WHITE : TFT_CYAN;
            footer = "B2:pause B1:cancel";
            break;
        }
        case Phase::Paused:
            snprintf(step, sizeof(step), "PAUSED");
            fmt(hero, sizeof(hero),
                static_cast<uint8_t>(m_pomo.remaining_ms() / 60000),
                static_cast<uint8_t>((m_pomo.remaining_ms() / 1000) % 60), true);
            snprintf(sub, sizeof(sub), "%s %u/%u",
                     m_pomo.paused_from() == Phase::RunWork ? "work" : "break",
                     m_pomo.loop_index(), s.loops);
            footer = "B2:resume B1:cancel";
            break;
        case Phase::Done:
            snprintf(step, sizeof(step), "DONE!");
            snprintf(hero, sizeof(hero), "x%u/%u", s.loops, s.loops);
            snprintf(sub, sizeof(sub), "nice work");
            hero_color = Colors::GREEN;
            footer = "B2:again B1:menu";
            break;
    }

    m_display.draw_text_centered(8, "POMODORO", Display::FontSize::MEDIUM);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);

    gfx_centered(m_display, 42, step, &PoppinsBold9pt7b, TFT_ORANGE);

    // Hero time: largest Poppins Bold size that fits with side margins.
    int tw = 0, th = 0, hs = 3;
    m_display.gfx_text_bounds(hero, &PoppinsBold9pt7b, hs, &tw, &th);
    if (tw > Display::WIDTH - 16) {
        hs = 2;
        m_display.gfx_text_bounds(hero, &PoppinsBold9pt7b, hs, &tw, &th);
    }
    // Screens without a sub-line (SET WORK / SET LOOPS) center the hero
    // vertically in the content area (y 30..214) instead of parking at y=72.
    int hero_y = 72;
    if (sub[0] == '\0') {
        hero_y = 30 + (184 - th) / 2;
    }
    m_display.draw_gfx_text((Display::WIDTH - tw) / 2, hero_y,
                            hero, &PoppinsBold9pt7b, hs, hero_color);

    if (sub[0] != '\0') {
        gfx_centered(m_display, 152, sub, &PoppinsRegular9pt7b, TFT_SILVER);
    }
    if (show_bar) {
        m_display.draw_progress_bar(20, 176, 200, 14, pct,
                                    hero_color == TFT_CYAN ? TFT_CYAN : TFT_ORANGE);
    }

    m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
    m_display.draw_text_centered(222, footer, Display::FontSize::SMALL, TFT_GRAY);
}

void UIManager::render_badge() {
    static constexpr Color kTransparent = 0xF81F;
    m_display.draw_text_centered(8, "BADGE", Display::FontSize::MEDIUM);
    m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
    m_display.draw_sprite_transparent((Display::WIDTH - 96) / 2, 52, 96, 96,
                                      BadgeIcon_data, kTransparent);
    m_display.draw_text_centered(160, "Badge", Display::FontSize::MEDIUM);
    m_display.draw_text_centered(186, "Coming soon", Display::FontSize::SMALL, TFT_GRAY);
    m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
    m_display.draw_text_centered(222, "B1:menu", Display::FontSize::SMALL, TFT_GRAY);
}

// ─── Animation test ───────────────────────────────────────
// Partial-flush animation: static chrome is full-flushed once on entry;
// each changed frame erases/redraws only the 96x96 sprite rect (transparent
// color-key 0xF81F) plus a narrow counter strip. No-op when tick() is false,
// so run() calling us every tick costs zero SPI on idle frames.
void UIManager::render_anim_test() {
    static constexpr Color kTransparent = 0xF81F;
    static constexpr int kCounterY = 190;
    static constexpr int kCounterH = 10; // SMALL font is 8px tall

    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (!m_anim_started) {
        m_anim_player.set_anim(&YetiAnim, now);
        m_anim_started = true;
        m_anim_chrome_drawn = false;
    }

    const int x = (Display::WIDTH - YetiAnim.w) / 2;
    const int y = 88 - YetiAnim.h / 2 + 30;

    if (!m_anim_chrome_drawn) {
        m_display.clear();
        m_display.draw_text_centered(8, "YETI", Display::FontSize::MEDIUM, TFT_CYAN);
        m_display.draw_hline(0, 30, Display::WIDTH, TFT_GRAY);
        const SpritePixel* f0 = m_anim_player.current_frame_data();
        if (f0 != nullptr) {
            m_display.fill_rect(x, y, YetiAnim.w, YetiAnim.h, TFT_BLACK);
            m_display.draw_sprite_transparent(x, y, YetiAnim.w, YetiAnim.h, f0, kTransparent);
        }
        char buf[24];
        snprintf(buf, sizeof(buf), "frame %u/%u", m_anim_player.current_frame() + 1, YetiAnim.frames);
        m_display.draw_text_centered(kCounterY, buf, Display::FontSize::SMALL, TFT_GRAY);
        m_display.draw_hline(0, 214, Display::WIDTH, TFT_GRAY);
        m_display.draw_text_centered(222, "B1:menu", Display::FontSize::SMALL, TFT_GRAY);
        m_display.flush();
        m_anim_chrome_drawn = true;
        m_anim_last_frame = m_anim_player.current_frame();
        return;
    }

    if (!m_anim_player.tick(now)) return; // frame unchanged → zero SPI
    const uint8_t fr = m_anim_player.current_frame();
    if (fr == m_anim_last_frame) return;
    m_anim_last_frame = fr;

    const SpritePixel* f = m_anim_player.current_frame_data();
    if (f == nullptr) return;
    // Erase keeps old opaque pixels from leaving trails behind transparent ones.
    m_display.fill_rect(x, y, YetiAnim.w, YetiAnim.h, TFT_BLACK);
    m_display.draw_sprite_transparent(x, y, YetiAnim.w, YetiAnim.h, f, kTransparent);
    m_display.flush_window(x, y, YetiAnim.w, YetiAnim.h);

    // Counter strip uses a full-width window so push_window() takes the
    // no-staging fast path (w == WIDTH).
    m_display.fill_rect(0, kCounterY, Display::WIDTH, kCounterH, TFT_BLACK);
    char buf[24];
    snprintf(buf, sizeof(buf), "frame %u/%u", fr + 1, YetiAnim.frames);
    m_display.draw_text_centered(kCounterY, buf, Display::FontSize::SMALL, TFT_GRAY);
    m_display.flush_window(0, kCounterY, Display::WIDTH, kCounterH);
}

// ─── Worlds Fair banner (static full-screen image) ──────────
// render() clear()s + flush()es around us; one full flush (~115ms @8MHz)
// is fine for a static image.
void UIManager::render_fair_pass() {
    m_display.draw_sprite(0, 0, FairPass.w, FairPass.h, FairPass.data);
}

// ─── Home screen (Pass_design bg + clock/date/weather + animated Yeti) ─
// Self-flushing like ANIM_TEST, but the background is a photo: every overlay
// redraw first restores its bg crop via draw_sprite_crop (a black clear would
// leave a box), then draws, then flush_window()s only that region.
// Clock/date/weather live on ONE frosted band (pre-blurred PassBlurTop asset
// covers y 0..130) so overlapping pads can't clobber each other.
void UIManager::render_home() {
    // Exact lopaka placement: time (5,4) size 3, date (5,53) size 2,
    // temp (5,88) size 2, icon 30x32 at (88,88), yeti 96x96 at (133,111).
    // Frosted pill hugs the glyphs (pad 1), not a filled band.
    static constexpr int kYetiX = 133, kYetiY = 111;
    static constexpr int kTimeX = 5,   kTimeY = 4, kTimeScale = 3;
    static constexpr int kDateX = 5,   kDateY = 53, kDateSize = 2;
    static constexpr int kWx = 5,      kWy = 88, kWSize = 2;
    static constexpr int kIx = 88,     kIy = 88;
    static constexpr Color kTransparent = 0xF81F;
    static constexpr Color kClock = 0xFFE0;  // yellow

    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (!m_home_started) {
        m_home_yeti.set_anim(&YetiAnim, now);
        m_home_started = true;
        m_home_chrome = false;
    }

    time_t t = time(nullptr);
    struct tm ti{};
    const bool synced = (t > 100000 && localtime_r(&t, &ti));

    char buf[16];
    if (synced) {
        int h12 = ti.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, sizeof(buf), "%d:%02d %s", h12, ti.tm_min,
                 ti.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    char dbuf[16];
    if (synced) {
        strftime(dbuf, sizeof(dbuf), "%d %a", &ti); // "09 Wed"
    } else {
        dbuf[0] = '\0';
    }
    const int minute = synced ? ti.tm_hour * 60 + ti.tm_min : -1;

    // Weather text + condition icon (dynamic).
    char wbuf[16];
    uint8_t wcode = 255;
    const SpritePixel* wbits = nullptr;
    if (m_weather_temp < -100.0f) {
        snprintf(wbuf, sizeof(wbuf), "-- C");
    } else {
        snprintf(wbuf, sizeof(wbuf), "%.0f C", static_cast<double>(m_weather_temp));
        wcode = m_weather_code;
        wbits = home_weather_bits(wcode);
    }

    // Fixed lopaka geometry: time size 3 left-aligned (even the widest
    // "12:59 PM" fits from x=0), date/temp size 2 below it.
    static constexpr int scale = kTimeScale;
    int tw = 0, th = 0;
    m_display.gfx_text_bounds(buf, &FreeSansBold9pt7b, scale, &tw, &th);
    const int tx = kTimeX;
    // Three tight frosted rows (time / date / weather) instead of one band,
    // so empty space right of short rows stays sharp photo. The band bbox
    // below is only the sharp-restore + SPI flush envelope.
    int dw = 0, dh = 0, ww = 0, wh = 0;
    m_display.gfx_text_bounds(dbuf, &FreeSansBold9pt7b, kDateSize, &dw, &dh);
    m_display.gfx_text_bounds(wbuf, &FreeSansBold9pt7b, kWSize, &ww, &wh);
    const int r1x = tx - 1, r1y = kTimeY - 1, r1w = tw + 2, r1h = th + 2;
    const bool has_date = (dw > 0 && dh > 0);
    const int r2x = kDateX - 1, r2y = kDateY - 1, r2w = dw + 2, r2h = dh + 2;
    int qx0 = kWx, qy0 = kWy, qx1 = kWx + ww, qy1 = kWy + wh;
    if (wbits != nullptr) {
        qx0 = std::min(qx0, kIx); qy0 = std::min(qy0, kIy);
        qx1 = std::max(qx1, kIx + kWeatherIconW);
        qy1 = std::max(qy1, kIy + kWeatherIconH);
    }
    const int r3x = qx0 - 1, r3y = qy0 - 1;
    const int r3w = (qx1 - qx0) + 2, r3h = (qy1 - qy0) + 2;
    int px = r1x, py = r1y, pr = r1x + r1w, pb = r1y + r1h;
    auto grow = [&](int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        px = std::min(px, x); py = std::min(py, y);
        pr = std::max(pr, x + w); pb = std::max(pb, y + h);
    };
    if (has_date) grow(r2x, r2y, r2w, r2h);
    grow(r3x, r3y, r3w, r3h);
    const int pw = pr - px, ph = pb - py;
    auto frost_rows = [&]() {
        m_display.draw_sprite_crop(r1x, r1y, PassBlurTop.w, r1x, r1y, r1w, r1h,
                                   PassBlurTop.data);
        if (has_date) {
            m_display.draw_sprite_crop(r2x, r2y, PassBlurTop.w, r2x, r2y, r2w, r2h,
                                       PassBlurTop.data);
        }
        m_display.draw_sprite_crop(r3x, r3y, PassBlurTop.w, r3x, r3y, r3w, r3h,
                                   PassBlurTop.data);
    };
    auto draw_texts = [&]() {
        m_display.draw_gfx_text(tx, kTimeY, buf, &FreeSansBold9pt7b, scale, kClock);
        m_display.draw_gfx_text(kDateX, kDateY, dbuf, &FreeSansBold9pt7b, kDateSize, kClock);
        m_display.draw_gfx_text(kWx, kWy, wbuf, &FreeSansBold9pt7b, kWSize, kClock);
        if (wbits != nullptr) {
            m_display.draw_sprite_transparent(kIx, kIy, kWeatherIconW, kWeatherIconH,
                                              wbits, kWeatherTransparent);
        }
    };

    if (!m_home_chrome) {
        // Frosted band comes from a pre-blurred asset (PassBlurTop covers
        // y 0..130) — deterministic on every boot, no heap or pixel math here.
        ESP_LOGI(TAG, "HOME chrome: band=(%d,%d %dx%d) free_blk=%u",
                 px, py, pw, ph,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        m_display.draw_sprite(0, 0, PassDesign.w, PassDesign.h, PassDesign.data);
        frost_rows();
        draw_texts();
        const SpritePixel* f0 = m_home_yeti.current_frame_data();
        if (f0 != nullptr) {
            m_display.draw_sprite_transparent(kYetiX, kYetiY,
                YetiAnim.w, YetiAnim.h, f0, kTransparent);
        }
        m_display.flush();
        m_home_chrome = true;
        m_home_last_frame = m_home_yeti.current_frame();
        m_home_last_minute = minute;
        m_home_tx = px; m_home_ty = py; m_home_tw = pw; m_home_th = ph;
        snprintf(m_home_wbuf, sizeof(m_home_wbuf), "%s", wbuf);
        m_home_wcode = wcode;
        return;
    }

    if (minute != m_home_last_minute ||
        strcmp(wbuf, m_home_wbuf) != 0 || wcode != m_home_wcode) {
        // Union old + new frosted bands, sharp-restore it, then frost the
        // new band — shrunken strings leave no blurred remnants.
        const int rx = std::min(m_home_tx, px);
        const int ry = std::min(m_home_ty, py);
        const int rr = std::max(m_home_tx + m_home_tw, px + pw);
        const int rb = std::max(m_home_ty + m_home_th, py + ph);
        const int rw = rr - rx, rh = rb - ry;
        m_display.draw_sprite_crop(rx, ry, PassDesign.w, rx, ry, rw, rh, PassDesign.data);
        frost_rows();
        draw_texts();
        m_display.flush_window(rx, ry, rw, rh);
        m_home_last_minute = minute;
        m_home_tx = px; m_home_ty = py; m_home_tw = pw; m_home_th = ph;
        snprintf(m_home_wbuf, sizeof(m_home_wbuf), "%s", wbuf);
        m_home_wcode = wcode;
    }

    if (!m_home_yeti.tick(now)) return; // frame unchanged → zero SPI
    const uint8_t fr = m_home_yeti.current_frame();
    if (fr == m_home_last_frame) return;
    m_home_last_frame = fr;

    const SpritePixel* f = m_home_yeti.current_frame_data();
    if (f == nullptr) return;
    m_display.draw_sprite_crop(kYetiX, kYetiY, PassDesign.w,
        kYetiX, kYetiY, YetiAnim.w, YetiAnim.h, PassDesign.data);
    m_display.draw_sprite_transparent(kYetiX, kYetiY,
        YetiAnim.w, YetiAnim.h, f, kTransparent);
    m_display.flush_window(kYetiX, kYetiY, YetiAnim.w, YetiAnim.h);
}

// ─── Setup wizard renderer (Poppins mix, all FreeSans-free) ───
void UIManager::render_setup() {
    switch (m_setup_stage) {

        case SetupStage::WIFI_PROMPT:
            // Step 1: Ask for WiFi
            setup_header(m_display);
            gfx_centered(m_display, 46, "Step 1", &PoppinsBold9pt7b, TFT_ORANGE);
            gfx_centered(m_display, 72, "Connect to WiFi", &PoppinsBold9pt7b, Colors::WHITE);
            gfx_centered(m_display, 104, "Go to serial monitor", &PoppinsRegular9pt7b, TFT_SILVER);
            gfx_centered(m_display, 130, "@ 115200", &PoppinsRegular9pt7b, TFT_CYAN);
            gfx_centered(m_display, 158, "type:", &PoppinsRegular9pt7b, TFT_GRAY);
            command_pill(m_display, 182, "w <SSID> <Password>", &PoppinsRegular9pt7b, TFT_CYAN);
            gfx_centered(m_display, 222, "Step 1 of 2", &PoppinsRegular9pt7b, TFT_GRAY);
            break;

        case SetupStage::WIFI_CONNECTING: {
            // Static screen: full-screen redraws are slow on SPI, so no
            // animated dots here — repaint happens on stage change only.
            setup_header(m_display);
            gfx_centered(m_display, 52, "Connecting...", &PoppinsBold9pt7b, Colors::WHITE);

            // SSID is user data: trim with "..." so wide names can't overflow.
            command_pill(m_display, 84,
                         fit_gfx(m_display, m_connecting_ssid, &PoppinsRegular9pt7b,
                                 Display::WIDTH - 32),
                         &PoppinsRegular9pt7b, Colors::WHITE);

            gfx_centered(m_display, 124, "Waiting for IP...", &PoppinsRegular9pt7b, TFT_GRAY);

            // Static empty progress bar (frame only)
            m_display.draw_progress_bar(20, 180, 200, 16, 0, TFT_CYAN);
            gfx_centered(m_display, 222, "Step 1 of 2", &PoppinsRegular9pt7b, TFT_GRAY);
            break;
        }

        case SetupStage::WALLET_PROMPT:
            setup_header(m_display);
            gfx_centered(m_display, 44, "WiFi: OK", &PoppinsBold9pt7b, Colors::GREEN);
            gfx_centered(m_display, 72, "Step 2", &PoppinsBold9pt7b, TFT_ORANGE);
            gfx_centered(m_display, 96, "Create Your Wallet", &PoppinsBold9pt7b, Colors::WHITE);
            command_pill(m_display, 126, "wallet_create", &PoppinsRegular9pt7b, TFT_CYAN);
            command_pill(m_display, 154, "wallet_import <key>", &PoppinsRegular9pt7b, TFT_CYAN);
            gfx_centered(m_display, 186, "<mnemonic / key>", &PoppinsRegular9pt7b, TFT_GRAY);
            gfx_centered(m_display, 222, "Step 2 of 2", &PoppinsRegular9pt7b, TFT_GRAY);
            break;

        case SetupStage::DONE:
        default:
            // size-2 hero: measure then draw centered manually
            {
                int w = 0, h = 0;
                m_display.gfx_text_bounds("Done!", &PoppinsBold9pt7b, 2, &w, &h);
                m_display.draw_gfx_text((Display::WIDTH - w) / 2, 96,
                                        "Done!", &PoppinsBold9pt7b, 2, Colors::GREEN);
            }
            gfx_centered(m_display, 140, "Opening home...", &PoppinsRegular9pt7b, TFT_GRAY);
            break;
    }
}

// ─── FreeRTOS task ────────────────────────────────────────
void UIManager::task_entry(void* arg) {
    static_cast<UIManager*>(arg)->run();
}

// One-shot worker: blocking HTTP stays off the UI task. Posts
// BALANCE_UPDATED when done (even if the user already backed out —
// the handler then only warms the cache).
void UIManager::balance_fetch_entry(void* arg) {
    auto* self = static_cast<UIManager*>(arg);
    double sol = 0.0, usdc = 0.0;
    bool ok = false;
    if (self->m_balance_monitor) {
        ok = self->m_balance_monitor->fetch_balances(sol, usdc);
    }
    Events::Event evt{};
    evt.type = Events::EventType::BALANCE_UPDATED;
    evt.data.balance.sol = sol;
    evt.data.balance.usdc = usdc;
    evt.data.balance.ok = ok;
    Events::post(Events::g_ui_queue, evt);
    vTaskDelete(nullptr);
}

void UIManager::start_balance_fetch() {
    if (m_bal_fetching || m_balance_monitor == nullptr) return;
    m_bal_fetching = true;
    m_bal_anim_last_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    xTaskCreate(balance_fetch_entry, "bal_fetch", 8192, this,
                tskIDLE_PRIORITY + 1, nullptr);
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
                     btn.id == ButtonId::B1_TX_BACK ? "B1_TX_BACK" :
                     btn.id == ButtonId::B2_MENU_SELECT ? "B2_MENU_SELECT" :
                     btn.id == ButtonId::B3_PREV ? "B3_PREV" : "B4_NEXT",
                     btn.event == ButtonEvent::PRESS        ? "PRESS" :
                     btn.event == ButtonEvent::DOUBLE_PRESS ? "DOUBLE_PRESS" :
                     btn.event == ButtonEvent::LONG_PRESS   ? "LONG_PRESS" : "RELEASE");

            // Reset idle cycle timer on any button activity
            m_last_idle_cycle_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

            // ── TX_CONFIRM: B1 exclusive ───────────────────────
            // B1 single tap = accept (deferred until no double/long follows),
            // B1 double/long = reject. B2/B3/B4 ignored here.
            if (m_current_screen == UIScreen::TX_CONFIRM) {
                if (btn.id == ButtonId::B1_TX_BACK) {
                    if (btn.event == ButtonEvent::PRESS) {
                        m_tx_press_start_ms = btn.timestamp_ms;
                        m_tx_pending_accept = false;
                    } else if (btn.event == ButtonEvent::RELEASE) {
                        // Clean single tap candidate — defer accept to rule out
                        // a fast second press (double press) or a held long press.
                        m_tx_pending_accept = true;
                        m_tx_accept_deadline_ms = m_tx_press_start_ms + 500;
                    } else if (btn.event == ButtonEvent::DOUBLE_PRESS ||
                               btn.event == ButtonEvent::LONG_PRESS) {
                        reject_transaction();
                    }
                }
            } else if (m_current_screen == UIScreen::POMODORO_VIEW) {
                // Pomodoro owns all four buttons while open.
                handle_pomodoro_button(btn);
            } else if (btn.id == ButtonId::B1_TX_BACK) {
                // ── B1 = hierarchical Back everywhere else ─────
                if (btn.event == ButtonEvent::PRESS) go_back();
            } else if (btn.id == ButtonId::B2_MENU_SELECT) {
                // ── B2 = open menu / select ────────────────────
                if (btn.event != ButtonEvent::PRESS) {
                    // ignore RELEASE/DOUBLE/LONG on B2
                } else if (m_current_screen == UIScreen::MENU_MAIN) {
                    open_menu_index(m_menu_index);
                } else if (m_current_screen == UIScreen::WALLET_INFO) {
                    // Hub: B2 opens the focused sub-tab (Balance / QR).
                    open_wallet_tab(m_wallet_tab);
                } else if (m_current_screen == UIScreen::WALLET_QR ||
                           m_current_screen == UIScreen::CHAT_VIEW ||
                           m_current_screen == UIScreen::IDLE_PRICE ||
                           m_current_screen == UIScreen::BALANCE_VIEW ||
                           m_current_screen == UIScreen::BADGE_VIEW ||
                           m_current_screen == UIScreen::FAIR_PASS ||
                           m_current_screen == UIScreen::ANIM_TEST) {
                    // In a sub-screen B2 re-opens the menu (no-op if already there)
                    ESP_LOGI(TAG, "[Menu] B2 -> opening main menu");
                    set_screen(UIScreen::MENU_MAIN);
                } else if (m_current_screen == UIScreen::TX_SUCCESS ||
                           m_current_screen == UIScreen::TX_FAIL) {
                    ESP_LOGI(TAG, "Screen: TX result -> returning to idle");
                    set_screen(UIScreen::HOME);
                } else {
                    ESP_LOGI(TAG, "[Menu] Opening main menu");
                    set_screen(UIScreen::MENU_MAIN);
                }
            } else if (btn.id == ButtonId::B3_PREV) {
                // ── B3 = previous ──────────────────────────────
                // Only MENU_MAIN (icon carousel) and the WALLET_INFO hub
                // (Balance/QR focus) respond. Content screens ignore B3 —
                // press B1 to go back first.
                if (btn.event == ButtonEvent::PRESS) {
                    if (m_current_screen == UIScreen::MENU_MAIN) {
                        step_menu(-1);
                    } else if (m_current_screen == UIScreen::WALLET_INFO) {
                        step_wallet_tab(-1);
                    } else if (m_current_screen == UIScreen::TX_SUCCESS ||
                               m_current_screen == UIScreen::TX_FAIL) {
                        ESP_LOGI(TAG, "Screen: TX result -> returning to idle");
                        set_screen(UIScreen::HOME);
                    }
                }
            } else if (btn.id == ButtonId::B4_NEXT) {
                // ── B4 = next ──────────────────────────────────
                // Only MENU_MAIN (icon carousel) and the WALLET_INFO hub
                // (Balance/QR focus) respond. Content screens ignore B4 —
                // press B1 to go back first.
                if (btn.event == ButtonEvent::PRESS) {
                    if (m_current_screen == UIScreen::MENU_MAIN) {
                        step_menu(+1);
                    } else if (m_current_screen == UIScreen::WALLET_INFO) {
                        step_wallet_tab(+1);
                    } else if (m_current_screen == UIScreen::TX_SUCCESS ||
                               m_current_screen == UIScreen::TX_FAIL) {
                        ESP_LOGI(TAG, "Screen: TX result -> returning to idle");
                        set_screen(UIScreen::HOME);
                    }
                }
            }
        }

        // Any button activity can change what's on screen next tick
        request_redraw();

        // Pomodoro countdown + hold-repeat while the view is open.
        if (m_current_screen == UIScreen::POMODORO_VIEW) {
            pomo_tick(static_cast<uint32_t>(esp_timer_get_time() / 1000));
        }
        // Buzzer pattern keeps sounding even if the user leaves the view.
        if (m_buzzer && m_buzz.is_active()) {
            m_buzz.tick(static_cast<uint32_t>(esp_timer_get_time() / 1000), *m_buzzer);
        }

        // Deferred TX accept — a clean single tap was confirmed (no double/long press)
        if (m_current_screen == UIScreen::TX_CONFIRM && m_tx_pending_accept) {
            uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (now >= m_tx_accept_deadline_ms) {
                approve_transaction();
            }
        }

        // Auto-timeout
        if (m_current_screen == UIScreen::TX_SUCCESS ||
            m_current_screen == UIScreen::TX_FAIL) {
            uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (now - m_tx_result_start_ms >= 4000) {
                set_screen(UIScreen::HOME);
            }
        } else if (m_current_screen == UIScreen::MENU_MAIN ||
            m_current_screen == UIScreen::WALLET_INFO ||
            m_current_screen == UIScreen::WALLET_QR) {
            uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (now - m_last_idle_cycle_ms >= 15000) {
                ESP_LOGI(TAG, "[Menu] Timeout after 15s inactivity -> Returning to Idle Cycle");
                set_screen(UIScreen::HOME);
            }
        } else if (m_current_screen == UIScreen::BALANCE_VIEW) {
            uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (now - m_bal_fetch_start_ms >= 15000) {
                ESP_LOGI(TAG, "Screen: BALANCE_VIEW timeout -> WALLET_INFO hub");
                set_screen(UIScreen::WALLET_INFO);
            }
        } else if (m_current_screen == UIScreen::IDLE_PRICE) {
            // SOL Price now lives under the Wallet hub (not the idle cycle).
            // 30s window (room to read price/high/low/change after a fetch
            // lands); re-armed by fresh PRICE_UPDATED events above.
            uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (now - m_last_idle_cycle_ms >= 30000) {
                ESP_LOGI(TAG, "Screen: IDLE_PRICE timeout -> WALLET_INFO hub");
                set_screen(UIScreen::WALLET_INFO);
            }
        } else if (m_current_screen == UIScreen::POMODORO_VIEW) {
            // Setup screens idle back to the menu; a running/paused timer
            // never times out.
            if (m_pomo.is_setup() ||
                m_pomo.phase() == PomodoroTimer::Phase::Done) {
                uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
                if (now - m_last_idle_cycle_ms >= Pomodoro::SETUP_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "Screen: POMODORO setup timeout -> MENU_MAIN");
                    set_screen(UIScreen::MENU_MAIN);
                }
            }
        } else if (!m_setup_needed) {
            // Cycle ambient idle screens (Home <-> Weather)
            cycle_idle_screen();
        }

        // On-demand balance fetch (async worker; the UI keeps painting
        // the loading screen while HTTP runs elsewhere).
        if (m_current_screen == UIScreen::BALANCE_VIEW && !m_bal_fetched) {
            start_balance_fetch();
        }

        // Redraw only when state actually changed (screen/data/button).
        // ANIM_TEST and HOME are polled every tick but no-op (zero SPI)
        // unless the frame/minute advanced.
        bool need_render = m_redraw_epoch.load(std::memory_order_relaxed) != m_last_rendered_epoch;

        if (!m_setup_needed && m_current_screen == UIScreen::ANIM_TEST) {
            need_render = true;
        } else if (!m_setup_needed && m_current_screen == UIScreen::HOME) {
            need_render = true;
        } else if (!m_setup_needed && m_current_screen == UIScreen::BALANCE_VIEW &&
                   !m_bal_fetched) {
            // Loading marquee: full flush costs ~115ms @8MHz SPI, cap at ~4fps.
            uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (now_ms - m_bal_anim_last_ms >= 250) {
                m_bal_anim_last_ms = now_ms;
                need_render = true;
            }
        }

        if (need_render) {
            render();
            m_last_rendered_epoch = m_redraw_epoch.load(std::memory_order_relaxed);
        }

        vTaskDelay(pdMS_TO_TICKS(Timing::DISPLAY_UPDATE_MS));
    }
}

} // namespace Fuchey

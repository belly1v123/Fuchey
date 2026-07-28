#pragma once
// ============================================================
// Fuchey — UIManager.hpp
// UI state machine and OLED rendering loop.
// Handles cycling ambient screens (Clock -> Weather -> SOL Price -> Message)
// and handles transitioning to active hardware wallet mode when requested.
// ============================================================

#include "../Display.hpp"
#include "../../events/Events.hpp"
#include "../../balance/BalanceMonitor.hpp"
#include <cstdint>
#include <string>

namespace Fuchey {

enum class UIScreen {
    IDLE_CLOCK,
    IDLE_WEATHER,
    IDLE_PRICE,
    IDLE_MESSAGE,
    MENU_MAIN,
    WALLET_INFO,
    WALLET_QR,
    TX_CONFIRM,
    TX_SUCCESS,
    TX_FAIL,
    CHAT_VIEW,
    BALANCE_VIEW
};

// Setup wizard stages (first-boot only)
enum class SetupStage {
    WIFI_PROMPT,       // Waiting for user to type WiFi credentials
    WIFI_CONNECTING,   // Credentials entered, waiting for IP
    WALLET_PROMPT,     // WiFi ready, waiting for wallet input
    DONE,              // Setup complete, entering idle mode
};

class UIManager {
public:
    explicit UIManager(Display& display);
    ~UIManager() = default;

    bool init();
    void set_screen(UIScreen screen);

    void set_setup_needed(bool wifi_missing, bool wallet_missing);
    void mark_wifi_configured(const char* ssid = nullptr); // ssid shown on OLED during connecting
    void mark_wallet_configured(const char* address = nullptr); // address cached for WALLET_INFO screen
    void on_wifi_got_ip();   // Called when WIFI_GOT_IP event received

    // Render loop processing
    void render();
    void process_event(const Events::Event& evt);

    static void task_entry(void* arg);
    void run();

    void set_balance_monitor(BalanceMonitor* bm) { m_balance_monitor = bm; }

private:
    Display& m_display;
    UIScreen m_current_screen{UIScreen::IDLE_CLOCK};

    // Ambient cached data
    float       m_weather_temp{-999.0f};
    float       m_sol_price{-1.0f};
    std::string m_weather_city{"--"};
    std::string m_last_ai_response{"Hello! I am Fuchey."};
    std::string m_tx_description{"Transfer 0.1 SOL"};
    uint64_t    m_tx_amount_cents{0};

    // Transaction result (for TX_SUCCESS / TX_FAIL screens)
    bool        m_tx_result_ok{false};
    char        m_tx_result_asset[8]{};
    uint64_t    m_tx_result_amount_cents{0};
    char        m_tx_result_recipient[48]{};
    char        m_tx_result_msg[64]{};
    uint32_t    m_tx_result_start_ms{0};

    // Balance view
    BalanceMonitor* m_balance_monitor{nullptr};
    double          m_bal_sol{0.0};
    double          m_bal_usdc{0.0};
    bool            m_bal_fetched{false};
    uint32_t        m_bal_fetch_start_ms{0};

    uint32_t    m_last_idle_cycle_ms{0};

    // Setup wizard
    bool        m_setup_needed{false};
    SetupStage  m_setup_stage{SetupStage::WIFI_PROMPT};
    std::string m_connecting_ssid{};    // SSID being connected to (shown on OLED)
    std::string m_wallet_address{};     // Cached after wallet created/imported
    uint32_t    m_connecting_dots_ms{0};
    uint8_t     m_connecting_dots{0};
    uint8_t     m_menu_index{0};

    void render_clock();
    void render_weather();
    void render_price();
    void render_message();
    void render_menu();
    void render_wallet_info();
    void render_wallet_qr();
    void render_tx_confirm();
    void render_tx_result();
    void render_balance();
    void render_chat();
    void render_setup();

    void cycle_idle_screen();

    static constexpr const char* TAG = "UIManager";
};

} // namespace Fuchey

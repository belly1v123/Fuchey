#pragma once
// ============================================================
// Fuchey — UIManager.hpp
// UI state machine and OLED rendering loop.
// Handles cycling ambient screens (Clock -> Weather -> SOL Price -> Message)
// and handles transitioning to active hardware wallet mode when requested.
// ============================================================

#include "../Display.hpp"
#include "../../events/Events.hpp"
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
    TX_CONFIRM,
    CHAT_VIEW
};

class UIManager {
public:
    explicit UIManager(Display& display);
    ~UIManager() = default;

    bool init();
    void set_screen(UIScreen screen);

    // Render loop processing
    void render();
    void process_event(const Events::Event& evt);

    static void task_entry(void* arg);
    void run();

private:
    Display& m_display;
    UIScreen m_current_screen{UIScreen::IDLE_CLOCK};

    // Ambient cached data
    float       m_weather_temp{22.5f};
    float       m_sol_price{150.00f};
    std::string m_last_ai_response{"Hello! I am Fuchey."};
    std::string m_tx_description{"Transfer 0.1 SOL"};
    uint64_t    m_tx_amount_cents{0};

    uint32_t    m_last_idle_cycle_ms{0};

    void render_clock();
    void render_weather();
    void render_price();
    void render_message();
    void render_menu();
    void render_wallet_info();
    void render_tx_confirm();
    void render_chat();

    void cycle_idle_screen();

    static constexpr const char* TAG = "UIManager";
};

} // namespace Fuchey

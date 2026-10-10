#pragma once
// ============================================================
// Fuchey — UIManager.hpp
// UI state machine and OLED rendering loop.
// Handles cycling ambient screens (Clock -> Weather -> SOL Price -> Message)
// and handles transitioning to active hardware wallet mode when requested.
// ============================================================

#include "../Display.hpp"
#include "SpritePlayer.hpp"
#include "../../events/Events.hpp"
#include "../../balance/BalanceMonitor.hpp"
#include "../../buttons/ButtonDriver.hpp"
#include "../../buzzer/Buzzer.hpp"
#include "../../wallet/WalletCreateSession.hpp"
#include "../../wallet/RecoveryController.hpp"
#include "../../pomodoro/PomodoroTimer.hpp"
#include "../../wearables/WearRenderer.hpp"
#include "../../behaviour/Behaviour.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace Fuchey {

class LedIndicator;
class PriceService;

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
    BALANCE_VIEW,
    POMODORO_VIEW,
    BADGE_VIEW,
    FAIR_PASS,
    HID_REMOTE,
    RECOVERY,      // scrambled-grid phrase entry (driven by the companion app)
    WALLET_CREATE, // new wallet: words shown on device only, confirmed via app grid
    HOME
};

// Setup wizard stages (first-boot only)
enum class SetupStage {
    WIFI_PROMPT,       // Waiting for user to type WiFi credentials
    WIFI_CONNECTING,   // Credentials entered, waiting for IP
    WALLET_PROMPT,     // WiFi ready, waiting for wallet input
    LOCATION_PROMPT,   // Wallet ready, waiting for weather location
    DONE,              // Setup complete, entering idle mode
};

class UIManager {
public:
    explicit UIManager(Display& display);
    ~UIManager() = default;

    bool init();
    void set_screen(UIScreen screen);

    void set_setup_needed(bool wifi_missing, bool wallet_missing, bool location_missing);
    void mark_wifi_configured(const char* ssid = nullptr); // ssid shown on OLED during connecting
    void mark_wallet_configured(const char* address = nullptr); // address cached for WALLET_INFO screen
    void mark_location_configured(const char* city = nullptr); // city cached for weather label
    void on_wifi_got_ip();   // Called when WIFI_GOT_IP event received
    void set_wifi_up(bool up) { m_wifi_up = up; }   // initial state at boot

    // Render loop processing
    void render();
    void process_event(const Events::Event& evt);

    static void task_entry(void* arg);
    void run();

    // One-shot balance fetch worker (keeps blocking HTTP off the UI task).
    static void balance_fetch_entry(void* arg);
    void start_balance_fetch();
    void set_balance_monitor(BalanceMonitor* bm) { m_balance_monitor = bm; }
    void set_price_service(PriceService* ps);
    void set_led_indicator(LedIndicator* led)     { m_led_indicator = led; }
    void set_buzzer(Buzzer* buzzer)               { m_buzzer = buzzer; }
    void set_create_session(WalletCreateSession* s) { m_create = s; }
    void set_recovery(RecoveryController* r) { m_rc = r; }

private:
    Display& m_display;
    UIScreen m_current_screen{UIScreen::HOME};

    // Ambient cached data
    float       m_weather_temp{-999.0f};
    uint8_t     m_weather_code{255}; // WMO weathercode, 255 = unknown
    float       m_sol_price{-1.0f};
    float       m_sol_high_24h{-1.0f};
    float       m_sol_low_24h{-1.0f};
    float       m_sol_change_pct{0.0f};
    std::string m_weather_city{"--"};
    // Pending confirmation (TX_CONFIRM), built by WalletManager from the
    // parsed message bytes.
    Events::TxSummary m_confirm{};

    // Transaction result (for TX_SUCCESS / TX_FAIL screens)
    bool        m_tx_result_ok{false};
    char        m_tx_result_asset[8]{};
    char        m_tx_result_amount[24]{};  // exact native amount, e.g. "0.25"
    uint64_t    m_tx_result_amount_cents{0};
    char        m_tx_result_recipient[48]{};
    char        m_tx_result_msg[64]{};
    uint32_t    m_tx_result_start_ms{0};

    // Balance view
    BalanceMonitor* m_balance_monitor{nullptr};
    // Price service (non-owning; syncs cached 24h market data on entry).
    PriceService*   m_price_service{nullptr};
    // Throttle for silent on-entry price fetches (first entry always fires).
    uint32_t        m_price_req_last_ms{0};
    double          m_bal_sol{0.0};
    double          m_bal_usdc{0.0};
    bool            m_bal_fetched{false};
    bool            m_bal_ok{false};
    bool            m_bal_fetching{false};
    uint32_t        m_bal_fetch_start_ms{0};
    uint32_t        m_bal_anim_last_ms{0};

    // Transaction result RGB LED indicator
    LedIndicator*   m_led_indicator{nullptr};

    // Pomodoro countdown (POMODORO_VIEW). Buzzer is optional — ticks are
    // skipped silently when no buzzer is wired (e.g. unit tests).
    Buzzer*         m_buzzer{nullptr};
    BuzzerPattern   m_buzz{};
    PomodoroTimer   m_pomo{};
    uint32_t        m_pomo_last_sec{UINT32_MAX}; // per-second redraw tracking
    // B3/B4 hold-to-repeat state (PRESS starts, RELEASE stops).
    bool            m_pomo_holding{false};
    ButtonId        m_pomo_hold_id{ButtonId::B3_PREV};
    uint32_t        m_pomo_hold_start_ms{0};
    uint32_t        m_pomo_next_repeat_ms{0};

    uint32_t    m_last_idle_cycle_ms{0};

    // Redraw gating: the run loop only renders when the redraw epoch advanced
    // (screen/data/button change from any task) or a time-based animation asks
    // for a frame. The counter is monotonic so cross-task requests are never lost.
    std::atomic<uint32_t> m_redraw_epoch{1};
    uint32_t              m_last_rendered_epoch{0};

    void request_redraw() { m_redraw_epoch.fetch_add(1, std::memory_order_relaxed); }

    // TX confirmation state — accept is deferred until a clean single tap is
    // confirmed (release without a double/long press following within the window).
    bool        m_tx_pending_accept{false};
    uint32_t    m_tx_accept_deadline_ms{0};
    uint32_t    m_tx_press_start_ms{0};

    // Setup wizard
    bool        m_setup_needed{false};
    bool        m_location_missing{false}; // still missing (setup wizard)
    bool        m_wifi_missing{false};
    bool        m_wallet_missing{false};
    bool        m_wifi_connecting{false};  // credentials sent, waiting for IP
    void        advance_setup();           // show the first missing step
    SetupStage  m_setup_stage{SetupStage::WIFI_PROMPT};
    std::string m_connecting_ssid{};    // SSID being connected to (shown on OLED)
    std::string m_wallet_address{};     // Cached after wallet created/imported
    std::string m_location_city{};      // Cached city after location set
    uint32_t    m_connecting_dots_ms{0};
    uint8_t     m_connecting_dots{0};
    uint8_t     m_menu_index{0};
    // Horizontal carousel slide state (MENU_MAIN): previous index + animation
    // start so the new icon visibly enters from the right (B4) or left (B3).
    int8_t      m_menu_prev_index{-1};
    uint32_t    m_menu_slide_start_ms{0};
    int8_t      m_menu_slide_dir{1};
    // Wallet Info hub sub-tab: View Balance (0), Receive QR (1), SOL Price (2).
    // B3/B4 flips the tab, B2 opens it, B1 goes back.
    uint8_t     m_wallet_tab{0};
    // Menu cursor animation throttle (full flush ~115ms @8MHz, so ~4fps max).
    uint32_t    m_menu_anim_last_ms{0};

    // BLE HID remote (HID_REMOTE). Untrusted: media keys only, no wallet access.
    // BT stack starts on screen entry, fully deinit on exit (see set_screen).
    bool m_hid_started{false};
    bool m_hid_connected{false};
    uint8_t m_hid_index{0};
    // Hold-B2-to-repeat state (PRESS arms, RELEASE disarms; see hid_tick).
    bool m_hid_repeating{false};
    uint8_t m_hid_repeat_row{0};
    uint32_t m_hid_repeat_next_ms{0};

    // HOME screen player (Pass_design bg + clock + animated Yeti overlay).
    SpritePlayer m_home_yeti;
    // DND mood: when true the home overlay plays the DND Yeti instead of idle.
    bool        m_dnd_mode{false};
    // Picks the home clip from events (money received → Happy) and DND.
    Behaviour   m_behaviour;
    Mood        m_home_mood{Mood::Idle}; // which anim the player currently holds
    bool        m_home_started{false};
    bool        m_home_chrome{false};
    uint8_t     m_home_last_frame{255};
    int         m_home_last_minute{-2};
    // Last frosted clock pill rect (for union-restore when it changes size).
    int         m_home_tx{0}, m_home_ty{0}, m_home_tw{0}, m_home_th{0};
    // Last home weather readout (change detection for pill updates).
    char        m_home_wbuf[16]{"-- C"};
    char        m_home_dbuf[16]{};
    // WiFi has an IP (WIFI_GOT_IP / WIFI_DISCONNECTED). HOME shows
    // "No WiFi" instead of empty time/weather while it is down.
    bool        m_wifi_up{false};
    uint8_t     m_home_wcode{255};
    // Worn marketplace items, drawn around the home Yeti (idle mood only).
    WearRenderer m_wear;
    // Screen box the home Yeti + items covered last frame (restore/flush).
    int         m_home_yx{0}, m_home_yy{0}, m_home_yw{0}, m_home_yh{0};
    void        home_yeti_rect(uint8_t frame, int& x, int& y, int& w, int& h) const;
    void        draw_home_yeti(uint8_t frame);
    // Scrambled-grid recovery: drawn straight from the shared controller.
    RecoveryController* m_rc{nullptr};
    uint32_t    m_recovery_ms{0};          // result timer
    uint32_t    m_recovery_drawn{0};       // layout id in the framebuffer
    void render_recovery();
    void render_network_confirm();
    void render_restore_confirm();
    // Create-wallet session (shared with UsbProtocol).
    WalletCreateSession* m_create{nullptr};
    uint32_t    m_create_result_ms{0};
    void render_wallet_create();

    void render_clock();
    void render_weather();
    void render_price();
    void render_message();
    void render_menu();
    void render_wallet_info();
    void render_wallet_qr();
    void render_tx_confirm();
    void render_export_confirm();
    void render_tx_result();
    void render_balance();
    void render_pomodoro();
    void render_hid();
    void render_badge();
    void render_fair_pass();
    void render_home();
    void render_setup();

    void cycle_idle_screen();
    void approve_transaction();
    void reject_transaction();
    void go_back();
    void handle_pomodoro_button(const ButtonState& btn);
    void handle_hid_button(const ButtonState& btn);
    void hid_send_row(uint8_t row);
    void hid_tick(uint32_t now_ms);
    void pomo_tick(uint32_t now_ms);
    void open_menu_index(uint8_t index);
    void open_wallet_tab(uint8_t tab);
    void step_wallet_tab(int8_t dir);
    void step_menu(int8_t dir);

    static constexpr const char* TAG = "UIManager";
};

} // namespace Fuchey

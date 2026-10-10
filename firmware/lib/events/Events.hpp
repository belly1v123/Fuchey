#pragma once
// ============================================================
// Fuchey — Events.hpp
// All FreeRTOS event definitions, queue handles, and event
// group bits. Subsystems post here; others subscribe.
// ============================================================

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <cstdint>
#include <array>
#include <atomic>

namespace Fuchey {
namespace Events {

// ─── Event Types ──────────────────────────────────────────
enum class EventType : uint32_t {
    // WiFi
    WIFI_CONNECTED       = 0x0001,
    WIFI_DISCONNECTED    = 0x0002,
    WIFI_GOT_IP          = 0x0003,
    WIFI_CONNECT_FAILED  = 0x0004,

    // Wallet
    WALLET_CREATED       = 0x0010,
    WALLET_IMPORTED      = 0x0011,
    WALLET_LOCKED        = 0x0012,
    WALLET_UNLOCKED      = 0x0013,

    // Transactions
    TX_REQUEST           = 0x0020,
    TX_APPROVED          = 0x0021,
    TX_REJECTED          = 0x0022,
    TX_SIGNED            = 0x0023,
    TX_BROADCAST_OK      = 0x0024,
    TX_BROADCAST_FAIL    = 0x0025,

    // Ambient
    WEATHER_UPDATED      = 0x0040,
    PRICE_UPDATED        = 0x0041,
    BALANCE_UPDATED      = 0x0042,
    FUNDS_RECEIVED       = 0x0043,  // IncomingWatcher: SOL/USDC balance went up

    // UI
    UI_BUTTON_CONFIRM    = 0x0050,
    UI_BUTTON_BACK       = 0x0051,
    UI_BUTTON_LONG_PRESS = 0x0052,
    UI_IDLE_TICK         = 0x0053,
    UI_SCREEN_CHANGE     = 0x0054,
    UI_SHOW_ADDRESS      = 0x0055,  // Companion app: open the Receive QR screen
    UI_RECOVERY_VIEW     = 0x0056,  // Recovery state changed (UI reads RecoveryController)
    UI_WALLET_CREATE     = 0x0057,  // Create-wallet session changed (redraw / open)

    // System
    SYSTEM_BOOT_DONE     = 0x0060,
    SYSTEM_LOW_MEMORY    = 0x0061,
};

// ─── Confirmation request (TX_REQUEST payload) ────────────
// Built by WalletManager from the exact bytes it is about to sign,
// pre-formatted so the UI only prints strings (no parsing on the UI side).
enum class ConfirmKind : uint8_t {
    TRANSFER   = 0,  // Sign a parsed transfer (SOL / USDC)
    EXPORT_KEY = 1,  // Debug: print the private key to the serial log
    NETWORK_SWITCH = 2,  // Companion app asks to switch devnet/mainnet (mainnet = target)
    RESTORE_WALLET = 3,  // Save a wallet restored via the grid (recipient = its address)
};

struct TxSummary {
    uint32_t    request_id;     // Echoed back in TX_APPROVED / TX_REJECTED
    ConfirmKind kind;
    bool        mainnet;        // false = devnet
    char        asset[8];       // "SOL", "USDC"
    char        amount[24];     // Exact decimal amount, e.g. "0.25"
    char        fee[16];        // Network fee in SOL, e.g. "0.000005"
    char        recipient[48];  // Base58 destination (wallet or token account)
    // USDC to a recipient without a USDC account: the transaction also opens
    // one (this wallet pays the rent). `recipient` is then the owner wallet.
    bool        creates_account;
    char        rent[16];       // Rent in SOL, e.g. "0.00203928"
    // Display-only USD values (micro-dollars). Not part of what is signed.
    uint64_t    usd_micro;      // Transfer value, 0 = unknown
    uint64_t    fee_usd_micro;  // Network fee value, 0 = unknown
    uint32_t    sol_usd_cents;  // SOL/USD rate used, 0 = no price available
    bool        price_live;     // true = fetched just before this request
};

// ─── Generic Event Payload ────────────────────────────────
// Keep small to fit comfortably in queues without heap allocation
struct Event {
    EventType type;
    union {
        // TX_BROADCAST_OK / TX_BROADCAST_FAIL (tx_data holds a result string)
        struct {
            uint8_t  tx_data[256];
            uint16_t tx_len;
            uint64_t amount_cents;   // Amount in cents (USD)
        } tx;

        // TX_REQUEST (to UI); request_id is also used by TX_APPROVED /
        // TX_REJECTED (data.u32) on g_tx_confirm_queue and g_ui_queue.
        TxSummary confirm;

        // WEATHER_UPDATED
        struct {
            float    temp_celsius;
            float    wind_speed_kmh;
            uint8_t  weather_code;
            char     city[32];
        } weather;

        // PRICE_UPDATED
        struct {
            float    sol_usd;
            float    high_24h;
            float    low_24h;
            float    change_pct_24h;
        } price;

        // BALANCE_UPDATED (posted by the balance fetch worker)
        struct {
            double   sol;
            double   usdc;
            bool     ok;
        } balance;

        // FUNDS_RECEIVED (amounts that arrived since the previous check)
        struct {
            double   sol;
            double   usdc;
        } funds;

        // UI
        struct {
            uint32_t screen_id;
        } ui;

        // Generic u32 data
        uint32_t u32;
        uint8_t  raw[256];
    } data;
};

// ─── Global Queue Handles (defined in main.cpp) ───────────
// Extern declarations — initialized in main before tasks start
extern QueueHandle_t g_wallet_queue;   // Event → WalletManager
extern QueueHandle_t g_ui_queue;       // Event → UIManager
extern QueueHandle_t g_button_queue;   // ButtonDriver → consumers
// UI → WalletManager: TX_APPROVED / TX_REJECTED for the pending request is
// declared as Fuchey::g_tx_confirm_queue below (defined in main.cpp).

// ─── Event Group Bits ─────────────────────────────────────
// Global event group for fast cross-task signaling
extern EventGroupHandle_t g_event_group;

// Bit positions in g_event_group
inline constexpr EventBits_t BIT_WIFI_CONNECTED   = BIT0;
inline constexpr EventBits_t BIT_WIFI_IP          = BIT1;
inline constexpr EventBits_t BIT_WALLET_READY     = BIT2;
inline constexpr EventBits_t BIT_WALLET_LOCKED    = BIT3;
inline constexpr EventBits_t BIT_TX_PENDING       = BIT4;
inline constexpr EventBits_t BIT_WEATHER_OK       = BIT6;
inline constexpr EventBits_t BIT_PRICE_OK         = BIT7;
// Set by the UI to wake the PriceService task for an immediate fetch
// (e.g. entering the SOL Price screen). Cleared on wake by the waiter.
inline constexpr EventBits_t BIT_PRICE_FETCH_REQ  = BIT8;

// ─── Helper: Post event to a queue (non-blocking) ─────────
inline bool post(QueueHandle_t q, const Event& evt, TickType_t wait = 0) {
    if (q == nullptr) return false;
    return xQueueSend(q, &evt, wait) == pdTRUE;
}

// ─── Helper: Post event from ISR ──────────────────────────
inline bool postFromISR(QueueHandle_t q, const Event& evt) {
    if (q == nullptr) return false;
    BaseType_t woken = pdFALSE;
    bool ok = xQueueSendFromISR(q, &evt, &woken) == pdTRUE;
    portYIELD_FROM_ISR(woken);
    return ok;
}

} // namespace Events

// UI → WalletManager approval channel (defined in main.cpp)
extern QueueHandle_t g_tx_confirm_queue;

} // namespace Fuchey

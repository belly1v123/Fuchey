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

namespace Fuchey {
namespace Events {

// ─── Event Types ──────────────────────────────────────────
enum class EventType : uint32_t {
    // WiFi
    WIFI_CONNECTED       = 0x0001,
    WIFI_DISCONNECTED    = 0x0002,
    WIFI_GOT_IP          = 0x0003,

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

    // AI / Chat
    AI_MESSAGE_RECV      = 0x0030,
    AI_RESPONSE_READY    = 0x0031,
    AI_PAYMENT_INTENT    = 0x0032,

    // Ambient
    WEATHER_UPDATED      = 0x0040,
    PRICE_UPDATED        = 0x0041,

    // UI
    UI_BUTTON_CONFIRM    = 0x0050,
    UI_BUTTON_BACK       = 0x0051,
    UI_BUTTON_LONG_PRESS = 0x0052,
    UI_IDLE_TICK         = 0x0053,
    UI_SCREEN_CHANGE     = 0x0054,

    // System
    SYSTEM_BOOT_DONE     = 0x0060,
    SYSTEM_LOW_MEMORY    = 0x0061,
};

// ─── Generic Event Payload ────────────────────────────────
// Keep small to fit comfortably in queues without heap allocation
struct Event {
    EventType type;
    union {
        // TX_REQUEST, TX_SIGNED
        struct {
            uint8_t  tx_data[256];
            uint16_t tx_len;
            uint64_t amount_cents;   // Amount in cents (USD)
        } tx;

        // AI_MESSAGE_RECV
        struct {
            char     text[128];
            bool     is_payment_intent;
            uint64_t payment_amount_cents;
        } chat;

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
        } price;

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
extern QueueHandle_t g_ai_queue;       // Event → AIManager
extern QueueHandle_t g_button_queue;   // ButtonDriver → consumers

// ─── Event Group Bits ─────────────────────────────────────
// Global event group for fast cross-task signaling
extern EventGroupHandle_t g_event_group;

// Bit positions in g_event_group
inline constexpr EventBits_t BIT_WIFI_CONNECTED   = BIT0;
inline constexpr EventBits_t BIT_WIFI_IP          = BIT1;
inline constexpr EventBits_t BIT_WALLET_READY     = BIT2;
inline constexpr EventBits_t BIT_WALLET_LOCKED    = BIT3;
inline constexpr EventBits_t BIT_TX_PENDING       = BIT4;
inline constexpr EventBits_t BIT_AI_BUSY          = BIT5;
inline constexpr EventBits_t BIT_WEATHER_OK       = BIT6;
inline constexpr EventBits_t BIT_PRICE_OK         = BIT7;

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
} // namespace Fuchey

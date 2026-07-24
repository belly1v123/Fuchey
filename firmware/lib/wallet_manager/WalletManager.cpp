// ============================================================
// Fuchey — WalletManager.cpp
// ============================================================

#include "WalletManager.hpp"
#include "../config/Config.hpp"
#include "../buttons/ButtonDriver.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <cstring>

// Forward-declare the global queue handle defined in main.cpp
extern QueueHandle_t g_button_queue_ref;

namespace Fuchey {

static constexpr const char* TAG = "WalletManager";

WalletManager::WalletManager(WalletCore& core, SpendingPolicy& policy)
    : m_core(core), m_policy(policy) {}

// ─── Init ─────────────────────────────────────────────────
bool WalletManager::init() {
    // Session timer: auto-lock after SESSION_TIMEOUT_MS
    m_session_timer = xTimerCreate(
        "session_timeout",
        pdMS_TO_TICKS(Timing::SESSION_TIMEOUT_MS),
        pdFALSE,  // one-shot
        static_cast<void*>(this),
        [](TimerHandle_t t) {
            auto* self = static_cast<WalletManager*>(pvTimerGetTimerID(t));
            self->on_session_timeout();
        }
    );

    if (!m_session_timer) {
        ESP_LOGE(TAG, "Failed to create session timer");
        return false;
    }

    ESP_LOGI(TAG, "WalletManager initialized (timeout=%lu ms)", Timing::SESSION_TIMEOUT_MS);
    return true;
}

// ─── Session timer ────────────────────────────────────────
void WalletManager::start_session_timer() {
    if (m_session_timer) xTimerStart(m_session_timer, 0);
}

void WalletManager::reset_session_timer() {
    if (m_session_timer) xTimerReset(m_session_timer, 0);
}

void WalletManager::on_session_timeout() {
    ESP_LOGI(TAG, "Session timeout — locking wallet");
    lock();

    // Notify UI
    Events::Event evt{};
    evt.type = Events::EventType::WALLET_LOCKED;
    Events::post(Events::g_ui_queue, evt);
}

// ─── Wallet lifecycle ─────────────────────────────────────
WalletResult WalletManager::create_wallet(int words, std::string& out_mnemonic) {
    auto result = m_core.create(words, out_mnemonic);
    if (result == WalletResult::OK) {
        start_session_timer();

        Events::Event evt{};
        evt.type = Events::EventType::WALLET_CREATED;
        Events::post(Events::g_ui_queue, evt);
        ESP_LOGI(TAG, "Wallet created — session started");
    }
    return result;
}

WalletResult WalletManager::import_wallet(std::string_view mnemonic) {
    auto result = m_core.import(mnemonic);
    if (result == WalletResult::OK) {
        start_session_timer();

        Events::Event evt{};
        evt.type = Events::EventType::WALLET_IMPORTED;
        Events::post(Events::g_ui_queue, evt);
    }
    return result;
}

void WalletManager::lock() {
    m_core.lock();
    if (m_session_timer) xTimerStop(m_session_timer, 0);

    if (Events::g_event_group) {
        xEventGroupSetBits(Events::g_event_group, Events::BIT_WALLET_LOCKED);
        xEventGroupClearBits(Events::g_event_group, Events::BIT_WALLET_READY);
    }
}

WalletResult WalletManager::unlock() {
    auto result = m_core.unlock();
    if (result == WalletResult::OK) {
        reset_session_timer();
        if (Events::g_event_group) {
            xEventGroupSetBits(Events::g_event_group, Events::BIT_WALLET_READY);
            xEventGroupClearBits(Events::g_event_group, Events::BIT_WALLET_LOCKED);
        }
    }
    return result;
}

// ─── Request signature (CRITICAL PATH) ───────────────────
TxResult WalletManager::request_signature(const TxRequest& req,
                                           bool force_confirm) {
    TxResult result{.approved = false, .signature{}, .error = WalletResult::ERR_LOCKED};

    if (!m_core.is_unlocked()) {
        ESP_LOGE(TAG, "Cannot sign — wallet locked");
        return result;
    }

    // Evaluate spending policy
    auto decision = m_policy.evaluate(req.amount_cents);
    bool need_confirmation = force_confirm ||
                             (decision == PolicyDecision::REQUIRE_CONFIRMATION);

    if (need_confirmation) {
        ESP_LOGI(TAG, "Transaction requires confirmation: %s, $%.2f",
                 req.description,
                 static_cast<double>(req.amount_cents) / 100.0);

        // Post TX_REQUEST event to UI — it will display the transaction
        Events::Event ui_evt{};
        ui_evt.type = Events::EventType::TX_REQUEST;
        std::memcpy(ui_evt.data.tx.tx_data, req.data,
                    std::min<size_t>(req.data_len, sizeof(ui_evt.data.tx.tx_data)));
        ui_evt.data.tx.tx_len        = req.data_len;
        ui_evt.data.tx.amount_cents  = req.amount_cents;
        Events::post(Events::g_ui_queue, ui_evt);

        // Block waiting for button confirmation
        bool confirmed = wait_for_confirmation(req);
        if (!confirmed) {
            ESP_LOGI(TAG, "Transaction rejected by user");
            Events::Event rej{};
            rej.type = Events::EventType::TX_REJECTED;
            Events::post(Events::g_ui_queue, rej);
            result.error = WalletResult::ERR_LOCKED; // Reuse for "rejected"
            return result;
        }
    } else {
        ESP_LOGI(TAG, "Auto-signing transaction ($%.2f <= limit %s)",
                 static_cast<double>(req.amount_cents) / 100.0,
                 m_policy.limit_to_string());
    }

    // === SIGN ===
    Crypto::Signature sig{};
    auto sign_result = m_core.sign(
        std::span<const uint8_t>(req.data, req.data_len),
        sig
    );

    if (sign_result != WalletResult::OK) {
        ESP_LOGE(TAG, "Signing failed");
        result.error = sign_result;
        return result;
    }

    // Reset session timer on activity
    reset_session_timer();

    // Post TX_SIGNED event
    Events::Event signed_evt{};
    signed_evt.type = Events::EventType::TX_SIGNED;
    Events::post(Events::g_ui_queue, signed_evt);

    result.approved  = true;
    result.signature = sig;
    result.error     = WalletResult::OK;

    ESP_LOGI(TAG, "Transaction signed successfully");
    return result;
}

// ─── wait_for_confirmation ────────────────────────────────
bool WalletManager::wait_for_confirmation(const TxRequest& req,
                                           uint32_t timeout_ms) {
    m_waiting_confirmation = true;

    // Set event group bit: UI knows we're waiting
    if (Events::g_event_group) {
        xEventGroupSetBits(Events::g_event_group, Events::BIT_TX_PENDING);
    }

    ButtonState btn{};
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        TickType_t remaining = deadline - xTaskGetTickCount();
        if (::g_button_queue_ref &&
            xQueueReceive(::g_button_queue_ref, &btn, remaining) == pdTRUE) {

            if (btn.event != ButtonEvent::PRESS) continue;

            if (Events::g_event_group) {
                xEventGroupClearBits(Events::g_event_group, Events::BIT_TX_PENDING);
            }
            m_waiting_confirmation = false;

            return (btn.id == ButtonId::CONFIRM);
        }
    }

    // Timeout
    ESP_LOGW(TAG, "Confirmation timeout — rejecting transaction");
    if (Events::g_event_group) {
        xEventGroupClearBits(Events::g_event_group, Events::BIT_TX_PENDING);
    }
    m_waiting_confirmation = false;
    return false;
}

// ─── Policy ───────────────────────────────────────────────
bool WalletManager::set_spend_limit(SpendLimit limit) {
    return m_policy.set_limit(limit);
}

SpendLimit WalletManager::get_spend_limit() const {
    return m_policy.get_limit();
}

// ─── FreeRTOS task ────────────────────────────────────────
void WalletManager::task_entry(void* arg) {
    static_cast<WalletManager*>(arg)->run();
}

void WalletManager::run() {
    ESP_LOGI(TAG, "WalletManager task started");
    Events::Event evt{};
    while (true) {
        // Wait for wallet events
        if (Events::g_wallet_queue &&
            xQueueReceive(Events::g_wallet_queue, &evt,
                         pdMS_TO_TICKS(1000)) == pdTRUE) {

            switch (evt.type) {
                case Events::EventType::TX_REQUEST: {
                    // Reconstruct TxRequest from event
                    TxRequest req{};
                    std::memcpy(req.data, evt.data.tx.tx_data,
                                std::min<size_t>(evt.data.tx.tx_len, sizeof(req.data)));
                    req.data_len     = evt.data.tx.tx_len;
                    req.amount_cents = evt.data.tx.amount_cents;
                    request_signature(req);
                    break;
                }
                default:
                    break;
            }
        }
        // Session timer runs independently via FreeRTOS timer callback
    }
}

} // namespace Fuchey

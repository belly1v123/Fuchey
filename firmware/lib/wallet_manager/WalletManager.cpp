// ============================================================
// Fuchey — WalletManager.cpp
// ============================================================

#include "WalletManager.hpp"
#include "../config/Config.hpp"
#include "../crypto/Base58.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

namespace Fuchey {

static constexpr const char* TAG = "WalletManager";

WalletManager::WalletManager(WalletCore& core)
    : m_core(core) {}

// ─── Init ─────────────────────────────────────────────────
bool WalletManager::init() {
    m_confirm_mutex = xSemaphoreCreateMutex();
    if (!m_confirm_mutex) {
        ESP_LOGE(TAG, "Failed to create confirmation mutex");
        return false;
    }
    ESP_LOGI(TAG, "WalletManager initialized (every signature needs a physical B1 tap)");
    return true;
}

// ─── Physical confirmation ────────────────────────────────
SignStatus WalletManager::request_confirmation(Events::TxSummary& summary,
                                               uint32_t timeout_ms) {
    if (!g_tx_confirm_queue || !Events::g_ui_queue || !m_confirm_mutex) {
        return SignStatus::SIGN_FAILED;
    }
    if (xSemaphoreTake(m_confirm_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Confirmation already pending — refusing new request");
        return SignStatus::BUSY;
    }

    summary.request_id = m_next_request_id++;
    if (m_next_request_id == 0) m_next_request_id = 1;  // 0 means "none pending"
    m_cancel_requested.store(false);
    m_pending_id.store(summary.request_id);

    // Drop stale decisions from earlier (timed-out) requests.
    Events::Event reply{};
    while (xQueueReceive(g_tx_confirm_queue, &reply, 0) == pdTRUE) {}

    Events::Event req{};
    req.type = Events::EventType::TX_REQUEST;
    req.data.confirm = summary;
    if (!Events::post(Events::g_ui_queue, req, pdMS_TO_TICKS(200))) {
        ESP_LOGE(TAG, "UI queue full — cannot show confirmation");
        m_pending_id.store(0);
        xSemaphoreGive(m_confirm_mutex);
        return SignStatus::SIGN_FAILED;
    }

    if (Events::g_event_group) {
        xEventGroupSetBits(Events::g_event_group, Events::BIT_TX_PENDING);
    }

    SignStatus status = SignStatus::TIMED_OUT;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (static_cast<int32_t>(deadline - now) <= 0) break;
        if (xQueueReceive(g_tx_confirm_queue, &reply, deadline - now) != pdTRUE) break;
        if (reply.data.u32 != summary.request_id) continue;  // stale / foreign

        if (reply.type == Events::EventType::TX_APPROVED) {
            status = SignStatus::APPROVED;
            break;
        }
        if (reply.type == Events::EventType::TX_REJECTED) {
            status = m_cancel_requested.exchange(false) ? SignStatus::CANCELLED
                                                        : SignStatus::REJECTED;
            break;
        }
    }

    if (status == SignStatus::TIMED_OUT) {
        // Tell the UI to drop the confirm screen for this request.
        ESP_LOGW(TAG, "Confirmation #%lu timed out", static_cast<unsigned long>(summary.request_id));
        Events::Event cancel{};
        cancel.type = Events::EventType::TX_REJECTED;
        cancel.data.u32 = summary.request_id;
        Events::post(Events::g_ui_queue, cancel, pdMS_TO_TICKS(200));
    }

    if (Events::g_event_group) {
        xEventGroupClearBits(Events::g_event_group, Events::BIT_TX_PENDING);
    }
    m_pending_id.store(0);
    xSemaphoreGive(m_confirm_mutex);
    return status;
}

bool WalletManager::cancel_pending() {
    const uint32_t id = m_pending_id.load();
    if (id == 0 || !g_tx_confirm_queue) return false;

    m_cancel_requested.store(true);
    Events::Event evt{};
    evt.type = Events::EventType::TX_REJECTED;
    evt.data.u32 = id;
    Events::post(g_tx_confirm_queue, evt, pdMS_TO_TICKS(50));  // unblocks the waiter
    Events::post(Events::g_ui_queue, evt, pdMS_TO_TICKS(50));  // closes TX_CONFIRM
    ESP_LOGW(TAG, "Confirmation #%lu cancelled by requester", static_cast<unsigned long>(id));
    return true;
}

// ─── Sign transaction (CRITICAL PATH) ─────────────────────
SignResult WalletManager::sign_transaction(std::span<const uint8_t> message,
                                           const SignContext& ctx) {
    SignResult result{};

    auto pubkey = m_core.get_pubkey();
    if (!pubkey) {
        result.status = SignStatus::NO_WALLET;
        return result;
    }

    // 1. Parse the exact bytes that will be signed.
    result.parse_error = TxParser::parse_transfer(message, *pubkey, result.parsed);
    if (result.parse_error != TxParser::ParseError::OK) {
        ESP_LOGE(TAG, "Refusing to sign: %s", TxParser::error_to_string(result.parse_error));
        result.status = SignStatus::UNSUPPORTED_TX;
        return result;
    }
    const auto& p = result.parsed;
    if (p.asset == TxParser::Asset::USDC && p.mint_is_mainnet != ctx.mainnet) {
        ESP_LOGE(TAG, "Refusing to sign: USDC mint is for %s but device is on %s",
                 p.mint_is_mainnet ? "mainnet" : "devnet",
                 ctx.mainnet ? "mainnet" : "devnet");
        result.status = SignStatus::NETWORK_MISMATCH;
        return result;
    }

    // 2. Build the on-screen summary from the parsed message.
    Events::TxSummary summary{};
    summary.kind    = Events::ConfirmKind::TRANSFER;
    summary.mainnet = ctx.mainnet;
    snprintf(summary.asset, sizeof(summary.asset), "%s",
             p.asset == TxParser::Asset::SOL ? "SOL" : "USDC");
    TxParser::format_units(p.amount, p.decimals, summary.amount, sizeof(summary.amount));
    TxParser::format_units(p.fee_lamports, 9, summary.fee, sizeof(summary.fee));
    // With an ATA create the parser has verified destination == ATA(owner),
    // so show the owner wallet — what the user actually typed and recognises.
    const Crypto::PubKey& shown = p.creates_token_account ? p.owner : p.destination;
    std::string dest = Crypto::Base58::pubkey_to_address(
        std::span<const uint8_t, 32>(shown.data(), 32));
    snprintf(summary.recipient, sizeof(summary.recipient), "%s", dest.c_str());
    summary.creates_account = p.creates_token_account;
    if (p.creates_token_account) {
        TxParser::format_units(p.rent_lamports, 9, summary.rent, sizeof(summary.rent));
    }
    // Display-only USD values. 1 lamport × (USD/SOL) = rate / 1000 micro-USD.
    if (ctx.sol_usd > 0.0f) {
        const double rate = static_cast<double>(ctx.sol_usd);
        summary.sol_usd_cents = static_cast<uint32_t>(rate * 100.0 + 0.5);
        summary.fee_usd_micro = static_cast<uint64_t>(
            static_cast<double>(p.fee_lamports) * rate / 1000.0 + 0.5);
        if (p.asset == TxParser::Asset::SOL) {
            summary.usd_micro = static_cast<uint64_t>(
                static_cast<double>(p.amount) * rate / 1000.0 + 0.5);
        }
        summary.price_live = ctx.price_live;
    }
    if (p.asset == TxParser::Asset::USDC) {
        summary.usd_micro = p.amount;  // 6 decimals == micro-dollars
    }

    ESP_LOGI(TAG, "Confirm: %s %s -> %s (fee %s SOL, %s)%s",
             summary.amount, summary.asset, summary.recipient, summary.fee,
             ctx.mainnet ? "MAINNET" : "devnet",
             summary.creates_account ? " + creates USDC account" : "");

    // 3. Physical confirmation.
    SignStatus decision = request_confirmation(summary);
    if (decision != SignStatus::APPROVED) {
        ESP_LOGW(TAG, "Not signing: %s", status_to_string(decision));
        result.status = decision;
        return result;
    }

    // 4. Sign the same bytes that were parsed and shown.
    if (!m_core.is_unlocked() && m_core.unlock() != WalletResult::OK) {
        result.status = SignStatus::SIGN_FAILED;
        return result;
    }
    if (m_core.sign(message, result.signature) != WalletResult::OK) {
        ESP_LOGE(TAG, "Signing failed");
        result.status = SignStatus::SIGN_FAILED;
        return result;
    }

    Events::Event signed_evt{};
    signed_evt.type = Events::EventType::TX_SIGNED;
    signed_evt.data.u32 = summary.request_id;
    Events::post(Events::g_wallet_queue, signed_evt);

    ESP_LOGI(TAG, "Transaction #%lu signed", static_cast<unsigned long>(summary.request_id));
    result.status = SignStatus::SIGNED;
    return result;
}

const char* WalletManager::status_to_string(SignStatus s) {
    switch (s) {
        case SignStatus::SIGNED:           return "signed";
        case SignStatus::APPROVED:         return "approved";
        case SignStatus::REJECTED:         return "rejected by user";
        case SignStatus::CANCELLED:        return "cancelled";
        case SignStatus::TIMED_OUT:        return "confirmation timed out";
        case SignStatus::UNSUPPORTED_TX:   return "unsupported transaction";
        case SignStatus::NETWORK_MISMATCH: return "network mismatch";
        case SignStatus::BUSY:             return "another request pending";
        case SignStatus::NO_WALLET:        return "no wallet";
        case SignStatus::SIGN_FAILED:      return "signing failed";
    }
    return "unknown";
}

// ─── FreeRTOS task ────────────────────────────────────────
void WalletManager::task_entry(void* arg) {
    static_cast<WalletManager*>(arg)->run();
}

void WalletManager::run() {
    ESP_LOGI(TAG, "WalletManager task started");
    Events::Event evt{};
    while (true) {
        // Signing runs in the caller's task (sign_transaction); this loop
        // only drains lifecycle notifications so g_wallet_queue never fills.
        if (!Events::g_wallet_queue) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (xQueueReceive(Events::g_wallet_queue, &evt, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Wallet event 0x%04lx", static_cast<unsigned long>(evt.type));
        }
    }
}

} // namespace Fuchey

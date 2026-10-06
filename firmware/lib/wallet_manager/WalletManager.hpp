#pragma once
// ============================================================
// Fuchey — WalletManager.hpp
// The ONLY module permitted to invoke WalletCore::sign().
//
// Signing flow (sign_transaction):
//   1. Parse the exact message bytes (TxParser) — reject anything that
//      is not a single SOL / USDC transfer paid by this wallet.
//   2. Show the parsed details on the TFT (TX_REQUEST → UIManager).
//   3. Block until the user physically taps B1 (approve) or
//      double/long-presses B1 (reject); console-injected buttons can
//      never approve. Timeout = reject.
//   4. Sign those same bytes.
//
// Every signature requires physical confirmation (no auto-sign).
// ============================================================

#include "../wallet/WalletCore.hpp"
#include "../events/Events.hpp"
#include "TxParser.hpp"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <optional>

namespace Fuchey {

// ─── Signing context (supplied by the caller) ─────────────
struct SignContext {
    bool  mainnet{false};  // Network the caller will broadcast to
    float sol_usd{0.0f};   // Display-only SOL/USD rate (<= 0: unknown)
    bool  price_live{false}; // rate was fetched right before this request
};

// ─── Signing result ───────────────────────────────────────
enum class SignStatus : uint8_t {
    SIGNED,
    APPROVED,         // request_confirmation only: user approved on hardware
    REJECTED,         // User rejected (B1 double/long press)
    CANCELLED,        // Requester withdrew the request (cancel_pending)
    TIMED_OUT,        // No decision within the confirmation window
    UNSUPPORTED_TX,   // TxParser refused the message
    NETWORK_MISMATCH, // USDC mint does not match the selected network
    BUSY,             // Another confirmation is already pending
    NO_WALLET,
    SIGN_FAILED,
};

struct SignResult {
    SignStatus             status{SignStatus::SIGN_FAILED};
    Crypto::Signature      signature{};
    TxParser::ParseError   parse_error{TxParser::ParseError::OK};
    TxParser::ParsedTransfer parsed{};
};

class WalletManager {
public:
    static constexpr uint32_t CONFIRM_TIMEOUT_MS = 30000;

    explicit WalletManager(WalletCore& core);

    // Non-copyable
    WalletManager(const WalletManager&) = delete;
    WalletManager& operator=(const WalletManager&) = delete;

    // ── Lifecycle ────────────────────────────────────────
    bool init();

    // ── Queries ──────────────────────────────────────────
    WalletState wallet_state() const { return m_core.state(); }
    std::optional<std::string> get_address() const { return m_core.get_address(); }

    // ── Transaction signing (the critical path) ──────────
    // Blocks the calling task for up to CONFIRM_TIMEOUT_MS. Safe to call
    // from any task; only one request can be pending at a time.
    SignResult sign_transaction(std::span<const uint8_t> message,
                                const SignContext& ctx);

    // ── Generic physical confirmation ────────────────────
    // Shows `summary` on the TFT and waits for a hardware B1 decision.
    // request_id is assigned here. Used by sign_transaction and by the
    // debug wallet_export console command.
    SignStatus request_confirmation(Events::TxSummary& summary,
                                    uint32_t timeout_ms = CONFIRM_TIMEOUT_MS);

    // Withdraw the pending confirmation (e.g. companion app "cancel").
    // Closes the TFT screen; the waiting call returns CANCELLED.
    // Returns false when nothing is pending. Can never approve.
    bool cancel_pending();
    bool is_pending() const { return m_pending_id.load() != 0; }

    static const char* status_to_string(SignStatus s);

    // ── FreeRTOS task entry ───────────────────────────────
    static void task_entry(void* arg);
    void run();

private:
    WalletCore&       m_core;
    SemaphoreHandle_t m_confirm_mutex{nullptr};
    uint32_t          m_next_request_id{1};
    std::atomic<uint32_t> m_pending_id{0};
    std::atomic<bool>     m_cancel_requested{false};

    static constexpr const char* TAG = "WalletManager";
};

} // namespace Fuchey

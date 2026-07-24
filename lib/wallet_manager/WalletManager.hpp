#pragma once
// ============================================================
// Fuchey — WalletManager.hpp
// The ONLY module permitted to invoke WalletCore.sign().
//
// Responsibilities:
//   - Wallet lifecycle management
//   - Spending policy enforcement
//   - Transaction approval flow
//   - Session timeout management
//   - Event posting (WALLET_CREATED, TX_APPROVED, etc.)
//
// SECURITY: AI and UI communicate with WalletManager.
//           WalletManager communicates with WalletCore.
//           AI NEVER touches WalletCore directly.
// ============================================================

#include "../wallet/WalletCore.hpp"
#include "../policy/SpendingPolicy.hpp"
#include "../events/Events.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <optional>

namespace Fuchey {

// ─── Transaction request ──────────────────────────────────
struct TxRequest {
    uint8_t  data[256];    // Raw transaction bytes
    uint16_t data_len;
    uint64_t amount_cents; // USD value for policy check (0 = unknown → confirm)
    char     description[64]; // Human-readable (for UI display)
};

// ─── Transaction result ───────────────────────────────────
struct TxResult {
    bool                approved;
    Crypto::Signature   signature;
    WalletResult        error;
};

class WalletManager {
public:
    explicit WalletManager(WalletCore& core,
                           SpendingPolicy& policy);

    // Non-copyable
    WalletManager(const WalletManager&) = delete;
    WalletManager& operator=(const WalletManager&) = delete;

    // ── Lifecycle ────────────────────────────────────────
    bool init();
    void start_session_timer();
    void reset_session_timer();
    void on_session_timeout();

    // ── Wallet operations ────────────────────────────────
    WalletResult create_wallet(int words, std::string& out_mnemonic);
    WalletResult import_wallet(std::string_view mnemonic);
    void lock();
    WalletResult unlock();

    // ── Queries ──────────────────────────────────────────
    WalletState wallet_state() const { return m_core.state(); }
    std::optional<std::string> get_address() const { return m_core.get_address(); }

    // ── Transaction approval (the critical path) ──────────
    // This is the ONLY place signing is initiated.
    // Blocks until user confirms (if required) or auto-signs.
    // Returns immediately if auto-sign applies.
    //
    // CALL FROM: WalletManager task only.
    // NOT callable by AI directly.
    TxResult request_signature(const TxRequest& req,
                                bool force_confirm = false);

    // ── Policy management ────────────────────────────────
    bool set_spend_limit(SpendLimit limit);
    SpendLimit get_spend_limit() const;

    // ── FreeRTOS task entry ───────────────────────────────
    // Runs the wallet manager event loop.
    static void task_entry(void* arg);
    void run();

private:
    WalletCore&     m_core;
    SpendingPolicy& m_policy;
    TimerHandle_t   m_session_timer{nullptr};
    bool            m_waiting_confirmation{false};

    // ── Confirmation handling ─────────────────────────────
    // Blocks waiting for button press (CONFIRM or BACK)
    // Returns true if user confirmed, false if rejected/timeout
    bool wait_for_confirmation(const TxRequest& req,
                               uint32_t timeout_ms = 30000);

    static constexpr const char* TAG = "WalletManager";
};

} // namespace Fuchey

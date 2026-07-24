#pragma once
// ============================================================
// Fuchey — SpendingPolicy.hpp
// Configurable spending limit enforcement.
// Threshold NEVER hardcoded. Always loaded from NVS.
// ============================================================

#include <cstdint>
#include <optional>

namespace Fuchey {

// ─── Policy Decision ──────────────────────────────────────
enum class PolicyDecision {
    AUTO_SIGN,           // Under threshold — sign automatically
    REQUIRE_CONFIRMATION, // Above threshold — require button press
    DENIED,              // Policy set to OFF — always require confirmation
};

// ─── Threshold presets ────────────────────────────────────
// Stored as cents (USD * 100)
enum class SpendLimit : uint32_t {
    OFF      = 0,    // Always require confirmation
    CENTS_50 = 50,   // $0.50
    USD_1    = 100,  // $1.00
    USD_5    = 500,  // $5.00
};

class SpendingPolicy {
public:
    SpendingPolicy();

    // Load policy from NVS on init
    bool init();

    // Evaluate whether a payment requires confirmation
    // amount_cents: amount in US cents (e.g., $1.50 = 150)
    PolicyDecision evaluate(uint64_t amount_cents) const;

    // Get/Set current limit
    SpendLimit get_limit() const  { return m_limit; }
    bool set_limit(SpendLimit limit); // persists to NVS

    // Human-readable limit string for UI display
    const char* limit_to_string() const;
    static const char* limit_to_string(SpendLimit limit);

private:
    SpendLimit m_limit{SpendLimit::OFF};
    static constexpr const char* TAG = "SpendingPolicy";
};

} // namespace Fuchey

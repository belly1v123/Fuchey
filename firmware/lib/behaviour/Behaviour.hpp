#pragma once
// ============================================================
// Fuchey — Behaviour.hpp
// First piece of the behaviour engine: events in, which Yeti clip the
// HOME screen plays out. Portable C++ (no ESP-IDF), driven by ms
// timestamps, so it can also run in a desktop simulator later.
//
// Today: Idle by default, Happy for a few seconds after money arrives,
// DND (user toggle) above everything. New moods slot into home_mood().
// ============================================================

#include <cstdint>

namespace Fuchey {

enum class Mood : uint8_t { Idle, Happy, Dnd };

class Behaviour {
public:
    // How long the Happy clip plays (3 loops of 12 frames at 8 fps).
    static constexpr uint32_t kHappyMs = 4500;
    // Money that arrives while HOME is not on screen still gets its
    // celebration if the user comes back home within this window.
    static constexpr uint32_t kPendingMs = 60000;

    // SOL or USDC balance went up.
    void on_funds_received(uint32_t now_ms) {
        m_pending = true;
        m_pending_until = now_ms + kPendingMs;
    }

    // Called by HOME every frame: the mood to show right now.
    Mood home_mood(uint32_t now_ms, bool dnd) {
        if (dnd) {
            // Do-not-disturb: no celebrations, and none saved for later.
            m_pending = false;
            m_happy = false;
            return Mood::Dnd;
        }
        if (m_pending) {
            m_pending = false;
            if (before(now_ms, m_pending_until)) {
                m_happy = true;
                m_happy_until = now_ms + kHappyMs;
            }
        }
        if (m_happy && before(now_ms, m_happy_until)) return Mood::Happy;
        m_happy = false;
        return Mood::Idle;
    }

private:
    static bool before(uint32_t now_ms, uint32_t deadline_ms) {
        return static_cast<int32_t>(now_ms - deadline_ms) < 0;
    }

    bool     m_pending{false};
    uint32_t m_pending_until{0};
    bool     m_happy{false};
    uint32_t m_happy_until{0};
};

} // namespace Fuchey

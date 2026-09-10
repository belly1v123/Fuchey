#pragma once
// ============================================================
// Fuchey — Buzzer.hpp
// Active 5V buzzer (e.g. PS-HT1205 class) driven low-side via NPN:
// GPIO -> 1k -> base, emitter -> GND, collector -> buzzer(-),
// buzzer(+) -> 5V. Active-high: HIGH = sound.
// init() parks the pin LOW (silent) — a floating GPIO40 otherwise
// lets the transistor float on and the buzzer sounds continuously.
// ============================================================

#include <cstdint>

namespace Fuchey {

class Buzzer {
public:
    Buzzer() = default;
    ~Buzzer() = default;

    bool init();   // GPIO output, driven LOW (silent)
    void on();
    void off();
    void beep(uint32_t ms);  // blocking beep, for later UI hooks

private:
    static constexpr const char* TAG = "Buzzer";
};

// ─── BuzzerPattern ─────────────────────────────────────────
// Non-blocking ON/OFF beep sequencer for the ACTIVE buzzer
// (timing only — no tone frequencies possible).
// The UI task drives it via tick() using esp_timer ms timestamps;
// tick() toggles the Buzzer and returns true while sounding/scheduled.
class BuzzerPattern {
public:
    BuzzerPattern() = default;

    // Begin `beeps` x (ON on_ms / OFF off_ms), sounding immediately.
    void start(uint32_t now_ms, Buzzer& buzzer,
               uint8_t beeps = 4, uint32_t on_ms = 180, uint32_t off_ms = 180) {
        if (beeps == 0) { m_active = false; return; }
        m_active  = true;
        m_left    = beeps;
        m_on      = true;
        m_on_ms   = on_ms;
        m_off_ms  = off_ms;
        m_next_ms = now_ms + on_ms;
        buzzer.on();
    }

    bool tick(uint32_t now_ms, Buzzer& buzzer) {
        if (!m_active) return false;
        if (static_cast<int32_t>(now_ms - m_next_ms) < 0) return true;
        if (m_on) {
            buzzer.off();
            m_on = false;
            if (--m_left == 0) { m_active = false; return false; }
            m_next_ms = now_ms + m_off_ms;
        } else {
            buzzer.on();
            m_on = true;
            m_next_ms = now_ms + m_on_ms;
        }
        return true;
    }

    bool is_active() const { return m_active; }

    void stop(Buzzer& buzzer) {
        m_active = false;
        buzzer.off();
    }

private:
    bool     m_active{false};
    uint8_t  m_left{0};
    bool     m_on{false};
    uint32_t m_next_ms{0};
    uint32_t m_on_ms{180};
    uint32_t m_off_ms{180};
};

} // namespace Fuchey

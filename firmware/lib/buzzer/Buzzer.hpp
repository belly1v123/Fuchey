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

} // namespace Fuchey

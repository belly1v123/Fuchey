#pragma once
// ============================================================
// Fuchey — ButtonDriver.hpp
// GPIO interrupt-based debounced button driver.
// No polling. Debounce via FreeRTOS timer.
// Events posted to g_button_queue.
// ============================================================

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/queue.h>
#include <cstdint>
#include <array>

namespace Fuchey {

// ─── Button IDs ───────────────────────────────────────────
enum class ButtonId : uint8_t {
    CONFIRM = 0,  // OK / Select / Up
    BACK    = 1,  // Cancel / Back / Down
};

// ─── Button Event Types ───────────────────────────────────
enum class ButtonEvent : uint8_t {
    PRESS        = 0,
    RELEASE      = 1,
    LONG_PRESS   = 2,
    DOUBLE_PRESS = 3,
};

// ─── Button State ─────────────────────────────────────────
struct ButtonState {
    ButtonId    id;
    ButtonEvent event;
    uint32_t    timestamp_ms;
};

class ButtonDriver {
public:
    static constexpr int NUM_BUTTONS = 2;

    // pin_confirm, pin_back: GPIO numbers (active-low, internal pull-up)
    ButtonDriver(int pin_confirm, int pin_back,
                 uint32_t debounce_ms = 50,
                 uint32_t long_press_ms = 1000);
    ~ButtonDriver();

    // Non-copyable
    ButtonDriver(const ButtonDriver&) = delete;
    ButtonDriver& operator=(const ButtonDriver&) = delete;

    // ── Lifecycle ────────────────────────────────────────
    // Call once after queue is created
    bool init(QueueHandle_t output_queue);

    // ── Query ─────────────────────────────────────────────
    bool is_pressed(ButtonId id) const;

private:
    struct ButtonInfo {
        int          pin;
        ButtonId     id;
        bool         last_state{false};
        bool         pressed{false};
        uint32_t     press_time_ms{0};
        uint32_t     last_press_time_ms{0};
        TimerHandle_t debounce_timer{nullptr};
        TimerHandle_t long_press_timer{nullptr};
    };

    std::array<ButtonInfo, NUM_BUTTONS> m_buttons;
    uint32_t m_debounce_ms;
    uint32_t m_long_press_ms;
    QueueHandle_t m_queue{nullptr};

    // ── ISR and timer callbacks ────────────────────────────
    // ISR is static to avoid capturing 'this'
    static void IRAM_ATTR gpio_isr_handler(void* arg);
    static void debounce_timer_cb(TimerHandle_t timer);
    static void long_press_timer_cb(TimerHandle_t timer);

    void handle_debounce(ButtonInfo& btn);

    static constexpr const char* TAG = "ButtonDriver";
};

} // namespace Fuchey

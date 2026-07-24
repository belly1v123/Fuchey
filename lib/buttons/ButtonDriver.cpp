// ============================================================
// Fuchey — ButtonDriver.cpp
// ============================================================

#include "ButtonDriver.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_timer.h>

// Forward-declare the global queue handle defined in main.cpp
extern QueueHandle_t g_button_queue_ref;

namespace Fuchey {

static constexpr const char* TAG = "ButtonDriver";

ButtonDriver::ButtonDriver(int pin_confirm, int pin_back,
                           uint32_t debounce_ms, uint32_t long_press_ms)
    : m_debounce_ms(debounce_ms), m_long_press_ms(long_press_ms) {
    m_buttons[0].pin = pin_confirm;
    m_buttons[0].id  = ButtonId::CONFIRM;
    m_buttons[1].pin = pin_back;
    m_buttons[1].id  = ButtonId::BACK;
}

ButtonDriver::~ButtonDriver() {
    for (auto& btn : m_buttons) {
        gpio_isr_handler_remove(static_cast<gpio_num_t>(btn.pin));
        if (btn.debounce_timer)   xTimerDelete(btn.debounce_timer, 0);
        if (btn.long_press_timer) xTimerDelete(btn.long_press_timer, 0);
    }
}

bool ButtonDriver::init(QueueHandle_t output_queue) {
    m_queue = output_queue;

    // Install ISR service (once globally)
    gpio_install_isr_service(0);

    for (auto& btn : m_buttons) {
        gpio_config_t cfg{};
        cfg.pin_bit_mask = 1ULL << btn.pin;
        cfg.mode         = GPIO_MODE_INPUT;
        cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type    = GPIO_INTR_ANYEDGE;  // Both edges

        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed pin=%d: %s", btn.pin, esp_err_to_name(err));
            return false;
        }

        // Debounce timer (one-shot, fires after debounce_ms)
        btn.debounce_timer = xTimerCreate(
            "btn_debounce",
            pdMS_TO_TICKS(m_debounce_ms),
            pdFALSE,  // one-shot
            static_cast<void*>(&btn),
            debounce_timer_cb
        );

        // Long press timer (one-shot, fires after long_press_ms)
        btn.long_press_timer = xTimerCreate(
            "btn_long",
            pdMS_TO_TICKS(m_long_press_ms),
            pdFALSE,  // one-shot
            static_cast<void*>(&btn),
            long_press_timer_cb
        );

        // Attach ISR — pass pointer to ButtonInfo
        gpio_isr_handler_add(static_cast<gpio_num_t>(btn.pin),
                              gpio_isr_handler,
                              static_cast<void*>(&btn));

        ESP_LOGI(TAG, "Button %d (GPIO%d) initialized",
                 static_cast<int>(btn.id), btn.pin);
    }
    return true;
}

bool ButtonDriver::is_pressed(ButtonId id) const {
    for (const auto& btn : m_buttons) {
        if (btn.id == id) return btn.pressed;
    }
    return false;
}

// ─── ISR (runs in IRAM, no FreeRTOS blocking calls) ───────
void IRAM_ATTR ButtonDriver::gpio_isr_handler(void* arg) {
    auto* btn = static_cast<ButtonInfo*>(arg);
    // Just restart the debounce timer — actual logic in timer callback
    BaseType_t woken = pdFALSE;
    xTimerResetFromISR(btn->debounce_timer, &woken);
    portYIELD_FROM_ISR(woken);
}

// ─── Debounce timer: actual state read happens here ───────
void ButtonDriver::debounce_timer_cb(TimerHandle_t timer) {
    auto* btn = static_cast<ButtonInfo*>(pvTimerGetTimerID(timer));
    bool current = (gpio_get_level(static_cast<gpio_num_t>(btn->pin)) == 0); // active-low

    if (current == btn->pressed) return; // No state change
    btn->pressed = current;

    // We need the queue — stored on the driver instance.
    // We find the driver via a simple static pattern:
    // In this design, we just need the queue. Since ButtonInfo doesn't
    // hold the queue directly, let's use a module-level accessor.
    // For clean design, post via esp_event or a globally accessible queue.
    // We use the global Events system here.
    if (::g_button_queue_ref == nullptr) return;

    ButtonState state{
        .id           = btn->id,
        .event        = current ? ButtonEvent::PRESS : ButtonEvent::RELEASE,
        .timestamp_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000),
    };

    xQueueSend(::g_button_queue_ref, &state, 0);

    if (current) {
        // Button pressed — start long press timer
        xTimerReset(btn->long_press_timer, 0);
    } else {
        // Button released — cancel long press timer
        xTimerStop(btn->long_press_timer, 0);
    }
}

// ─── Long press timer ─────────────────────────────────────
void ButtonDriver::long_press_timer_cb(TimerHandle_t timer) {
    auto* btn = static_cast<ButtonInfo*>(pvTimerGetTimerID(timer));
    if (!btn->pressed) return; // Already released

    if (::g_button_queue_ref == nullptr) return;

    ButtonState state{
        .id           = (btn->id == ButtonId::CONFIRM) ? ButtonId::BACK : btn->id,
        .event        = ButtonEvent::LONG_PRESS,
        .timestamp_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000),
    };
    xQueueSend(::g_button_queue_ref, &state, 0);
}

} // namespace Fuchey

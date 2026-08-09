#pragma once
// ============================================================
// Fuchey — LedIndicator.hpp
// Onboard WS2812 RGB LED (ESP32-S3-DevKitC-1, GPIO48) driven
// over the modern RMT TX driver. Blinks a fixed pattern for
// transaction success (green) or failure (red), then turns off.
// ============================================================

#include <driver/rmt_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstdint>

namespace Fuchey {

enum class LedCommand : uint8_t {
    NONE    = 0,
    SUCCESS,   // green blink pattern
    FAILURE,   // red blink pattern
    OFF,
};

class LedIndicator {
public:
    LedIndicator() = default;
    ~LedIndicator() = default;

    bool init();
    void blink_success();
    void blink_failure();
    void off();

    static void task_entry(void* arg);
    void run();

private:
    void transmit_rgb(uint8_t r, uint8_t g, uint8_t b);

    rmt_channel_handle_t m_channel{nullptr};
    rmt_encoder_handle_t m_encoder{nullptr};
    QueueHandle_t        m_cmd_queue{nullptr};
    uint8_t              m_led_data[3]{};  // GRB byte order

    static constexpr const char* TAG = "LedIndicator";
};

} // namespace Fuchey

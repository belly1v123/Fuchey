// ============================================================
// Fuchey — LedIndicator.cpp
// Onboard WS2812 RGB LED (ESP32-S3-DevKitC-1, GPIO48) driven
// over the modern RMT TX driver. Blinks a fixed pattern for
// transaction success (green) or failure (red), then turns off.
// ============================================================

#include "LedIndicator.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/task.h"

namespace Fuchey {

// Build an RMT symbol word from the two (level, duration) halves.
// Layout: [duration1:15 | level1:1 | duration0:15 | level0:1]
static constexpr rmt_symbol_word_t make_symbol(bool level0, uint32_t dur0,
                                               bool level1, uint32_t dur1) {
    rmt_symbol_word_t s{};
    s.val = (static_cast<uint32_t>(level0) << 15) | (dur0 & 0x7FFF) |
            (static_cast<uint32_t>(level1) << 31) | ((dur1 & 0x7FFF) << 16);
    return s;
}

// WS2812 bit timing at the configured RMT resolution (10 MHz → 0.1 us/tick).
// From the ESP-IDF led_strip reference implementation:
//   bit0 → 0.3 us high / 0.8 us low
//   bit1 → 0.9 us high / 0.4 us low
static constexpr rmt_symbol_word_t kBit0 = make_symbol(true, 3, false, 8);
static constexpr rmt_symbol_word_t kBit1 = make_symbol(true, 9, false, 4);

bool LedIndicator::init() {
    m_cmd_queue = xQueueCreate(Fuchey::Queues::LED_COMMANDS, sizeof(LedCommand));
    if (m_cmd_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return false;
    }

    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num          = static_cast<gpio_num_t>(Fuchey::LedConfig::PIN);
    tx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz     = Fuchey::LedConfig::RMT_RESOLUTION_HZ;
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 1;
    tx_cfg.intr_priority     = 0;

    if (rmt_new_tx_channel(&tx_cfg, &m_channel) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel");
        return false;
    }

    rmt_bytes_encoder_config_t enc_cfg = {};
    enc_cfg.bit0            = kBit0;
    enc_cfg.bit1            = kBit1;
    enc_cfg.flags.msb_first = 1;

    if (rmt_new_bytes_encoder(&enc_cfg, &m_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create bytes encoder");
        return false;
    }

    if (rmt_enable(m_channel) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel");
        return false;
    }

    transmit_rgb(0, 0, 0);
    ESP_LOGI(TAG, "Initialized onboard RGB LED (GPIO%d)", Fuchey::LedConfig::PIN);
    return true;
}

void LedIndicator::transmit_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (m_channel == nullptr || m_encoder == nullptr) return;

    // WS2812 expects GRB byte order.
    m_led_data[0] = g;
    m_led_data[1] = r;
    m_led_data[2] = b;

    rmt_transmit_config_t cfg = {};
    cfg.loop_count = 0;  // single shot; WS2812 latches on the trailing reset

    if (rmt_transmit(m_channel, m_encoder, m_led_data, sizeof(m_led_data), &cfg) != ESP_OK) {
        return;
    }
    // Wait for the ~30 us burst to finish so a following toggle is not dropped.
    rmt_tx_wait_all_done(m_channel, 50);
}

void LedIndicator::blink_success() {
    if (m_cmd_queue) {
        LedCommand cmd = LedCommand::SUCCESS;
        xQueueSend(m_cmd_queue, &cmd, 0);
    }
}

void LedIndicator::blink_failure() {
    if (m_cmd_queue) {
        LedCommand cmd = LedCommand::FAILURE;
        xQueueSend(m_cmd_queue, &cmd, 0);
    }
}

void LedIndicator::off() {
    if (m_cmd_queue) {
        LedCommand cmd = LedCommand::OFF;
        xQueueSend(m_cmd_queue, &cmd, 0);
    }
}

void LedIndicator::task_entry(void* arg) {
    static_cast<LedIndicator*>(arg)->run();
}

void LedIndicator::run() {
    ESP_LOGI(TAG, "LedIndicator task running on Core %d", xPortGetCoreID());

    LedCommand cmd = LedCommand::NONE;
    uint8_t r = 0, g = 0, b = 0;

    while (true) {
        if (m_cmd_queue && xQueueReceive(m_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            int blink_count = 0;
            const char* name = "NONE";

            switch (cmd) {
                case LedCommand::SUCCESS:
                    r = 0; g = Fuchey::LedConfig::SUCCESS_BRIGHTNESS; b = 0;
                    blink_count = Fuchey::LedConfig::SUCCESS_BLINKS;
                    name = "SUCCESS";
                    break;
                case LedCommand::FAILURE:
                    r = Fuchey::LedConfig::FAILURE_BRIGHTNESS; g = 0; b = 0;
                    blink_count = Fuchey::LedConfig::FAILURE_BLINKS;
                    name = "FAILURE";
                    break;
                case LedCommand::OFF:
                default:
                    r = 0; g = 0; b = 0;
                    break;
            }

            if (blink_count > 0) ESP_LOGI(TAG, "Blinking %s x%d (r=%d g=%d b=%d)", name, blink_count, r, g, b);

            for (int i = 0; i < blink_count; ++i) {
                transmit_rgb(r, g, b);
                vTaskDelay(pdMS_TO_TICKS(Fuchey::LedConfig::BLINK_ON_MS));
                transmit_rgb(0, 0, 0);
                if (i < blink_count - 1) {
                    vTaskDelay(pdMS_TO_TICKS(Fuchey::LedConfig::BLINK_OFF_MS));
                }
            }

            if (blink_count > 0) ESP_LOGI(TAG, "Blink done (%s)", name);
            if (blink_count == 0) transmit_rgb(0, 0, 0);
        }
    }
}

} // namespace Fuchey

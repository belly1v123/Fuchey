// ============================================================
// Fuchey — Buzzer.cpp
// ============================================================

#include "Buzzer.hpp"
#include "../config/Config.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/gpio.h>

namespace Fuchey {

bool Buzzer::init() {
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << BuzzerConfig::PIN;
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed for GPIO%d", BuzzerConfig::PIN);
        return false;
    }
    gpio_set_level(static_cast<gpio_num_t>(BuzzerConfig::PIN), 0);
    ESP_LOGI(TAG, "Buzzer on GPIO%d initialized (silent)", BuzzerConfig::PIN);
    return true;
}

void Buzzer::on() {
    gpio_set_level(static_cast<gpio_num_t>(BuzzerConfig::PIN), 1);
}

void Buzzer::off() {
    gpio_set_level(static_cast<gpio_num_t>(BuzzerConfig::PIN), 0);
}

void Buzzer::beep(uint32_t ms) {
    on();
    vTaskDelay(pdMS_TO_TICKS(ms));
    off();
}

} // namespace Fuchey

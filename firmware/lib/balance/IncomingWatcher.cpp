#include "IncomingWatcher.hpp"
#include "BalanceMonitor.hpp"
#include "../wifi/WiFiManager.hpp"
#include "../events/Events.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

namespace Fuchey {

void IncomingWatcher::start() {
    xTaskCreatePinnedToCore(task_entry, "incoming", 8192, this,
                            tskIDLE_PRIORITY + 1, nullptr, 1);
}

void IncomingWatcher::task_entry(void* arg) {
    static_cast<IncomingWatcher*>(arg)->run();
}

void IncomingWatcher::run() {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        check();
    }
}

void IncomingWatcher::check() {
    if (!m_wifi.has_ip()) return;
    const BalanceMonitor::Target t = m_monitor.target();
    if (t.addr.empty()) return;

    // New wallet or network: start over so a different balance isn't
    // mistaken for money arriving.
    const std::string key = t.addr + "|" + t.rpc + "|" + t.mint;
    if (key != m_key) {
        m_key = key;
        m_have = false;
    }

    uint64_t lamports = 0, usdc = 0;
    if (!m_monitor.fetch_raw(t, lamports, usdc)) return;

    if (m_have && (lamports > m_lamports || usdc > m_usdc_units)) {
        Events::Event evt{};
        evt.type = Events::EventType::FUNDS_RECEIVED;
        evt.data.funds.sol  = lamports > m_lamports ? (lamports - m_lamports) / 1e9 : 0.0;
        evt.data.funds.usdc = usdc > m_usdc_units ? (usdc - m_usdc_units) / 1e6 : 0.0;
        ESP_LOGI(TAG, "Received +%.9f SOL, +%.6f USDC",
                 evt.data.funds.sol, evt.data.funds.usdc);
        Events::post(Events::g_ui_queue, evt);
    }
    m_have = true;
    m_lamports = lamports;
    m_usdc_units = usdc;
}

} // namespace Fuchey

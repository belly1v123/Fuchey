// ============================================================
// Fuchey — Storage.cpp
// ============================================================

#include "Storage.hpp"
#include "esp_log.h"

namespace Fuchey {

static constexpr const char* TAG = "Storage";

// ─── Global init ──────────────────────────────────────────
esp_err_t Storage::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t Storage::erase_all() {
    ESP_LOGW(TAG, "!!! ERASING ALL NVS DATA — FACTORY RESET !!!");
    return nvs_flash_erase();
}

// ─── Handle ───────────────────────────────────────────────
Storage::Handle::Handle(const char* ns, nvs_open_mode_t mode) {
    esp_err_t err = nvs_open(ns, mode, &m_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", ns, esp_err_to_name(err));
        m_open = false;
    } else {
        m_open = true;
    }
}

Storage::Handle::~Handle() {
    if (m_open) {
        nvs_close(m_handle);
        m_open = false;
    }
}

Storage::Handle::Handle(Handle&& other) noexcept
    : m_handle(other.m_handle), m_open(other.m_open) {
    other.m_open = false;
}

Storage::Handle& Storage::Handle::operator=(Handle&& other) noexcept {
    if (this != &other) {
        if (m_open) nvs_close(m_handle);
        m_handle = other.m_handle;
        m_open   = other.m_open;
        other.m_open = false;
    }
    return *this;
}

// ─── Getters ──────────────────────────────────────────────
std::optional<uint8_t> Storage::Handle::get_u8(const char* key) const {
    if (!m_open) return std::nullopt;
    uint8_t val{};
    esp_err_t err = nvs_get_u8(m_handle, key, &val);
    if (err != ESP_OK) return std::nullopt;
    return val;
}

std::optional<uint32_t> Storage::Handle::get_u32(const char* key) const {
    if (!m_open) return std::nullopt;
    uint32_t val{};
    esp_err_t err = nvs_get_u32(m_handle, key, &val);
    if (err != ESP_OK) return std::nullopt;
    return val;
}

std::optional<uint64_t> Storage::Handle::get_u64(const char* key) const {
    if (!m_open) return std::nullopt;
    uint64_t val{};
    esp_err_t err = nvs_get_u64(m_handle, key, &val);
    if (err != ESP_OK) return std::nullopt;
    return val;
}

std::optional<std::string> Storage::Handle::get_str(const char* key) const {
    if (!m_open) return std::nullopt;
    size_t len = 0;
    esp_err_t err = nvs_get_str(m_handle, key, nullptr, &len);
    if (err != ESP_OK || len == 0) return std::nullopt;
    std::string result(len, '\0');
    err = nvs_get_str(m_handle, key, result.data(), &len);
    if (err != ESP_OK) return std::nullopt;
    // Remove null terminator from string
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

size_t Storage::Handle::get_blob(const char* key, uint8_t* buf, size_t max_len) const {
    if (!m_open) return 0;
    size_t len = max_len;
    esp_err_t err = nvs_get_blob(m_handle, key, buf, &len);
    if (err != ESP_OK) return 0;
    return len;
}

// ─── Setters ──────────────────────────────────────────────
bool Storage::Handle::set_u8(const char* key, uint8_t val) {
    if (!m_open) return false;
    return nvs_set_u8(m_handle, key, val) == ESP_OK;
}

bool Storage::Handle::set_u32(const char* key, uint32_t val) {
    if (!m_open) return false;
    return nvs_set_u32(m_handle, key, val) == ESP_OK;
}

bool Storage::Handle::set_u64(const char* key, uint64_t val) {
    if (!m_open) return false;
    return nvs_set_u64(m_handle, key, val) == ESP_OK;
}

bool Storage::Handle::set_str(const char* key, const char* val) {
    if (!m_open) return false;
    return nvs_set_str(m_handle, key, val) == ESP_OK;
}

bool Storage::Handle::set_blob(const char* key, const uint8_t* data, size_t len) {
    if (!m_open) return false;
    return nvs_set_blob(m_handle, key, data, len) == ESP_OK;
}

bool Storage::Handle::erase_key(const char* key) {
    if (!m_open) return false;
    return nvs_erase_key(m_handle, key) == ESP_OK;
}

bool Storage::Handle::commit() {
    if (!m_open) return false;
    esp_err_t err = nvs_commit(m_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

} // namespace Fuchey

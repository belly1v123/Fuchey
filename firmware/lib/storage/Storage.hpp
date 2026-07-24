#pragma once
// ============================================================
// Fuchey — Storage.hpp
// RAII NVS wrapper. Typed key-value interface.
// All NVS access goes through this. No raw nvs_* calls outside.
// ============================================================

#include <nvs_flash.h>
#include <nvs.h>
#include <string>
#include <optional>
#include <cstdint>
#include <cstring>
#include <array>
#include "esp_log.h"

namespace Fuchey {

class Storage {
public:
    // ─── RAII handle for a single NVS namespace ───────────
    class Handle {
    public:
        Handle() = default;
        Handle(const char* ns, nvs_open_mode_t mode);
        ~Handle();

        // Non-copyable, movable
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&& other) noexcept;
        Handle& operator=(Handle&& other) noexcept;

        bool is_open() const { return m_open; }

        // ── Typed getters ────────────────────────────────
        std::optional<uint8_t>   get_u8(const char* key) const;
        std::optional<uint32_t>  get_u32(const char* key) const;
        std::optional<uint64_t>  get_u64(const char* key) const;
        std::optional<std::string> get_str(const char* key) const;

        // Blob getter — returns bytes written, or 0 on failure
        size_t get_blob(const char* key, uint8_t* buf, size_t max_len) const;

        // ── Typed setters ────────────────────────────────
        bool set_u8(const char* key, uint8_t val);
        bool set_u32(const char* key, uint32_t val);
        bool set_u64(const char* key, uint64_t val);
        bool set_str(const char* key, const char* val);
        bool set_blob(const char* key, const uint8_t* data, size_t len);

        bool erase_key(const char* key);
        bool commit();

    private:
        nvs_handle_t m_handle{};
        bool         m_open{false};
        static constexpr const char* TAG = "Storage";
    };

    // ─── Global NVS lifecycle ─────────────────────────────
    // Call once at boot before any Handle is opened
    static esp_err_t init();

    // Erase all NVS — use with extreme caution (factory reset)
    static esp_err_t erase_all();

private:
    static constexpr const char* TAG = "Storage";
};

} // namespace Fuchey

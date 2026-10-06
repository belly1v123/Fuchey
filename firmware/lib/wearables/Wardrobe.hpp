#pragma once
// ============================================================
// Fuchey — Wardrobe.hpp
// What Yeti is wearing (one item id per slot) and the wallets
// linked to this Fuchey, kept in NVS namespace "wardrobe".
//
// Security tier "settings": cosmetic only. Linked wallets are
// plain public addresses the app uses to look up owned items;
// the device never signs anything about them and never trusts
// them for anything but choosing what to draw.
// ============================================================

#include "WearItem.hpp"
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace Fuchey {

class Wardrobe {
public:
    static constexpr size_t MAX_WALLETS = 8;

    void load();   // once at boot (after NVS init)

    // "" when the slot is empty.
    std::string worn(int slot) const;
    // id must be a valid item id, or "" / "none" to take the slot off.
    bool set_worn(int slot, const char* id);
    // Takes `id` off every slot wearing it (item deleted).
    void unequip_item(const char* id);

    std::vector<std::string> linked_wallets() const;
    // Each a base58 address (32..44 chars); replaces the whole list.
    bool set_linked_wallets(const std::vector<std::string>& wallets);
    static bool valid_address(const char* s);

    // Bumped on every change so renderers know to reload.
    uint32_t generation() const { return m_gen.load(); }

private:
    bool save_slot(int slot);

    std::array<std::string, WEAR_SLOT_COUNT> m_worn;
    std::vector<std::string>                 m_wallets;
    std::atomic<uint32_t>                    m_gen{1};
    SemaphoreHandle_t                        m_lock{xSemaphoreCreateMutex()};

    static constexpr const char* NS  = "wardrobe";
    static constexpr const char* TAG = "Wardrobe";
};

Wardrobe& wardrobe();

} // namespace Fuchey

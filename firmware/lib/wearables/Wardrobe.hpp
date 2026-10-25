#pragma once
// ============================================================
// Fuchey — Wardrobe.hpp
// Which character Fuchey shows, what each character is wearing (one
// item id per slot, kept per character so switching back restores a
// look) and the wallets linked to this Fuchey, in NVS namespace
// "wardrobe". The Yeti's look keeps its original keys (one per slot);
// any other character's look is one key, "lk" + hash of its id.
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

    // The character on the home screen: "yeti" (built in) or a pack id.
    std::string character() const;
    // Switches character and loads its look. The caller checks a pack exists.
    bool set_character(const char* id);

    // The current character's look. "" when the slot is empty.
    std::string worn(int slot) const;
    // id must be a valid item id, or "" / "none" to take the slot off.
    bool set_worn(int slot, const char* id);
    // Takes `id` off every slot of `character`'s look (item deleted).
    void unequip_item(const char* id, const char* character = nullptr);

    std::vector<std::string> linked_wallets() const;
    // Each a base58 address (32..44 chars); replaces the whole list.
    bool set_linked_wallets(const std::vector<std::string>& wallets);
    static bool valid_address(const char* s);

    // Bumped on every change so renderers know to reload.
    uint32_t generation() const { return m_gen.load(); }

private:
    using Look = std::array<std::string, WEAR_SLOT_COUNT>;
    static bool is_yeti(const std::string& character);
    static std::string look_key(const std::string& character);
    Look load_look(const std::string& character) const;   // from NVS
    bool save_look(const std::string& character, const Look& look, int only_slot = -1);

    std::string                              m_character{"yeti"};
    Look                                     m_worn;
    std::vector<std::string>                 m_wallets;
    std::atomic<uint32_t>                    m_gen{1};
    SemaphoreHandle_t                        m_lock{xSemaphoreCreateMutex()};

    static constexpr const char* NS  = "wardrobe";
    static constexpr const char* TAG = "Wardrobe";
};

Wardrobe& wardrobe();

} // namespace Fuchey

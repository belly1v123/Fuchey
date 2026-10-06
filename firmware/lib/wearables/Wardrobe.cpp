// ============================================================
// Fuchey — Wardrobe.cpp
// ============================================================

#include "Wardrobe.hpp"
#include "../storage/Storage.hpp"
#include "esp_log.h"
#include <cstring>

namespace Fuchey {

namespace {
struct Lock {
    SemaphoreHandle_t m;
    explicit Lock(SemaphoreHandle_t mm) : m(mm) { xSemaphoreTake(m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m); }
};
constexpr const char* KEY_WALLETS = "wallets";
} // namespace

Wardrobe& wardrobe() {
    static Wardrobe w;
    return w;
}

bool Wardrobe::valid_address(const char* s) {
    if (!s) return false;
    static constexpr const char* B58 =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    const size_t n = strlen(s);
    if (n < 32 || n > 44) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!strchr(B58, s[i])) return false;
    }
    return true;
}

void Wardrobe::load() {
    Lock l(m_lock);
    Storage::Handle h(NS, NVS_READONLY);   // missing namespace = nothing worn
    for (int i = 0; i < WEAR_SLOT_COUNT; ++i) {
        auto v = h.is_open() ? h.get_str(WEAR_SLOTS[i]) : std::nullopt;
        m_worn[i] = (v && WearItem::valid_id(v->c_str())) ? *v : "";
    }
    m_wallets.clear();
    if (auto v = h.is_open() ? h.get_str(KEY_WALLETS) : std::nullopt) {
        // Stored comma-separated.
        size_t start = 0;
        while (start < v->size() && m_wallets.size() < MAX_WALLETS) {
            size_t comma = v->find(',', start);
            if (comma == std::string::npos) comma = v->size();
            std::string a = v->substr(start, comma - start);
            if (valid_address(a.c_str())) m_wallets.push_back(a);
            start = comma + 1;
        }
    }
    m_gen.fetch_add(1);
}

std::string Wardrobe::worn(int slot) const {
    if (slot < 0 || slot >= WEAR_SLOT_COUNT) return "";
    Lock l(m_lock);
    return m_worn[slot];
}

bool Wardrobe::save_slot(int slot) {
    Storage::Handle h(NS, NVS_READWRITE);
    if (!h.is_open()) return false;
    const bool ok = m_worn[slot].empty() ? (h.erase_key(WEAR_SLOTS[slot]) || true)
                                         : h.set_str(WEAR_SLOTS[slot], m_worn[slot].c_str());
    return ok && h.commit();
}

bool Wardrobe::set_worn(int slot, const char* id) {
    if (slot < 0 || slot >= WEAR_SLOT_COUNT || !id) return false;
    const bool off = id[0] == '\0' || strcmp(id, "none") == 0;
    if (!off && !WearItem::valid_id(id)) return false;
    Lock l(m_lock);
    m_worn[slot] = off ? "" : id;
    const bool ok = save_slot(slot);
    m_gen.fetch_add(1);
    return ok;
}

void Wardrobe::unequip_item(const char* id) {
    if (!id) return;
    Lock l(m_lock);
    for (int i = 0; i < WEAR_SLOT_COUNT; ++i) {
        if (m_worn[i] == id) {
            m_worn[i].clear();
            save_slot(i);
            m_gen.fetch_add(1);
        }
    }
}

std::vector<std::string> Wardrobe::linked_wallets() const {
    Lock l(m_lock);
    return m_wallets;
}

bool Wardrobe::set_linked_wallets(const std::vector<std::string>& wallets) {
    if (wallets.size() > MAX_WALLETS) return false;
    std::string joined;
    for (const auto& a : wallets) {
        if (!valid_address(a.c_str())) return false;
        if (!joined.empty()) joined += ',';
        joined += a;
    }
    Lock l(m_lock);
    Storage::Handle h(NS, NVS_READWRITE);
    if (!h.is_open()) return false;
    const bool ok = (joined.empty() ? (h.erase_key(KEY_WALLETS) || true)
                                    : h.set_str(KEY_WALLETS, joined.c_str())) && h.commit();
    if (ok) m_wallets = wallets;
    m_gen.fetch_add(1);
    return ok;
}

} // namespace Fuchey

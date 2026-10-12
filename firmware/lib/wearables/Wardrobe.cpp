// ============================================================
// Fuchey — Wardrobe.cpp
// ============================================================

#include "Wardrobe.hpp"
#include "../storage/Storage.hpp"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

namespace Fuchey {

namespace {
struct Lock {
    SemaphoreHandle_t m;
    explicit Lock(SemaphoreHandle_t mm) : m(mm) { xSemaphoreTake(m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m); }
};
constexpr const char* KEY_WALLETS   = "wallets";
constexpr const char* KEY_CHARACTER = "character";
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

bool Wardrobe::is_yeti(const std::string& character) {
    return character.empty() || character == "yeti";
}

// NVS keys are at most 15 chars, character ids up to 48: "lk" + FNV-1a hex.
std::string Wardrobe::look_key(const std::string& character) {
    uint32_t h = 2166136261u;
    for (unsigned char c : character) h = (h ^ c) * 16777619u;
    char key[12];
    snprintf(key, sizeof(key), "lk%08lx", static_cast<unsigned long>(h));
    return key;
}

Wardrobe::Look Wardrobe::load_look(const std::string& character) const {
    Look look;
    Storage::Handle h(NS, NVS_READONLY);   // missing namespace = nothing worn
    if (!h.is_open()) return look;
    if (is_yeti(character)) {
        for (int i = 0; i < WEAR_SLOT_COUNT; ++i) {
            auto v = h.get_str(WEAR_SLOTS[i]);
            look[i] = (v && WearItem::valid_id(v->c_str())) ? *v : "";
        }
        return look;
    }
    // Stored as "<id>,<id>,,…": one field per slot, in WEAR_SLOTS order.
    auto v = h.get_str(look_key(character).c_str());
    if (!v) return look;
    size_t start = 0;
    for (int i = 0; i < WEAR_SLOT_COUNT && start <= v->size(); ++i) {
        size_t comma = v->find(',', start);
        if (comma == std::string::npos) comma = v->size();
        std::string id = v->substr(start, comma - start);
        look[i] = WearItem::valid_id(id.c_str()) ? id : "";
        start = comma + 1;
    }
    return look;
}

bool Wardrobe::save_look(const std::string& character, const Look& look, int only_slot) {
    Storage::Handle h(NS, NVS_READWRITE);
    if (!h.is_open()) return false;
    bool ok = true;
    if (is_yeti(character)) {
        for (int i = 0; i < WEAR_SLOT_COUNT; ++i) {
            if (only_slot >= 0 && i != only_slot) continue;
            ok = (look[i].empty() ? (h.erase_key(WEAR_SLOTS[i]) || true)
                                  : h.set_str(WEAR_SLOTS[i], look[i].c_str())) && ok;
        }
    } else {
        std::string joined;
        bool any = false;
        for (int i = 0; i < WEAR_SLOT_COUNT; ++i) {
            if (i) joined += ',';
            joined += look[i];
            any = any || !look[i].empty();
        }
        const std::string key = look_key(character);
        ok = any ? h.set_str(key.c_str(), joined.c_str()) : (h.erase_key(key.c_str()) || true);
    }
    return ok && h.commit();
}

void Wardrobe::load() {
    Lock l(m_lock);
    std::string character = "yeti";
    {
        Storage::Handle h(NS, NVS_READONLY);
        auto v = h.is_open() ? h.get_str(KEY_CHARACTER) : std::nullopt;
        if (v && WearItem::valid_id(v->c_str())) character = *v;
        m_wallets.clear();
        if (auto w = h.is_open() ? h.get_str(KEY_WALLETS) : std::nullopt) {
            // Stored comma-separated.
            size_t start = 0;
            while (start < w->size() && m_wallets.size() < MAX_WALLETS) {
                size_t comma = w->find(',', start);
                if (comma == std::string::npos) comma = w->size();
                std::string a = w->substr(start, comma - start);
                if (valid_address(a.c_str())) m_wallets.push_back(a);
                start = comma + 1;
            }
        }
    }
    m_character = character;
    m_worn = load_look(character);
    m_gen.fetch_add(1);
}

std::string Wardrobe::character() const {
    Lock l(m_lock);
    return m_character;
}

bool Wardrobe::set_character(const char* id) {
    if (!id || !WearItem::valid_id(id)) return false;
    Lock l(m_lock);
    if (m_character == id) return true;
    Storage::Handle h(NS, NVS_READWRITE);
    if (!h.is_open() || !h.set_str(KEY_CHARACTER, id) || !h.commit()) return false;
    m_character = id;
    m_worn = load_look(m_character);
    m_gen.fetch_add(1);
    ESP_LOGI(TAG, "Character: %s", id);
    return true;
}

std::string Wardrobe::worn(int slot) const {
    if (slot < 0 || slot >= WEAR_SLOT_COUNT) return "";
    Lock l(m_lock);
    return m_worn[slot];
}

bool Wardrobe::set_worn(int slot, const char* id) {
    if (slot < 0 || slot >= WEAR_SLOT_COUNT || !id) return false;
    const bool off = id[0] == '\0' || strcmp(id, "none") == 0;
    if (!off && !WearItem::valid_id(id)) return false;
    Lock l(m_lock);
    m_worn[slot] = off ? "" : id;
    const bool ok = save_look(m_character, m_worn, slot);
    m_gen.fetch_add(1);
    return ok;
}

void Wardrobe::unequip_item(const char* id, const char* character) {
    if (!id) return;
    Lock l(m_lock);
    const std::string who = (character && character[0]) ? character : "yeti";
    const bool current = who == m_character || (is_yeti(who) && is_yeti(m_character));
    Look look = current ? m_worn : load_look(who);
    bool changed = false;
    for (int i = 0; i < WEAR_SLOT_COUNT; ++i) {
        if (look[i] == id) {
            look[i].clear();
            changed = true;
        }
    }
    if (!changed) return;
    save_look(who, look);
    if (current) {
        m_worn = look;
        m_gen.fetch_add(1);
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

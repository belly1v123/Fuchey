// ============================================================
// Fuchey — WearItem.cpp
// ============================================================

#include "WearItem.hpp"
#include <cstring>

namespace Fuchey {

int wear_slot_from_name(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < WEAR_SLOT_COUNT; ++i) {
        if (strcmp(name, WEAR_SLOTS[i]) == 0) return i;
    }
    return -1;
}

bool WearItem::valid_id(const char* id) {
    if (!id) return false;
    const size_t n = strlen(id);
    if (n < 2 || n > MAX_ID || id[0] == '-') return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

namespace {
struct Reader {
    const uint8_t* p;
    size_t         left;
    bool take(void* out, size_t n) {
        if (n > left) return false;
        memcpy(out, p, n);
        p += n;
        left -= n;
        return true;
    }
    bool u8(uint8_t& v) { return take(&v, 1); }
    bool u16(uint16_t& v) {
        uint8_t b[2];
        if (!take(b, 2)) return false;
        v = static_cast<uint16_t>(b[0] | (b[1] << 8));
        return true;
    }
};
} // namespace

bool WearItem::parse(const uint8_t* data, size_t len, WearItem& out) {
    if (!data || len > MAX_BYTES) return false;
    Reader r{data, len};
    char magic[4];
    if (!r.take(magic, 4) || memcmp(magic, "FWR1", 4) != 0) return false;

    uint8_t id_len = 0;
    if (!r.u8(id_len) || id_len < 2 || id_len > MAX_ID) return false;
    char id[MAX_ID + 1] = {};
    if (!r.take(id, id_len) || !valid_id(id)) return false;

    uint8_t slot = 0;
    uint16_t z = 0, front_n = 0, back_n = 0;
    if (!r.u8(slot) || slot >= WEAR_SLOT_COUNT) return false;
    if (!r.u16(z) || !r.u16(front_n) || !r.u16(back_n)) return false;
    if (static_cast<size_t>(front_n) + back_n > MAX_RUNS) return false;
    if (r.left != (static_cast<size_t>(front_n) + back_n) * 5) return false;   // exact size

    auto read_runs = [&r](std::vector<WearRun>& v, uint16_t n) {
        v.clear();
        v.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            WearRun run{};
            uint8_t x, y;
            if (!r.u8(x) || !r.u8(y) || !r.u8(run.w) || !r.u16(run.c)) return false;
            run.x = static_cast<int8_t>(x);
            run.y = static_cast<int8_t>(y);
            if (run.w == 0) return false;
            v.push_back(run);
        }
        return true;
    };

    out.id   = id;
    out.slot = slot;
    out.z    = static_cast<int16_t>(z);
    return read_runs(out.front, front_n) && read_runs(out.back, back_n);
}

} // namespace Fuchey

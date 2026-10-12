// ============================================================
// Fuchey — CharacterPack.cpp
// ============================================================

#include "CharacterPack.hpp"
#include "WearItem.hpp"
#include "../display/Display.hpp"
#include "esp_heap_caps.h"
#include <cstring>

namespace Fuchey {

namespace {
struct Reader {
    const uint8_t* base;
    size_t         len;
    size_t         pos{0};
    bool take(void* out, size_t n) {
        if (n > len - pos) return false;
        memcpy(out, base + pos, n);
        pos += n;
        return true;
    }
    bool skip(size_t n) {
        if (n > len - pos) return false;
        pos += n;
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
constexpr size_t RUN_BYTES = 5;
} // namespace

CharacterPack::~CharacterPack() {
    if (m_data) heap_caps_free(m_data);
}

bool CharacterPack::parse(const uint8_t* data, size_t len, CharacterPack* out, std::string* id_out) {
    if (!data || len > MAX_BYTES) return false;
    Reader r{data, len};
    char magic[4];
    if (!r.take(magic, 4) || memcmp(magic, "FCP1", 4) != 0) return false;

    uint8_t id_len = 0;
    if (!r.u8(id_len) || id_len < 2 || id_len > WearItem::MAX_ID) return false;
    char id[WearItem::MAX_ID + 1] = {};
    if (!r.take(id, id_len) || !WearItem::valid_id(id)) return false;

    uint8_t legs = 0, clip_count = 0;
    if (!r.u8(legs) || !r.u8(clip_count) || clip_count == 0 || clip_count > MOOD_COUNT) return false;

    bool seen[MOOD_COUNT] = {};
    for (uint8_t c = 0; c < clip_count; ++c) {
        uint8_t mood, fps, w, h, frames;
        if (!r.u8(mood) || !r.u8(fps) || !r.u8(w) || !r.u8(h) || !r.u8(frames)) return false;
        if (mood >= MOOD_COUNT || seen[mood] || fps == 0 || frames == 0) return false;
        if (w == 0 || h == 0 || w > BOX || h > BOX) return false;
        seen[mood] = true;
        Clip clip;
        clip.fps = fps;
        clip.w = w;
        clip.h = h;
        clip.frames.reserve(frames);
        for (uint8_t f = 0; f < frames; ++f) {
            uint8_t dx, dy;
            uint16_t count;
            if (!r.u8(dx) || !r.u8(dy) || !r.u16(count)) return false;
            Frame fr;
            fr.dx = static_cast<int8_t>(dx);
            fr.dy = static_cast<int8_t>(dy);
            fr.runs = static_cast<uint32_t>(r.pos);
            fr.count = count;
            // Every run must stay inside the frame.
            for (uint16_t i = 0; i < count; ++i) {
                const uint8_t* run = data + r.pos;
                if (!r.skip(RUN_BYTES)) return false;
                if (run[2] == 0 || run[1] >= h || run[0] + run[2] > w) return false;
            }
            clip.frames.push_back(fr);
        }
        if (out) {
            out->m_clips[mood] = std::move(clip);
            out->m_has[mood] = true;
        }
    }
    if (!seen[IDLE] || r.pos != len) return false;   // idle required, exact size
    if (out) {
        out->m_id = id;
        out->m_legs_y = static_cast<int8_t>(legs);
    }
    if (id_out) *id_out = id;
    return true;
}

bool CharacterPack::validate(const uint8_t* data, size_t len, std::string* id_out) {
    return parse(data, len, nullptr, id_out);
}

bool CharacterPack::adopt(uint8_t* data, size_t len) {
    for (auto& h : m_has) h = false;
    if (!parse(data, len, this, nullptr)) return false;
    if (m_data) heap_caps_free(m_data);
    m_data = data;
    m_len = len;
    return true;
}

const CharacterPack::Clip& CharacterPack::clip(Mood m) const {
    return (m < MOOD_COUNT && m_has[m]) ? m_clips[m] : m_clips[IDLE];
}

void CharacterPack::draw(Display& d, const Clip& c, uint8_t f, int x, int y) const {
    if (!m_data || f >= c.frames.size()) return;
    const Frame& fr = c.frames[f];
    const uint8_t* p = m_data + fr.runs;
    for (uint16_t i = 0; i < fr.count; ++i, p += RUN_BYTES) {
        const uint16_t col = static_cast<uint16_t>(p[3] | (p[4] << 8));
        d.fill_rect(x + p[0], y + p[1], p[2], 1, col);
    }
}

} // namespace Fuchey

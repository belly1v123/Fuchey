#pragma once
// ============================================================
// Fuchey — SpritePlayer.hpp
// Header-only frame-by-frame animation player for ST7789 240x240.
// No heap, no blocking. Frames by scripts/convert_sprite.py.
// ============================================================

#include <cstdint>
#include <cstddef>

namespace Fuchey {

using SpritePixel = uint16_t;  // RGB565, CPU order

struct SpriteAnim {
    const SpritePixel* data;   // frames packed back-to-back, w*h each
    uint16_t w = 0;
    uint16_t h = 0;
    uint8_t frames = 0;
    uint8_t fps = 10;
    bool loop = true;
};

class SpritePlayer {
public:
    SpritePlayer() = default;

    void set_anim(const SpriteAnim* anim, uint32_t now_ms) {
        m_anim = anim;
        m_start_ms = now_ms;
        m_last_frame = 255;  // force dirty on first tick
        m_done = false;
    }

    void clear() { m_anim = nullptr; m_done = false; m_last_frame = 255; }

    bool has_anim() const { return m_anim != nullptr && m_anim->data != nullptr && m_anim->frames > 0; }
    bool done() const { return m_done; }

    uint8_t frame_at(uint32_t now_ms) const {
        if (!has_anim()) return 0;
        uint32_t elapsed = now_ms - m_start_ms;
        uint32_t per = 1000u / (m_anim->fps ? m_anim->fps : 10u);
        uint32_t idx = elapsed / per;
        if (m_anim->loop) return static_cast<uint8_t>(idx % m_anim->frames);
        if (idx >= m_anim->frames) return static_cast<uint8_t>(m_anim->frames - 1);
        return static_cast<uint8_t>(idx);
    }

    // Returns true when the visible frame changed (caller should redraw).
    bool tick(uint32_t now_ms) {
        if (!has_anim()) return false;
        uint8_t f = frame_at(now_ms);
        if (f != m_last_frame) {
            m_last_frame = f;
            if (!m_anim->loop && f == m_anim->frames - 1) m_done = true;
            return true;
        }
        return false;
    }

    const SpritePixel* frame_data(uint8_t f) const {
        if (!has_anim() || f >= m_anim->frames) return nullptr;
        return m_anim->data + static_cast<size_t>(f) * static_cast<size_t>(m_anim->w) * m_anim->h;
    }

    const SpritePixel* current_frame_data() const { return frame_data(m_last_frame == 255 ? 0 : m_last_frame); }
    uint8_t current_frame() const { return m_last_frame == 255 ? 0 : m_last_frame; }

private:
    const SpriteAnim* m_anim = nullptr;
    uint32_t m_start_ms = 0;
    uint8_t m_last_frame = 255;
    bool m_done = false;
};

}  // namespace Fuchey

// ============================================================
// Fuchey — WearRenderer.cpp
// ============================================================

#include "WearRenderer.hpp"
#include "ItemStore.hpp"
#include "Wardrobe.hpp"
#include "../display/Display.hpp"
#include "esp_log.h"
#include <algorithm>
#include <climits>

namespace Fuchey {

namespace {
constexpr const char* TAG = "WearRenderer";

// floor(v * num / den), also for negative v (hats sit above the grid).
inline int scale_floor(int v, int num, int den) {
    const int n = v * num;
    return n >= 0 ? n / den : -((-n + den - 1) / den);
}
} // namespace

bool WearRenderer::refresh() {
    const uint32_t sg = item_store().generation();
    const uint32_t wg = wardrobe().generation();
    if (sg == m_store_gen && wg == m_wardrobe_gen) return false;
    m_store_gen = sg;
    m_wardrobe_gen = wg;

    m_items.clear();
    for (int slot = 0; slot < WEAR_SLOT_COUNT; ++slot) {
        const std::string id = wardrobe().worn(slot);
        if (id.empty()) continue;
        WearItem item;
        if (!item_store().load(id.c_str(), item)) {
            ESP_LOGW(TAG, "%s worn in %s but not installed", id.c_str(), WEAR_SLOTS[slot]);
            continue;
        }
        m_items.push_back(std::move(item));
    }
    std::stable_sort(m_items.begin(), m_items.end(),
                     [](const WearItem& a, const WearItem& b) { return a.rank() < b.rank(); });
    ESP_LOGI(TAG, "Wearing %u item(s)", static_cast<unsigned>(m_items.size()));
    return true;
}

void WearRenderer::offset_for(const WearItem& it, const CharacterPose& p, int& dx, int& dy) {
    int top = INT_MAX;
    for (const WearRun& r : it.back) top = std::min(top, static_cast<int>(r.y));
    for (const WearRun& r : it.front) top = std::min(top, static_cast<int>(r.y));
    const bool still = top >= p.still_from_y;
    dx = still ? 0 : p.dx;
    dy = still ? 0 : p.dy;
}

void WearRenderer::draw_runs(Display& d, const CharacterPose& p,
                             const std::vector<WearRun>& runs, int dx, int dy) const {
    const int num = p.scale_num, den = p.scale_den > 0 ? p.scale_den : 1;
    for (const WearRun& r : runs) {
        const int gx = r.x + dx, gy = r.y + dy;
        const int x0 = p.origin_x + scale_floor(gx, num, den);
        const int x1 = p.origin_x + scale_floor(gx + r.w, num, den);
        const int y0 = p.origin_y + scale_floor(gy, num, den);
        const int y1 = p.origin_y + scale_floor(gy + 1, num, den);
        if (x1 > x0 && y1 > y0) d.fill_rect(x0, y0, x1 - x0, y1 - y0, r.c);   // clips to screen
    }
}

void WearRenderer::draw_back(Display& d, const CharacterPose& p) const {
    for (const WearItem& it : m_items) {
        int dx, dy;
        offset_for(it, p, dx, dy);
        draw_runs(d, p, it.back, dx, dy);
    }
}

void WearRenderer::draw_front(Display& d, const CharacterPose& p) const {
    for (const WearItem& it : m_items) {
        int dx, dy;
        offset_for(it, p, dx, dy);
        draw_runs(d, p, it.front, dx, dy);
    }
}

bool WearRenderer::bounds(const CharacterPose& p, int& x, int& y, int& w, int& h) const {
    const int num = p.scale_num, den = p.scale_den > 0 ? p.scale_den : 1;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (const WearItem& it : m_items) {
        int dx, dy;
        offset_for(it, p, dx, dy);
        for (const auto* runs : {&it.back, &it.front}) {
            for (const WearRun& r : *runs) {
                x0 = std::min(x0, p.origin_x + scale_floor(r.x + dx, num, den));
                x1 = std::max(x1, p.origin_x + scale_floor(r.x + r.w + dx, num, den));
                y0 = std::min(y0, p.origin_y + scale_floor(r.y + dy, num, den));
                y1 = std::max(y1, p.origin_y + scale_floor(r.y + 1 + dy, num, den));
            }
        }
    }
    if (x0 >= x1 || y0 >= y1) return false;
    x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    return true;
}

} // namespace Fuchey

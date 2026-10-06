#pragma once
// ============================================================
// Fuchey — WearRenderer.hpp
// Draws the worn items around a character into the framebuffer.
//
// Knows nothing about Yeti: the caller says where the character's
// 96×96 grid sits on screen, how big a grid pixel is, and how far
// the current animation frame moved the body (CharacterPose). Order
// matches the website: every back layer, the character, then every
// front layer, each sorted by slot*1000 + z.
// ============================================================

#include "WearItem.hpp"
#include <cstdint>
#include <vector>

namespace Fuchey {

class Display;

// Where a character's base grid is on screen for one frame.
struct CharacterPose {
    int origin_x{0};      // screen x of grid (0,0)
    int origin_y{0};
    int scale_num{2};     // one grid px = scale_num/scale_den screen px
    int scale_den{1};
    int dx{0};            // body offset of this animation frame, grid px
    int dy{0};
    // Items that start at or below this grid row (e.g. something standing
    // by the feet) stay put: the frame offset only moves the upper body.
    int still_from_y{1 << 20};
};

class WearRenderer {
public:
    // Reloads the worn items when the wardrobe or the item files changed.
    // Returns true when what is drawn changed (caller should redraw).
    bool refresh();

    bool empty() const { return m_items.empty(); }

    void draw_back(Display& d, const CharacterPose& p) const;
    void draw_front(Display& d, const CharacterPose& p) const;

    // Screen box covering every worn layer at pose p (false if nothing worn).
    bool bounds(const CharacterPose& p, int& x, int& y, int& w, int& h) const;

private:
    void draw_runs(Display& d, const CharacterPose& p, const std::vector<WearRun>& runs,
                   int dx, int dy) const;
    // The pose with its frame offset dropped for items that don't follow the body.
    static void offset_for(const WearItem& it, const CharacterPose& p, int& dx, int& dy);

    std::vector<WearItem> m_items;      // worn, back-to-front
    uint32_t              m_store_gen{0};
    uint32_t              m_wardrobe_gen{0};
};

} // namespace Fuchey

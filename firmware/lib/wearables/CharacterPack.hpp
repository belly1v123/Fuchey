#pragma once
// ============================================================
// Fuchey — CharacterPack.hpp
// A marketplace character other than the built-in Yeti, as stored
// on flash (/fs/chars/<id>.bin). Packs are DATA built on the PC by
// scripts/make_character_pack.py and sent by the companion app.
//
// File "FCP1" (all integers little-endian):
//   char[4] magic "FCP1"
//   u8      id_len, then id (a-z 0-9 -, 2..48 chars, = file name)
//   i8      legs_y      grid row: in idle, items from here down stay put
//   u8      clip_count
//   clips × { u8 mood (0 idle, 1 happy, 2 dnd), u8 fps, u8 w, u8 h, u8 frames,
//             frames × { i8 dx, i8 dy, u16 run_count,
//                        runs × { u8 x, u8 y, u8 w, u16 rgb565 } } }
// Frames are drawn 1:1 into the home screen's 120×120 character box
// (= the 96×96 item grid at 1.25×). dx/dy is the frame's head offset vs
// idle frame 0 in grid px, so worn hats follow the head.
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Fuchey {

class Display;

class CharacterPack {
public:
    enum Mood : uint8_t { IDLE = 0, HAPPY = 1, DND = 2, MOOD_COUNT = 3 };

    struct Frame {
        int8_t   dx{0};
        int8_t   dy{0};
        uint32_t runs{0};     // byte offset of the first run in the file
        uint16_t count{0};
    };
    struct Clip {
        uint8_t            fps{10};
        uint8_t            w{0};
        uint8_t            h{0};
        std::vector<Frame> frames;
    };

    CharacterPack() = default;
    ~CharacterPack();
    CharacterPack(const CharacterPack&) = delete;
    CharacterPack& operator=(const CharacterPack&) = delete;

    // Validates a whole FCP1 file (every run inside its frame). Doesn't keep `data`.
    static bool validate(const uint8_t* data, size_t len, std::string* id_out = nullptr);
    // Takes ownership of `data` (heap_caps_malloc'ed) on success.
    bool adopt(uint8_t* data, size_t len);

    const std::string& id() const { return m_id; }
    int legs_y() const { return m_legs_y; }
    // The clip for a mood; a missing happy/dnd clip falls back to idle.
    const Clip& clip(Mood m) const;

    // Draws frame `f` of `c` with its top-left at (x, y).
    void draw(Display& d, const Clip& c, uint8_t f, int x, int y) const;

    static constexpr size_t MAX_BYTES  = 2 * 1024 * 1024;
    static constexpr uint8_t BOX       = 120;

private:
    static bool parse(const uint8_t* data, size_t len, CharacterPack* out, std::string* id_out);

    uint8_t*    m_data{nullptr};
    size_t      m_len{0};
    std::string m_id;
    int         m_legs_y{1 << 20};
    Clip        m_clips[MOOD_COUNT];
    bool        m_has[MOOD_COUNT]{};
};

} // namespace Fuchey

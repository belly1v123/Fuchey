#pragma once
// ============================================================
// Fuchey — WearItem.hpp
// A marketplace wearable as stored on flash (/fs/items/<id>.bin).
// Items are DATA: the companion app converts the website's
// `visual` into this file; nothing per item is built into firmware.
//
// File "FWR1" (all integers little-endian):
//   char[4]  magic "FWR1"
//   u8       id_len, then id (a-z 0-9 -, 2..48 chars, = file name)
//   u8       slot   (index into WEAR_SLOTS)
//   i16      z      (fine order inside the slot)
//   u16      front_count
//   u16      back_count
//   runs     front_count + back_count × { i8 x, i8 y, u8 w, u16 rgb565 }
// Runs are horizontal 1-pixel-high strips on the character's 96×96
// base grid (y may be negative for hats). Already resolved for one
// character (perCharacter overrides and x/y shifts applied).
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Fuchey {

// Wardrobe slots, back to front (the website's SLOT_ORDER).
inline constexpr const char* WEAR_SLOTS[] = {
    "outfit", "backpack", "accessory", "headwear", "hat", "held", "special"};
inline constexpr int WEAR_SLOT_COUNT = 7;

int wear_slot_from_name(const char* name);   // -1 if unknown

struct WearRun {
    int8_t   x;
    int8_t   y;
    uint8_t  w;
    uint16_t c;   // RGB565
};

struct WearItem {
    std::string          id;
    uint8_t              slot{0};
    int16_t              z{0};
    std::vector<WearRun> front;
    std::vector<WearRun> back;

    // Back-to-front stacking rank (same as the website's layerRank).
    int rank() const { return slot * 1000 + z; }

    // Parses and validates a FWR1 file. False on any malformed byte.
    static bool parse(const uint8_t* data, size_t len, WearItem& out);
    // 2..48 chars of a-z 0-9 '-', not starting with '-' (the website's id rule).
    static bool valid_id(const char* id);

    static constexpr size_t MAX_ID    = 48;
    static constexpr size_t MAX_RUNS  = 6000;          // 96×96 worst case ≈ 4.6k
    static constexpr size_t MAX_BYTES = 16 + MAX_ID + MAX_RUNS * 5;
};

} // namespace Fuchey

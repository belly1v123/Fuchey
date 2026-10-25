#pragma once
// ============================================================
// Fuchey — ItemStore.hpp
// Wearable files on the LittleFS "items" partition (/fs/items),
// and character packs (/fs/chars, see CharacterPack.hpp).
//
// Items are resolved for one character, so each character has its own
// folder: the built-in Yeti's items stay in /fs/items (as before),
// any other character's in /fs/items/<character>/.
//
// Uploads arrive in chunks over USB (UsbProtocol item_begin /
// item_chunk / item_end). They are collected in PSRAM, checked
// (size, CRC32, FWR1 format, id == file id) and only then written,
// via a temp file + rename, so a broken upload never replaces a
// good item. Thread-safe: the console task writes, the UI reads.
// ============================================================

#include "CharacterPack.hpp"
#include "WearItem.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace Fuchey {

class ItemStore {
public:
    enum class Err {
        OK, NOT_MOUNTED, BAD_ID, TOO_BIG, NO_UPLOAD, BAD_SEQ, BAD_SIZE,
        BAD_CRC, BAD_FORMAT, NOT_FOUND, IO, NO_MEMORY,
    };
    static const char* err_name(Err e);

    struct Info {
        std::string id;
        uint8_t     slot{0};
        int16_t     z{0};
        uint32_t    bytes{0};
        uint32_t    crc{0};       // zlib CRC32 of the file (the app compares it)
    };

    ItemStore();

    // Mounts LittleFS (formats it the first time). Call once at boot.
    bool mount();
    bool mounted() const { return m_mounted; }

    // ── Chunked upload (one at a time; a new begin drops the old one) ──
    // An item resolved for `character` (nullptr / "" / "yeti" = the built-in
    // Yeti), or a character pack (begin_pack).
    Err begin(const char* id, size_t size, uint32_t crc, const char* character = nullptr);
    Err begin_pack(const char* id, size_t size, uint32_t crc);
    Err chunk(const char* id, uint32_t seq, const uint8_t* data, size_t len);
    Err end(const char* id);
    void abort_upload();

    Err  remove(const char* id, const char* character = nullptr);
    bool exists(const char* id, const char* character = nullptr);
    bool list(std::vector<Info>& out, const char* character = nullptr);
    bool load(const char* id, WearItem& out, const char* character = nullptr);

    // ── Character packs ──
    Err  remove_pack(const char* id);
    bool has_pack(const char* id);
    bool list_packs(std::vector<Info>& out);   // slot / z unused
    bool load_pack(const char* id, CharacterPack& out);

    static constexpr const char* BUILTIN = "yeti";
    static bool is_builtin(const char* character);
    // nullptr, "", "yeti" or a valid id.
    static bool valid_character(const char* character);

    // Bumped on every install/delete so renderers know to reload.
    uint32_t generation() const { return m_gen.load(); }
    uint32_t pack_generation() const { return m_pack_gen.load(); }

    // Partition usage in bytes (0/0 when not mounted).
    void usage(size_t& used, size_t& total);

    static constexpr const char* BASE  = "/fs";
    static constexpr const char* ITEMS_DIR = "/fs/items";
    static constexpr const char* PACKS_DIR = "/fs/chars";
    static constexpr const char* LABEL = "items";

private:
    struct Upload {
        bool        pack{false};
        std::string character;   // items: "" = the built-in Yeti
        std::string id;
        uint8_t*    buf{nullptr};
        size_t      size{0};
        size_t      got{0};
        uint32_t    crc{0};
        uint32_t    next_seq{0};
    };

    Err  begin_upload(const char* id, size_t size, uint32_t crc, size_t max, bool pack,
                      const char* character);
    static bool read_file(const std::string& path, std::vector<uint8_t>& out, size_t max);
    static std::string dir_for(const char* character);
    static std::string path_for(const char* id, const char* character);
    static std::string pack_path(const char* id);

    SemaphoreHandle_t     m_lock;
    bool                  m_mounted{false};
    Upload                m_up;
    std::atomic<uint32_t> m_gen{1};
    std::atomic<uint32_t> m_pack_gen{1};

    static constexpr const char* TAG = "ItemStore";
};

ItemStore& item_store();   // the device's single store

} // namespace Fuchey

// ============================================================
// Fuchey — ItemStore.cpp
// ============================================================

#include "ItemStore.hpp"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Fuchey {

namespace {
// IEEE 802.3 / zlib CRC32, same as the USB frames.
uint32_t crc32_of(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

struct Lock {
    SemaphoreHandle_t m;
    explicit Lock(SemaphoreHandle_t mm) : m(mm) { xSemaphoreTake(m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m); }
};
} // namespace

ItemStore& item_store() {
    static ItemStore store;
    return store;
}

const char* ItemStore::err_name(Err e) {
    switch (e) {
        case Err::OK:          return "ok";
        case Err::NOT_MOUNTED: return "no_storage";
        case Err::BAD_ID:      return "bad_id";
        case Err::TOO_BIG:     return "too_big";
        case Err::NO_UPLOAD:   return "no_upload";
        case Err::BAD_SEQ:     return "bad_seq";
        case Err::BAD_SIZE:    return "bad_size";
        case Err::BAD_CRC:     return "bad_crc";
        case Err::BAD_FORMAT:  return "bad_format";
        case Err::NOT_FOUND:   return "not_found";
        case Err::IO:          return "io_error";
        case Err::NO_MEMORY:   return "no_memory";
    }
    return "failed";
}

ItemStore::ItemStore() : m_lock(xSemaphoreCreateMutex()) {}

bool ItemStore::mount() {
    Lock l(m_lock);
    if (m_mounted) return true;
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = BASE;
    conf.partition_label        = LABEL;
    conf.format_if_mount_failed = true;
    conf.dont_mount             = false;
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    mkdir(ITEMS_DIR, 0775);   // EEXIST is fine
    mkdir(PACKS_DIR, 0775);
    m_mounted = true;
    size_t used = 0, total = 0;
    esp_littlefs_info(LABEL, &total, &used);
    ESP_LOGI(TAG, "Mounted %s: %u / %u KB used", BASE,
             static_cast<unsigned>(used / 1024), static_cast<unsigned>(total / 1024));
    return true;
}

void ItemStore::usage(size_t& used, size_t& total) {
    used = total = 0;
    if (m_mounted) esp_littlefs_info(LABEL, &total, &used);
}

bool ItemStore::is_builtin(const char* character) {
    return !character || character[0] == '\0' || strcmp(character, BUILTIN) == 0;
}

bool ItemStore::valid_character(const char* character) {
    return is_builtin(character) || WearItem::valid_id(character);
}

std::string ItemStore::dir_for(const char* character) {
    return is_builtin(character) ? std::string(ITEMS_DIR) : std::string(ITEMS_DIR) + "/" + character;
}

std::string ItemStore::path_for(const char* id, const char* character) {
    return dir_for(character) + "/" + id + ".bin";
}

std::string ItemStore::pack_path(const char* id) {
    return std::string(PACKS_DIR) + "/" + id + ".bin";
}

// ─── Upload ───────────────────────────────────────────────
void ItemStore::abort_upload() {
    if (m_up.buf) heap_caps_free(m_up.buf);
    m_up = Upload{};
}

ItemStore::Err ItemStore::begin_upload(const char* id, size_t size, uint32_t crc, size_t max,
                                       bool pack, const char* character) {
    Lock l(m_lock);
    if (!m_mounted) return Err::NOT_MOUNTED;
    if (!WearItem::valid_id(id) || !valid_character(character)) return Err::BAD_ID;
    if (size < 16 || size > max) return Err::TOO_BIG;
    abort_upload();
    m_up.buf = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (!m_up.buf) return Err::NO_MEMORY;
    m_up.pack = pack;
    m_up.character = is_builtin(character) ? "" : character;
    m_up.id   = id;
    m_up.size = size;
    m_up.crc  = crc;
    return Err::OK;
}

ItemStore::Err ItemStore::begin(const char* id, size_t size, uint32_t crc, const char* character) {
    return begin_upload(id, size, crc, WearItem::MAX_BYTES, false, character);
}

ItemStore::Err ItemStore::begin_pack(const char* id, size_t size, uint32_t crc) {
    // The Yeti is built in: no pack can take its id.
    if (id && is_builtin(id)) return Err::BAD_ID;
    return begin_upload(id, size, crc, CharacterPack::MAX_BYTES, true, nullptr);
}

ItemStore::Err ItemStore::chunk(const char* id, uint32_t seq, const uint8_t* data, size_t len) {
    Lock l(m_lock);
    if (!m_up.buf || !id || m_up.id != id) return Err::NO_UPLOAD;
    if (seq != m_up.next_seq) return Err::BAD_SEQ;
    if (len == 0 || m_up.got + len > m_up.size) {
        abort_upload();
        return Err::BAD_SIZE;
    }
    memcpy(m_up.buf + m_up.got, data, len);
    m_up.got += len;
    m_up.next_seq++;
    return Err::OK;
}

ItemStore::Err ItemStore::end(const char* id) {
    Lock l(m_lock);
    if (!m_up.buf || !id || m_up.id != id) return Err::NO_UPLOAD;
    Err result = Err::OK;
    WearItem parsed;
    std::string pack_id;
    const char* character = m_up.character.empty() ? nullptr : m_up.character.c_str();
    if (m_up.got != m_up.size) {
        result = Err::BAD_SIZE;
    } else if (crc32_of(m_up.buf, m_up.size) != m_up.crc) {
        result = Err::BAD_CRC;
    } else if (m_up.pack ? !CharacterPack::validate(m_up.buf, m_up.size, &pack_id) || pack_id != m_up.id
                         : !WearItem::parse(m_up.buf, m_up.size, parsed) || parsed.id != m_up.id) {
        result = Err::BAD_FORMAT;
    } else {
        const std::string dir = m_up.pack ? std::string(PACKS_DIR) : dir_for(character);
        mkdir(dir.c_str(), 0775);            // a new character's folder; EEXIST is fine
        const std::string tmp = dir + "/" + id + ".tmp";
        const std::string dst = m_up.pack ? pack_path(id) : path_for(id, character);
        FILE* f = fopen(tmp.c_str(), "wb");
        bool ok = f && fwrite(m_up.buf, 1, m_up.size, f) == m_up.size;
        if (f) ok = (fclose(f) == 0) && ok;
        if (ok) {
            unlink(dst.c_str());             // LittleFS rename won't overwrite
            ok = rename(tmp.c_str(), dst.c_str()) == 0;
        }
        if (!ok) {
            unlink(tmp.c_str());
            result = Err::IO;
        } else if (m_up.pack) {
            m_pack_gen.fetch_add(1);
            ESP_LOGI(TAG, "Installed character %s (%u bytes)", id, static_cast<unsigned>(m_up.size));
        } else {
            m_gen.fetch_add(1);
            ESP_LOGI(TAG, "Installed %s for %s (%u bytes, slot %s, %u+%u runs)", id,
                     character ? character : BUILTIN, static_cast<unsigned>(m_up.size),
                     WEAR_SLOTS[parsed.slot], static_cast<unsigned>(parsed.front.size()),
                     static_cast<unsigned>(parsed.back.size()));
        }
    }
    abort_upload();
    return result;
}

// ─── Files ────────────────────────────────────────────────
ItemStore::Err ItemStore::remove(const char* id, const char* character) {
    Lock l(m_lock);
    if (!m_mounted) return Err::NOT_MOUNTED;
    if (!WearItem::valid_id(id) || !valid_character(character)) return Err::BAD_ID;
    if (unlink(path_for(id, character).c_str()) != 0) return Err::NOT_FOUND;
    m_gen.fetch_add(1);
    ESP_LOGI(TAG, "Deleted %s for %s", id, is_builtin(character) ? BUILTIN : character);
    return Err::OK;
}

bool ItemStore::exists(const char* id, const char* character) {
    Lock l(m_lock);
    if (!m_mounted || !WearItem::valid_id(id) || !valid_character(character)) return false;
    struct stat st{};
    return stat(path_for(id, character).c_str(), &st) == 0;
}

bool ItemStore::read_file(const std::string& path, std::vector<uint8_t>& out, size_t max) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
        if (out.size() > max) break;
    }
    fclose(f);
    return out.size() <= max;
}

bool ItemStore::load(const char* id, WearItem& out, const char* character) {
    Lock l(m_lock);
    if (!m_mounted || !WearItem::valid_id(id) || !valid_character(character)) return false;
    std::vector<uint8_t> bytes;
    return read_file(path_for(id, character), bytes, WearItem::MAX_BYTES) &&
           WearItem::parse(bytes.data(), bytes.size(), out) && out.id == id;
}

bool ItemStore::list(std::vector<Info>& out, const char* character) {
    Lock l(m_lock);
    out.clear();
    if (!m_mounted || !valid_character(character)) return false;
    const std::string dir = dir_for(character);
    ::DIR* d = opendir(dir.c_str());
    if (!d) return !is_builtin(character);   // no folder yet = nothing installed
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".bin") != 0) continue;
        const std::string id = name.substr(0, name.size() - 4);
        std::vector<uint8_t> bytes;
        WearItem item;
        if (!read_file(dir + "/" + name, bytes, WearItem::MAX_BYTES) ||
            !WearItem::parse(bytes.data(), bytes.size(), item)) {
            continue;   // unreadable: not listed, so the app re-installs it
        }
        out.push_back({id, item.slot, item.z, static_cast<uint32_t>(bytes.size()),
                       crc32_of(bytes.data(), bytes.size())});
    }
    closedir(d);
    return true;
}

// ─── Character packs ──────────────────────────────────────
ItemStore::Err ItemStore::remove_pack(const char* id) {
    Lock l(m_lock);
    if (!m_mounted) return Err::NOT_MOUNTED;
    if (!WearItem::valid_id(id) || is_builtin(id)) return Err::BAD_ID;
    if (unlink(pack_path(id).c_str()) != 0) return Err::NOT_FOUND;
    m_pack_gen.fetch_add(1);
    ESP_LOGI(TAG, "Deleted character %s", id);
    return Err::OK;
}

bool ItemStore::has_pack(const char* id) {
    Lock l(m_lock);
    if (!m_mounted || !WearItem::valid_id(id) || is_builtin(id)) return false;
    struct stat st{};
    return stat(pack_path(id).c_str(), &st) == 0;
}

bool ItemStore::list_packs(std::vector<Info>& out) {
    Lock l(m_lock);
    out.clear();
    if (!m_mounted) return false;
    ::DIR* d = opendir(PACKS_DIR);
    if (!d) return true;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".bin") != 0) continue;
        // CRC of the stored file, read in pieces (packs are large); the app
        // compares it to decide whether to re-send. load_pack validates.
        FILE* f = fopen((std::string(PACKS_DIR) + "/" + name).c_str(), "rb");
        if (!f) continue;
        uint32_t crc = 0xFFFFFFFFu, bytes = 0;
        uint8_t buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            for (size_t i = 0; i < n; ++i) {
                crc ^= buf[i];
                for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            bytes += n;
        }
        fclose(f);
        out.push_back({name.substr(0, name.size() - 4), 0, 0, bytes, ~crc});
    }
    closedir(d);
    return true;
}

bool ItemStore::load_pack(const char* id, CharacterPack& out) {
    Lock l(m_lock);
    if (!m_mounted || !WearItem::valid_id(id) || is_builtin(id)) return false;
    FILE* f = fopen(pack_path(id).c_str(), "rb");
    if (!f) return false;
    struct stat st{};
    const bool sized = fstat(fileno(f), &st) == 0 && st.st_size > 0 &&
                       static_cast<size_t>(st.st_size) <= CharacterPack::MAX_BYTES;
    const size_t len = sized ? static_cast<size_t>(st.st_size) : 0;
    uint8_t* buf = sized ? static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM)) : nullptr;
    const bool read = buf && fread(buf, 1, len, f) == len;
    fclose(f);
    if (!read || !out.adopt(buf, len)) {   // adopt owns buf only on success
        if (buf) heap_caps_free(buf);
        ESP_LOGW(TAG, "Character %s: unreadable pack", id);
        return false;
    }
    return out.id() == id;
}

} // namespace Fuchey

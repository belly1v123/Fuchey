#pragma once
// ============================================================
// Fuchey — WalletCreateSession.hpp
// "Create wallet" started from the companion app, with the recovery
// words shown ONLY on the Fuchey screen.
//
//   INTRO   → warning screen; B4 shows the words
//   WORDS   → 4 words per page (B4 next, B3 back); after the last
//             page the user confirms 3 random words
//   VERIFY  → "Confirm word #n": the device shows 8 words in a
//             shuffled 3×3 grid; the app sends only the clicked
//             position. A wrong pick sends the user back to the words.
//   VERIFIED→ the caller imports the phrase into WalletCore, then
//             calls finish() → DONE
//
// Nothing is stored until VERIFIED. The phrase lives in RAM only and
// is wiped on finish / cancel / restart. Thread-safe: the UI task
// (buttons, drawing) and the console task (app commands) share it.
// ============================================================

#include <cstdint>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace Fuchey {

class WalletCreateSession {
public:
    enum class Stage : uint8_t { IDLE, INTRO, WORDS, VERIFY, VERIFIED, DONE, CANCELLED, FAILED };

    static constexpr int WORDS_PER_PAGE = 4;
    static constexpr int VERIFY_COUNT   = 3;

    // Snapshot for drawing. `words` holds ONLY the current page and is
    // filled only for the device UI (never for the USB protocol).
    struct View {
        Stage   stage{Stage::IDLE};
        uint8_t total{0};          // 12 or 24
        uint8_t page{0}, pages{0}; // WORDS
        uint8_t first{0};          // 1-based number of words[0]
        uint8_t verify_n{0};       // 1..VERIFY_COUNT
        uint8_t verify_word{0};    // 1-based position being confirmed
        bool    wrong{false};      // last verify pick was wrong
        char    words[WORDS_PER_PAGE][12]{};
        char    cells[9][12]{};
        char    address[48]{};
    };

    WalletCreateSession();
    ~WalletCreateSession();

    bool begin(int total_words);     // generates a fresh phrase (RAM only)
    void next();                     // device B4
    void prev();                     // device B3
    void cancel();                   // device B1 / app / timeout
    bool tap(int pos);               // VERIFY: app grid position 0..8
    // VERIFIED → copy of the phrase for WalletCore::import (caller wipes it).
    std::string phrase_if_verified();
    void finish(bool ok, const std::string& address);   // VERIFIED → DONE/FAILED
    void reset_if_terminal();        // DONE/CANCELLED/FAILED → IDLE

    View view(bool with_secrets) const;
    Stage stage() const;
    uint32_t last_activity_ms() const;

private:
    mutable SemaphoreHandle_t m_lock;
    Stage       m_stage{Stage::IDLE};
    int         m_total{12};
    int         m_page{0};
    std::string m_phrase;            // space-separated words
    int         m_word_idx[24]{};    // BIP39 index of each word
    int         m_verify_pos[VERIFY_COUNT]{};   // 0-based word positions to confirm
    int         m_verify_n{0};
    bool        m_wrong{false};
    int         m_cells[9]{};        // BIP39 index per grid cell, -1 = empty
    std::string m_address;
    uint32_t    m_last_ms{0};

    void wipe_locked();
    void setup_verify_locked();      // choose positions, first grid
    void shuffle_cells_locked();     // grid for the current verify word
    void touch_locked();
};

} // namespace Fuchey

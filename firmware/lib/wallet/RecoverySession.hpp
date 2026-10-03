#pragma once
// ============================================================
// Fuchey — RecoverySession.hpp
// Scrambled-grid entry of a BIP39 recovery phrase (Trezor-style).
//
// The DEVICE shows a 3×3 grid; the companion app shows 9 blank
// buttons and only ever sends a position 0..8. The layout is
// reshuffled after every tap, so the host never learns letters or
// words.
//
//   letters mode : 8 phone-keypad groups (abc … wxyz) + ⌫
//   words mode   : up to 8 candidate words + ⌫ (once ≤ 8 remain), or —
//                  when a short word is fully typed but longer words share
//                  its prefix ("act" / "action") — those exact words + "…"
//                  (keep typing) + ⌫
//
// Pure logic (no ESP-IDF): words come from a getter and randomness
// from a callback, so it is unit-testable on a PC.
// ============================================================

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Fuchey {

class RecoverySession {
public:
    static constexpr int CELLS      = 9;
    static constexpr int WORDS_MODE_MAX = 8;   // switch to words at ≤ 8 candidates
    static constexpr int MAX_WORDS  = 24;

    enum class Mode : uint8_t { LETTERS, WORDS };

    using WordFn   = std::function<const char*(int)>;   // index → word (0..count-1)
    using RandomFn = std::function<uint32_t()>;

    RecoverySession(WordFn word_at, int wordlist_size, RandomFn rnd);
    ~RecoverySession() { wipe(); }

    void start(int total_words);
    void wipe();                         // zero all entered data

    // Apply a tap at position 0..8. Returns false if the cell is empty
    // (ignored). Reshuffles the grid after every accepted tap.
    bool tap(int pos);

    bool        active() const      { return m_active; }
    bool        complete() const    { return m_active && static_cast<int>(m_words.size()) == m_total; }
    Mode        mode() const        { return m_mode; }
    int         word_number() const { return static_cast<int>(m_words.size()) + 1; }  // 1-based
    int         total() const       { return m_total; }

    // On-device display only (never sent to the host).
    std::string cell_label(int pos) const;
    std::string typed_groups() const;    // e.g. "abc-def"

    // The phrase, space-separated. Caller must wipe its copy.
    std::string phrase() const;

    // On-device display only.
    int         entered_count() const { return static_cast<int>(m_words.size()); }
    std::string entered_word(int i) const;
    std::string last_word() const;

private:
    WordFn   m_word_at;
    int      m_count;
    RandomFn m_rnd;

    bool m_active{false};
    int  m_total{12};
    Mode m_mode{Mode::LETTERS};
    std::vector<int>     m_words;        // chosen word indices
    std::vector<uint8_t> m_groups;       // keypad groups typed for the current word
    std::vector<int>     m_candidates;   // word indices matching m_groups (prefix)
    std::vector<int>     m_shown;        // words offered in words mode
    bool                 m_more{false};  // words mode also offers "…" (keep typing)
    bool                 m_force_letters{false};  // user chose "…" for this step
    // Grid: what each cell holds. Letters mode: 0..7 group, 8 = backspace,
    // -1 = empty. Words mode: index into m_shown, 100 = backspace,
    // 101 = "…" keep typing, -1 = empty.
    std::array<int, CELLS> m_cells{};

    static int group_of(char c);         // 'a'..'z' → 0..7, else -1
    void recompute_candidates();
    void reshuffle();
};

} // namespace Fuchey

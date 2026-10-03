// ============================================================
// Fuchey — RecoverySession.cpp
// ============================================================

#include "RecoverySession.hpp"
#include <algorithm>
#include <cstring>

namespace Fuchey {

namespace {
constexpr const char* GROUPS[8] = {"abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"};
constexpr int BACKSPACE_LETTERS = 8;
constexpr int BACKSPACE_WORDS   = 100;
constexpr int MORE_LETTERS      = 101;
}

RecoverySession::RecoverySession(WordFn word_at, int wordlist_size, RandomFn rnd)
    : m_word_at(std::move(word_at)), m_count(wordlist_size), m_rnd(std::move(rnd)) {
    m_cells.fill(-1);
}

int RecoverySession::group_of(char c) {
    for (int g = 0; g < 8; ++g) {
        if (std::strchr(GROUPS[g], c) != nullptr && c != '\0') return g;
    }
    return -1;
}

void RecoverySession::start(int total_words) {
    wipe();
    m_total  = (total_words == 24) ? 24 : 12;
    m_active = true;
    recompute_candidates();
    reshuffle();
}

void RecoverySession::wipe() {
    std::fill(m_words.begin(), m_words.end(), 0);
    std::fill(m_groups.begin(), m_groups.end(), 0);
    std::fill(m_candidates.begin(), m_candidates.end(), 0);
    std::fill(m_shown.begin(), m_shown.end(), 0);
    m_words.clear();
    m_groups.clear();
    m_candidates.clear();
    m_shown.clear();
    m_more = false;
    m_force_letters = false;
    m_cells.fill(-1);
    m_mode   = Mode::LETTERS;
    m_active = false;
}

void RecoverySession::recompute_candidates() {
    m_candidates.clear();
    for (int i = 0; i < m_count; ++i) {
        const char* w = m_word_at(i);
        const size_t n = std::strlen(w);
        if (n < m_groups.size()) continue;
        bool ok = true;
        for (size_t k = 0; k < m_groups.size() && ok; ++k) {
            ok = group_of(w[k]) == m_groups[k];
        }
        if (ok) m_candidates.push_back(i);
    }
    m_shown.clear();
    m_more = false;
    m_mode = Mode::LETTERS;
    if (m_groups.empty() || m_force_letters) return;
    if (static_cast<int>(m_candidates.size()) <= WORDS_MODE_MAX) {
        m_shown = m_candidates;
        m_mode = Mode::WORDS;
        return;
    }
    // Too many to list, but some words are already fully typed ("act"):
    // offer those, plus "…" to keep typing for a longer word.
    for (int idx : m_candidates) {
        if (std::strlen(m_word_at(idx)) == m_groups.size()) m_shown.push_back(idx);
    }
    if (!m_shown.empty() && static_cast<int>(m_shown.size()) <= WORDS_MODE_MAX - 1) {
        m_more = true;
        m_mode = Mode::WORDS;
    } else {
        m_shown.clear();
    }
}

void RecoverySession::reshuffle() {
    std::array<int, CELLS> items{};
    items.fill(-1);
    int n = 0;
    if (m_mode == Mode::LETTERS) {
        // Only groups that still lead to at least one word, plus backspace.
        for (int g = 0; g < 8; ++g) {
            bool possible = false;
            for (int idx : m_candidates) {
                const char* w = m_word_at(idx);
                if (std::strlen(w) > m_groups.size() && group_of(w[m_groups.size()]) == g) {
                    possible = true;
                    break;
                }
            }
            if (possible) items[n++] = g;
        }
        items[n++] = BACKSPACE_LETTERS;
    } else {
        for (size_t i = 0; i < m_shown.size(); ++i) items[n++] = static_cast<int>(i);
        if (m_more) items[n++] = MORE_LETTERS;
        items[n++] = BACKSPACE_WORDS;
    }
    // Fisher–Yates over all 9 cells (empties move too).
    for (int i = CELLS - 1; i > 0; --i) {
        const int j = static_cast<int>(m_rnd() % static_cast<uint32_t>(i + 1));
        std::swap(items[i], items[j]);
    }
    m_cells = items;
}

bool RecoverySession::tap(int pos) {
    if (!m_active || complete() || pos < 0 || pos >= CELLS) return false;
    const int item = m_cells[pos];
    if (item < 0) return false;

    if (m_mode == Mode::LETTERS) {
        if (item == BACKSPACE_LETTERS) {
            if (!m_groups.empty()) {
                m_groups.pop_back();
            } else if (!m_words.empty()) {
                m_words.pop_back();      // edit the previous word again
            }
        } else {
            m_groups.push_back(static_cast<uint8_t>(item));
        }
        m_force_letters = false;
    } else {
        if (item == BACKSPACE_WORDS) {
            if (!m_groups.empty()) m_groups.pop_back();
            m_force_letters = false;
        } else if (item == MORE_LETTERS) {
            m_force_letters = true;      // stay in letters mode for this step
        } else {
            m_words.push_back(m_shown[static_cast<size_t>(item)]);
            m_groups.clear();
            m_force_letters = false;
        }
    }
    recompute_candidates();
    if (complete()) {
        m_cells.fill(-1);
    } else {
        reshuffle();
    }
    return true;
}

std::string RecoverySession::cell_label(int pos) const {
    if (pos < 0 || pos >= CELLS) return {};
    const int item = m_cells[pos];
    if (item < 0) return {};
    if (m_mode == Mode::LETTERS) {
        return item == BACKSPACE_LETTERS ? "<-" : GROUPS[item];
    }
    if (item == BACKSPACE_WORDS) return "<-";
    if (item == MORE_LETTERS) return "...";
    return m_word_at(m_shown[static_cast<size_t>(item)]);
}

std::string RecoverySession::typed_groups() const {
    std::string s;
    for (size_t i = 0; i < m_groups.size(); ++i) {
        if (i) s += '-';
        s += GROUPS[m_groups[i]];
    }
    return s;
}

std::string RecoverySession::phrase() const {
    std::string s;
    for (size_t i = 0; i < m_words.size(); ++i) {
        if (i) s += ' ';
        s += m_word_at(m_words[i]);
    }
    return s;
}

} // namespace Fuchey

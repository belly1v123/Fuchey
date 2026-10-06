// ============================================================
// Fuchey — WalletCreateSession.cpp
// ============================================================

#include "WalletCreateSession.hpp"
#include "../crypto/BIP39.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Fuchey {

namespace {
struct Lock {
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    SemaphoreHandle_t m_;
};
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
}

WalletCreateSession::WalletCreateSession() : m_lock(xSemaphoreCreateMutex()) {
    std::fill(std::begin(m_cells), std::end(m_cells), -1);
}

WalletCreateSession::~WalletCreateSession() {
    wipe_locked();
}

void WalletCreateSession::touch_locked() { m_last_ms = now_ms(); }

void WalletCreateSession::wipe_locked() {
    std::fill(m_phrase.begin(), m_phrase.end(), '\0');
    m_phrase.clear();
    std::fill(std::begin(m_word_idx), std::end(m_word_idx), 0);
    std::fill(std::begin(m_verify_pos), std::end(m_verify_pos), 0);
    std::fill(std::begin(m_cells), std::end(m_cells), -1);
    m_verify_n = 0;
    m_page = 0;
    m_wrong = false;
}

bool WalletCreateSession::begin(int total_words) {
    Lock l(m_lock);
    wipe_locked();
    m_address.clear();
    m_total = (total_words == 24) ? 24 : 12;
    auto phrase = Crypto::BIP39::generate(m_total);   // hardware RNG
    if (!phrase) { m_stage = Stage::FAILED; return false; }
    m_phrase = std::move(*phrase);
    // Word indices (for decoys / display).
    int n = 0;
    size_t start = 0;
    while (start <= m_phrase.size() && n < m_total) {
        size_t end = m_phrase.find(' ', start);
        if (end == std::string::npos) end = m_phrase.size();
        m_word_idx[n++] = Crypto::BIP39::find_word(std::string_view(m_phrase).substr(start, end - start));
        start = end + 1;
    }
    m_stage = (n == m_total) ? Stage::INTRO : Stage::FAILED;
    if (m_stage == Stage::FAILED) wipe_locked();
    touch_locked();
    return m_stage == Stage::INTRO;
}

void WalletCreateSession::next() {
    Lock l(m_lock);
    const int pages = m_total / WORDS_PER_PAGE;
    if (m_stage == Stage::INTRO) {
        m_stage = Stage::WORDS;
        m_page = 0;
    } else if (m_stage == Stage::WORDS) {
        if (m_page + 1 < pages) {
            ++m_page;
        } else {
            setup_verify_locked();
            m_stage = Stage::VERIFY;
        }
    }
    touch_locked();
}

void WalletCreateSession::prev() {
    Lock l(m_lock);
    if (m_stage == Stage::WORDS && m_page > 0) --m_page;
    else if (m_stage == Stage::VERIFY) { m_stage = Stage::WORDS; m_page = m_total / WORDS_PER_PAGE - 1; }
    touch_locked();
}

void WalletCreateSession::cancel() {
    Lock l(m_lock);
    if (m_stage == Stage::IDLE || m_stage == Stage::DONE) return;
    wipe_locked();
    m_stage = Stage::CANCELLED;
    touch_locked();
}

void WalletCreateSession::setup_verify_locked() {
    // VERIFY_COUNT distinct random word positions, in ascending order.
    int chosen = 0;
    while (chosen < VERIFY_COUNT) {
        const int p = static_cast<int>(esp_random() % static_cast<uint32_t>(m_total));
        bool dup = false;
        for (int i = 0; i < chosen; ++i) dup |= (m_verify_pos[i] == p);
        if (!dup) m_verify_pos[chosen++] = p;
    }
    std::sort(m_verify_pos, m_verify_pos + VERIFY_COUNT);
    m_verify_n = 0;
    m_wrong = false;
    shuffle_cells_locked();
}

void WalletCreateSession::shuffle_cells_locked() {
    // The right word + 7 distinct decoys + 1 empty cell, shuffled.
    const int right = m_word_idx[m_verify_pos[m_verify_n]];
    int items[9];
    int n = 0;
    items[n++] = right;
    while (n < 8) {
        const int w = static_cast<int>(esp_random() % Crypto::BIP39::WORDLIST_SIZE);
        bool dup = false;
        for (int i = 0; i < n; ++i) dup |= (items[i] == w);
        if (!dup) items[n++] = w;
    }
    items[8] = -1;
    for (int i = 8; i > 0; --i) {
        const int j = static_cast<int>(esp_random() % static_cast<uint32_t>(i + 1));
        std::swap(items[i], items[j]);
    }
    std::copy(items, items + 9, m_cells);
}

bool WalletCreateSession::tap(int pos) {
    Lock l(m_lock);
    if (m_stage != Stage::VERIFY || pos < 0 || pos > 8 || m_cells[pos] < 0) return false;
    touch_locked();
    if (m_cells[pos] != m_word_idx[m_verify_pos[m_verify_n]]) {
        // Wrong: back to the words so the user re-checks the paper.
        m_wrong = true;
        m_stage = Stage::WORDS;
        m_page = 0;
        return true;
    }
    m_wrong = false;
    if (++m_verify_n >= VERIFY_COUNT) {
        m_stage = Stage::VERIFIED;
        std::fill(std::begin(m_cells), std::end(m_cells), -1);
    } else {
        shuffle_cells_locked();
    }
    return true;
}

std::string WalletCreateSession::phrase_if_verified() {
    Lock l(m_lock);
    return m_stage == Stage::VERIFIED ? m_phrase : std::string{};
}

void WalletCreateSession::finish(bool ok, const std::string& address) {
    Lock l(m_lock);
    if (m_stage != Stage::VERIFIED) return;
    wipe_locked();
    m_address = ok ? address : std::string{};
    m_stage = ok ? Stage::DONE : Stage::FAILED;
    touch_locked();
}

void WalletCreateSession::reset_if_terminal() {
    Lock l(m_lock);
    if (m_stage == Stage::DONE || m_stage == Stage::CANCELLED || m_stage == Stage::FAILED) {
        wipe_locked();
        m_address.clear();
        m_stage = Stage::IDLE;
    }
}

WalletCreateSession::View WalletCreateSession::view(bool with_secrets) const {
    Lock l(m_lock);
    View v;
    v.stage = m_stage;
    v.total = static_cast<uint8_t>(m_total);
    v.page = static_cast<uint8_t>(m_page);
    v.pages = static_cast<uint8_t>(m_total / WORDS_PER_PAGE);
    v.first = static_cast<uint8_t>(m_page * WORDS_PER_PAGE + 1);
    v.verify_n = static_cast<uint8_t>(m_verify_n + 1);
    v.verify_word = (m_stage == Stage::VERIFY)
                        ? static_cast<uint8_t>(m_verify_pos[m_verify_n] + 1) : 0;
    v.wrong = m_wrong;
    snprintf(v.address, sizeof(v.address), "%s", m_address.c_str());
    if (with_secrets) {
        if (m_stage == Stage::WORDS) {
            for (int i = 0; i < WORDS_PER_PAGE; ++i) {
                const int w = m_page * WORDS_PER_PAGE + i;
                snprintf(v.words[i], sizeof(v.words[i]), "%s",
                         w < m_total ? Crypto::BIP39::get_word(m_word_idx[w]) : "");
            }
        }
        if (m_stage == Stage::VERIFY) {
            for (int i = 0; i < 9; ++i) {
                snprintf(v.cells[i], sizeof(v.cells[i]), "%s",
                         m_cells[i] >= 0 ? Crypto::BIP39::get_word(m_cells[i]) : "");
            }
        }
    }
    return v;
}

WalletCreateSession::Stage WalletCreateSession::stage() const {
    Lock l(m_lock);
    return m_stage;
}

uint32_t WalletCreateSession::last_activity_ms() const {
    Lock l(m_lock);
    return m_last_ms;
}

} // namespace Fuchey

// ============================================================
// Fuchey — RecoveryController.cpp
// ============================================================

#include "RecoveryController.hpp"
#include "../crypto/BIP39.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include <cstdio>

namespace Fuchey {

namespace {
struct Lock {
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    SemaphoreHandle_t m_;
};
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
}

RecoveryController::RecoveryController()
    : m_lock(xSemaphoreCreateMutex()),
      m_s([](int i) { return Crypto::BIP39::get_word(i); }, Crypto::BIP39::WORDLIST_SIZE,
          [] { return esp_random(); }) {}

void RecoveryController::start(bool restore, int total_words) {
    Lock l(m_lock);
    m_restore = restore;
    m_result  = Result::NONE;
    m_s.start(total_words);
    ++m_layout;
    m_last_ms = now_ms();
}

RecoveryController::TapStatus RecoveryController::tap(int pos) {
    Lock l(m_lock);
    if (!m_s.active() || m_result != Result::NONE) return TapStatus::NO_SESSION;
    if (m_rendered != m_layout) return TapStatus::STALE;   // screen not showing this layout yet
    m_last_ms = now_ms();
    if (m_s.tap(pos)) ++m_layout;                          // reshuffled → new layout
    return TapStatus::OK;
}

bool RecoveryController::complete() const {
    Lock l(m_lock);
    return m_s.complete();
}

std::string RecoveryController::phrase() const {
    Lock l(m_lock);
    return m_s.complete() ? m_s.phrase() : std::string{};
}

void RecoveryController::set_result(Result r) {
    Lock l(m_lock);
    m_result = r;
    // Keep the recorded words only for the on-device review of a bad phrase.
    if (r != Result::BAD_CHECKSUM) m_s.wipe();
    ++m_layout;
    m_last_ms = now_ms();
}

void RecoveryController::cancel() {
    Lock l(m_lock);
    if (!m_s.active() && m_result == Result::NONE) return;
    m_s.wipe();
    m_result = Result::CANCELLED;
    ++m_layout;
    m_last_ms = now_ms();
}

void RecoveryController::dismiss() {
    Lock l(m_lock);
    m_s.wipe();
    m_result = Result::NONE;
    ++m_layout;
}

void RecoveryController::mark_rendered(uint32_t layout) {
    Lock l(m_lock);
    m_rendered = layout;
}

RecoveryController::View RecoveryController::view(bool with_secrets) const {
    Lock l(m_lock);
    View v;
    v.active     = m_s.active() && m_result == Result::NONE;
    v.restore    = m_restore;
    v.words_mode = m_s.mode() == RecoverySession::Mode::WORDS;
    v.word       = static_cast<uint8_t>(m_s.word_number());
    v.total      = static_cast<uint8_t>(m_s.total());
    v.result     = m_result;
    v.layout     = m_layout;
    if (with_secrets) {
        snprintf(v.typed, sizeof(v.typed), "%s", m_s.typed_groups().c_str());
        snprintf(v.prev, sizeof(v.prev), "%s", m_s.last_word().c_str());
        for (int i = 0; i < RecoverySession::CELLS; ++i) {
            snprintf(v.cells[i], sizeof(v.cells[i]), "%s", m_s.cell_label(i).c_str());
        }
        if (m_result == Result::BAD_CHECKSUM) {
            const int n = m_s.entered_count();
            v.review_count = static_cast<uint8_t>(n);
            for (int i = 0; i < n && i < MAX_WORDS; ++i) {
                snprintf(v.review[i], sizeof(v.review[i]), "%s", m_s.entered_word(i).c_str());
            }
        }
    }
    return v;
}

bool RecoveryController::active() const {
    Lock l(m_lock);
    return m_s.active() && m_result == Result::NONE;
}

bool RecoveryController::restore() const {
    Lock l(m_lock);
    return m_restore;
}

uint32_t RecoveryController::last_activity_ms() const {
    Lock l(m_lock);
    return m_last_ms;
}

} // namespace Fuchey

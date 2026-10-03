#pragma once
// ============================================================
// Fuchey — RecoveryController.hpp
// Shared, thread-safe owner of a scrambled-grid recovery session.
//
// UsbProtocol (console task) applies the app's taps; UIManager (UI
// task) draws straight from here — no copies through the event queue,
// so the screen always shows the live layout.
//
// Stale-click guard: every layout gets an id. The UI reports the id it
// has actually pushed to the panel (mark_rendered). A tap is accepted
// only if the screen is showing the current layout; otherwise the user
// clicked while looking at an outdated grid and the tap is rejected.
// ============================================================

#include "RecoverySession.hpp"
#include <cstdint>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace Fuchey {

class RecoveryController {
public:
    enum class Result : uint8_t { NONE, MATCH, MISMATCH, BAD_CHECKSUM, RESTORED, FAILED, CANCELLED };
    enum class TapStatus : uint8_t { OK, STALE, NO_SESSION };

    static constexpr int MAX_WORDS = RecoverySession::MAX_WORDS;

    struct View {
        bool     active{false};
        bool     restore{false};
        bool     words_mode{false};
        uint8_t  word{0}, total{0};
        Result   result{Result::NONE};
        uint32_t layout{0};
        char     typed[28]{};
        char     prev[12]{};              // last picked word (device only)
        char     cells[9][12]{};          // device only
        uint8_t  review_count{0};         // BAD_CHECKSUM: words recorded
        char     review[MAX_WORDS][10]{}; // device only
    };

    RecoveryController();

    void      start(bool restore, int total_words);
    TapStatus tap(int pos);
    bool      complete() const;
    std::string phrase() const;          // caller wipes its copy
    void      set_result(Result r);      // ends entry (keeps words for review on BAD_CHECKSUM)
    void      cancel();                  // ends + wipes (device B1, app, timeout)
    void      dismiss();                 // clears the result / review (wipes)
    void      mark_rendered(uint32_t layout);

    View      view(bool with_secrets) const;
    bool      active() const;
    bool      restore() const;
    uint32_t  last_activity_ms() const;

private:
    mutable SemaphoreHandle_t m_lock;
    RecoverySession m_s;
    bool     m_restore{false};
    Result   m_result{Result::NONE};
    uint32_t m_layout{0};
    uint32_t m_rendered{0};
    uint32_t m_last_ms{0};
};

} // namespace Fuchey

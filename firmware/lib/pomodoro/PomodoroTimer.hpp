#pragma once
// ============================================================
// Fuchey — PomodoroTimer.hpp
// Pure Pomodoro settings + countdown state machine.
// No display / buzzer / FreeRTOS dependencies — unit-testable.
// All time is in milliseconds; the caller supplies `now_ms`
// (esp_timer_get_time()/1000) to start()/tick().
// ============================================================

#include <cstdint>

namespace Fuchey {

struct PomodoroSettings {
    uint8_t work_min{25};
    uint8_t work_sec{0};
    uint8_t brk_min{5};
    uint8_t brk_sec{0};
    uint8_t loops{1};
};

class PomodoroTimer {
public:
    enum class Phase : uint8_t {
        SetWork,    // B4 = +min, B3 = +sec (work duration)
        SetBreak,   // B4 = +min, B3 = +sec (break duration, 0 = skip)
        SetLoops,   // B3/B4 = +1 loop (1..MAX_LOOPS)
        Ready,      // B3/B4 = +1 loop, B2 = start
        RunWork,    // counting down a work session
        RunBreak,   // counting down a break
        Paused,     // frozen RunWork / RunBreak (see paused_from())
        Done,       // all loops finished
    };

    enum class TickEvent : uint8_t {
        None,
        WorkFinished,   // work session ended -> break or next loop / done
        BreakFinished,  // break ended -> next work session
        AllFinished,    // final work session ended -> Done
    };

    static constexpr uint8_t kMaxMin   = 60;
    static constexpr uint8_t kMaxLoops = 5;

    PomodoroTimer() { reset(); }

    void reset() {
        m_s = PomodoroSettings{};
        m_phase = Phase::SetWork;
        m_loop = 1;
        m_remain_ms = 0;
        m_deadline_ms = 0;
        m_paused_from = Phase::RunWork;
    }

    // ── Accessors ──────────────────────────────────────────
    Phase phase() const              { return m_phase; }
    const PomodoroSettings& settings() const { return m_s; }
    uint8_t loop_index() const       { return m_loop; }  // 1-based while running
    uint32_t remaining_ms() const    { return m_remain_ms; }
    Phase paused_from() const        { return m_paused_from; }

    bool is_setup() const {
        return m_phase == Phase::SetWork || m_phase == Phase::SetBreak ||
               m_phase == Phase::SetLoops || m_phase == Phase::Ready;
    }
    bool is_running() const {
        return m_phase == Phase::RunWork || m_phase == Phase::RunBreak;
    }

    static uint32_t total_ms(uint8_t m, uint8_t s) {
        return static_cast<uint32_t>(m) * 60000u + static_cast<uint32_t>(s) * 1000u;
    }
    uint32_t work_ms()  const { return total_ms(m_s.work_min, m_s.work_sec); }
    uint32_t break_ms() const { return total_ms(m_s.brk_min, m_s.brk_sec); }

    // ── Setup: B4 (right) = +1, B3 (left) = field step ──
    // In time phases B3 steps seconds up; in SetLoops B3 steps loops down.
    void adjust_min() {
        if (m_phase == Phase::SetWork) {
            m_s.work_min = static_cast<uint8_t>((m_s.work_min + 1) % (kMaxMin + 1));
            if (m_s.work_min == kMaxMin) m_s.work_sec = 0;  // cap 60:00
        } else if (m_phase == Phase::SetBreak) {
            m_s.brk_min = static_cast<uint8_t>((m_s.brk_min + 1) % (kMaxMin + 1));
            if (m_s.brk_min == kMaxMin) m_s.brk_sec = 0;
        }
    }
    void adjust_sec() {
        if (m_phase == Phase::SetWork) {
            if (m_s.work_min == kMaxMin) { m_s.work_sec = 0; return; }
            m_s.work_sec = static_cast<uint8_t>((m_s.work_sec + 1) % 60);
        } else if (m_phase == Phase::SetBreak) {
            if (m_s.brk_min == kMaxMin) { m_s.brk_sec = 0; return; }
            m_s.brk_sec = static_cast<uint8_t>((m_s.brk_sec + 1) % 60);
        }
    }
    // Loop count stepper, wraps 1..kMaxLoops in either direction.
    void adjust_loops(int8_t dir) {
        if (m_phase != Phase::SetLoops) return;
        m_s.loops = static_cast<uint8_t>((m_s.loops - 1 + dir + kMaxLoops) % kMaxLoops + 1);
    }

    // ── Setup navigation ───────────────────────────────────
    // confirm(): advance one setup step. Ready is terminal (use start()).
    void confirm() {
        switch (m_phase) {
            case Phase::SetWork:  m_phase = Phase::SetBreak; break;
            case Phase::SetBreak: m_phase = Phase::SetLoops; break;
            case Phase::SetLoops: m_phase = Phase::Ready;    break;
            default: break;
        }
    }
    // back(): step back. Returns false when already at the first step
    // (caller should exit to the menu).
    bool back() {
        switch (m_phase) {
            case Phase::SetBreak: m_phase = Phase::SetWork;  return true;
            case Phase::SetLoops: m_phase = Phase::SetBreak; return true;
            case Phase::Ready:    m_phase = Phase::SetLoops; return true;
            default: return false;
        }
    }

    // ── Run control ────────────────────────────────────────
    // start(): Ready -> first RunWork. False when work duration is 0.
    bool start(uint32_t now_ms) {
        if (m_phase != Phase::Ready || work_ms() == 0) return false;
        m_loop = 1;
        enter_work(now_ms);
        return true;
    }
    void pause(uint32_t now_ms) {
        if (!is_running()) return;
        m_remain_ms = (m_deadline_ms > now_ms) ? (m_deadline_ms - now_ms) : 0;
        m_paused_from = m_phase;
        m_phase = Phase::Paused;
    }
    void resume(uint32_t now_ms) {
        if (m_phase != Phase::Paused) return;
        m_phase = m_paused_from;
        m_deadline_ms = now_ms + m_remain_ms;
    }
    void cancel() {
        // Back to Ready, settings + loop count preserved.
        if (is_running() || m_phase == Phase::Paused || m_phase == Phase::Done) {
            m_phase = Phase::Ready;
            m_remain_ms = 0;
        }
    }

    // tick(): advance the countdown. Call frequently from the UI task.
    // Emits exactly one event per expired phase.
    TickEvent tick(uint32_t now_ms) {
        if (!is_running()) {
            return TickEvent::None;
        }
        if (now_ms < m_deadline_ms) {
            m_remain_ms = m_deadline_ms - now_ms;
            return TickEvent::None;
        }
        m_remain_ms = 0;
        if (m_phase == Phase::RunWork) {
            if (m_loop >= m_s.loops) {
                m_phase = Phase::Done;
                return TickEvent::AllFinished;
            }
            if (break_ms() == 0) {
                // No break configured — roll straight into the next work session.
                ++m_loop;
                enter_work(now_ms);
                return TickEvent::WorkFinished;
            }
            enter_break(now_ms);
            return TickEvent::WorkFinished;
        }
        // RunBreak expired -> next work session.
        ++m_loop;
        enter_work(now_ms);
        return TickEvent::BreakFinished;
    }

private:
    void enter_work(uint32_t now_ms) {
        m_phase = Phase::RunWork;
        m_remain_ms = work_ms();
        m_deadline_ms = now_ms + m_remain_ms;
    }
    void enter_break(uint32_t now_ms) {
        m_phase = Phase::RunBreak;
        m_remain_ms = break_ms();
        m_deadline_ms = now_ms + m_remain_ms;
    }

    PomodoroSettings m_s{};
    Phase    m_phase{Phase::SetWork};
    Phase    m_paused_from{Phase::RunWork};
    uint8_t  m_loop{1};
    uint32_t m_remain_ms{0};
    uint32_t m_deadline_ms{0};
};

} // namespace Fuchey

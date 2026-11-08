// adx-thread: main
//
// LED feedback (phase_11.md §4.7): a pad controller's lights follow the clip states, on
// a thread of their own at 30 Hz - not the audio thread (sending MIDI is a blocking
// call) and not the UI's (the lights must not stutter while a dialog is open).
//
// Each tick reads every cell's look (empty, or its clip's state from the session's
// state ring), works out its light (a blinking one is off half the time), and sends a
// message for each light that changed: a note-on whose velocity is the colour, or the
// profile's sysex around the pad's note and an RGB triple.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <thread>
#include <vector>

#include "engine/midi/PadController.h"

namespace adx::midi {

class PadFeedback {
public:
    /// The look of the cell at (column, row): kPadLooks for one outside the set.
    using LookOf = std::function<PadLook(std::uint32_t column, std::uint32_t row)>;
    /// Sends one complete MIDI message; false when it could not.
    using Send = std::function<bool(std::span<const std::uint8_t>)>;

    PadFeedback(PadProfile profile, LookOf look, Send send, double hz = 30.0);
    ~PadFeedback();
    PadFeedback(const PadFeedback&) = delete;
    PadFeedback& operator=(const PadFeedback&) = delete;

    /// Ticks run, and messages sent, since the start. Any thread.
    [[nodiscard]] std::uint64_t ticks() const noexcept {
        return m_ticks.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t sent() const noexcept {
        return m_sent.load(std::memory_order_acquire);
    }
    /// What the cell's light last said, by look: what a test compares with the session.
    [[nodiscard]] PadLook shown(std::uint32_t column, std::uint32_t row) const noexcept;
    /// The thread the lights are sent from.
    [[nodiscard]] std::thread::id thread() const noexcept {
        return m_thread.get_id();
    }

    /// The message that lights the pad `note` as `color` (on, or off when `lit` is
    /// false), for this profile. Empty when the profile has no feedback.
    [[nodiscard]] static std::vector<std::uint8_t>
    message(const PadProfile& profile, std::uint8_t note, const PadColor& color, bool lit);

private:
    void run();
    void tick();

    PadProfile m_profile;
    LookOf m_look;
    Send m_send;
    double m_hz;
    /// Per cell: the look shown, and the light last sent (-1 none yet).
    std::vector<std::atomic<std::uint8_t>> m_shown;
    std::vector<std::int64_t> m_lastSent;
    std::atomic<std::uint64_t> m_ticks{0};
    std::atomic<std::uint64_t> m_sent{0};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace adx::midi

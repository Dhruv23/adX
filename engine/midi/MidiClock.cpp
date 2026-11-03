// adx-thread: realtime
#include "engine/midi/MidiClock.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adx::midi {

void ClockFollower::pulse(std::uint64_t at) noexcept {
    ++m_pulses;
    const auto t = static_cast<double>(at);
    if (m_pulses == 1) {
        m_firstAt = at;
    } else if (m_pulses == 2) {
        // Seed the loop from the first interval, then let it filter from here on.
        m_period = t - static_cast<double>(m_firstAt);
        const double omega =
            2.0 * std::numbers::pi * m_bandwidth * m_period / static_cast<double>(m_sampleRate);
        m_b = std::numbers::sqrt2 * omega;
        m_c = omega * omega;
        m_last = t;
        m_next = t + m_period;
    } else {
        const double error = t - m_next;
        m_last = m_next;
        m_next += (m_b * error) + m_period;
        m_period += m_c * error;
    }
    if (m_running) {
        if (m_startPending) {
            m_startPending = false; // the first pulse after start is the downbeat
        } else {
            m_ticks += kTicksPerPulse;
        }
    }
}

void ClockFollower::start() noexcept {
    m_ticks = 0;
    m_running = true;
    m_startPending = true;
}

void ClockFollower::stop() noexcept {
    m_running = false;
}

void ClockFollower::resume() noexcept {
    m_running = true;
}

void ClockFollower::songPosition(std::uint16_t sixteenths) noexcept {
    m_ticks = static_cast<std::int64_t>(sixteenths) * (core::kPpq / 4);
    m_startPending = true;
}

double ClockFollower::bpm() const noexcept {
    if (!locked() || m_period <= 0.0) {
        return 0.0;
    }
    return 60.0 * static_cast<double>(m_sampleRate) /
           (m_period * static_cast<double>(kPulsesPerQuarter));
}

std::size_t clockPulses(std::int64_t fromTick, std::int64_t toTick, std::uint32_t frames,
                        std::span<ClockMessage> out) noexcept {
    if (toTick <= fromTick || frames == 0) {
        return 0;
    }
    // The first pulse at or after fromTick.
    std::int64_t tick = ((fromTick + kTicksPerPulse - 1) / kTicksPerPulse) * kTicksPerPulse;
    if (fromTick < 0) {
        tick = (fromTick / kTicksPerPulse) * kTicksPerPulse;
    }
    const auto span = static_cast<double>(toTick - fromTick);
    std::size_t written = 0;
    for (; tick < toTick && written < out.size(); tick += kTicksPerPulse) {
        const double fraction = static_cast<double>(tick - fromTick) / span;
        auto offset = static_cast<std::uint32_t>(std::floor(fraction * frames));
        offset = offset < frames ? offset : frames - 1;
        out[written++] = ClockMessage{.offset = offset, .status = kClockPulse};
    }
    return written;
}

ClockMessage songPositionAt(std::int64_t tick, std::uint32_t offset) noexcept {
    const std::int64_t sixteenths = std::max<std::int64_t>(0, tick / (core::kPpq / 4));
    const auto beats = static_cast<std::uint16_t>(std::min<std::int64_t>(sixteenths, 0x3FFF));
    return ClockMessage{.offset = offset,
                        .status = kSongPosition,
                        .data1 = static_cast<std::uint8_t>(beats & 0x7FU),
                        .data2 = static_cast<std::uint8_t>(beats >> 7U)};
}

} // namespace adx::midi

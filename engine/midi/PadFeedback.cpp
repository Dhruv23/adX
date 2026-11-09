// adx-thread: main
#include "engine/midi/PadFeedback.h"

#include <algorithm>
#include <chrono>

namespace adx::midi {

PadFeedback::PadFeedback(PadProfile profile, LookOf look, Send send, double hz)
    : m_profile(std::move(profile)), m_look(std::move(look)), m_send(std::move(send)),
      m_hz(hz > 0.0 ? hz : 30.0),
      m_shown(static_cast<std::size_t>(m_profile.columns) * m_profile.rows),
      m_lastSent(static_cast<std::size_t>(m_profile.columns) * m_profile.rows, -1) {
    for (std::atomic<std::uint8_t>& shown : m_shown) {
        shown.store(static_cast<std::uint8_t>(kPadLooks), std::memory_order_relaxed);
    }
    m_thread = std::thread([this] { run(); });
}

PadFeedback::~PadFeedback() {
    m_stop.store(true, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

PadLook PadFeedback::shown(std::uint32_t column, std::uint32_t row) const noexcept {
    if (column >= m_profile.columns || row >= m_profile.rows) {
        return static_cast<PadLook>(kPadLooks);
    }
    return static_cast<PadLook>(
        m_shown[(static_cast<std::size_t>(row) * m_profile.columns) + column].load(
            std::memory_order_acquire));
}

std::vector<std::uint8_t> PadFeedback::message(const PadProfile& profile, std::uint8_t padNote,
                                               const PadColor& color, bool lit) {
    const auto note = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(padNote) + profile.feedbackOffset, 0, 127));
    switch (profile.feedback) {
    case FeedbackMode::Note:
        return {static_cast<std::uint8_t>(0x90U | ((profile.feedbackChannel - 1U) & 0x0FU)),
                static_cast<std::uint8_t>(note & 0x7FU),
                static_cast<std::uint8_t>(lit ? (color.velocity & 0x7FU) : 0U)};
    case FeedbackMode::SysexRgb: {
        std::vector<std::uint8_t> out = profile.sysexPrefix;
        const std::uint32_t rgb = lit ? color.rgb : 0U;
        out.push_back(static_cast<std::uint8_t>(note & 0x7FU));
        out.push_back(static_cast<std::uint8_t>((rgb >> 17U) & 0x7FU));
        out.push_back(static_cast<std::uint8_t>((rgb >> 9U) & 0x7FU));
        out.push_back(static_cast<std::uint8_t>((rgb >> 1U) & 0x7FU));
        out.insert(out.end(), profile.sysexSuffix.begin(), profile.sysexSuffix.end());
        return out;
    }
    case FeedbackMode::None:
        break;
    }
    return {};
}

void PadFeedback::tick() {
    const std::uint64_t tick = m_ticks.load(std::memory_order_relaxed);
    // Every two seconds every light again: a controller plugged in (or a listener that
    // opened its port) after the last change still comes to show the set.
    const auto refresh = static_cast<std::uint64_t>(m_hz * 2.0);
    if (refresh > 0 && tick > 0 && tick % refresh == 0) {
        std::ranges::fill(m_lastSent, -1);
    }
    // Blinking at about 2 Hz: on for the first half of every half-second.
    const auto period = static_cast<std::uint64_t>(m_hz / 2.0);
    const bool blinkOn = period == 0 || (tick % period) < (period / 2 + (period % 2));
    for (std::uint32_t r = 0; r < m_profile.rows; ++r) {
        for (std::uint32_t c = 0; c < m_profile.columns; ++c) {
            const PadLook look = m_look(c, r);
            const std::size_t cell = (static_cast<std::size_t>(r) * m_profile.columns) + c;
            m_shown[cell].store(static_cast<std::uint8_t>(look), std::memory_order_release);
            const int note = m_profile.noteAt(c, r);
            if (note < 0 || static_cast<std::size_t>(look) >= kPadLooks) {
                continue;
            }
            const PadColor& color = m_profile.colors.at(static_cast<std::size_t>(look));
            const bool lit = !color.blink || blinkOn;
            // One number for the light: what it is, and whether it is on now.
            const std::int64_t light = (static_cast<std::int64_t>(look) << 1) | (lit ? 1 : 0);
            if (m_lastSent[cell] == light) {
                continue;
            }
            const std::vector<std::uint8_t> bytes =
                message(m_profile, static_cast<std::uint8_t>(note), color, lit);
            if (!bytes.empty() && m_send(bytes)) {
                m_sent.fetch_add(1, std::memory_order_acq_rel);
            }
            m_lastSent[cell] = light;
        }
    }
    m_ticks.fetch_add(1, std::memory_order_acq_rel);
}

void PadFeedback::run() {
    using Clock = std::chrono::steady_clock;
    const auto step =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / m_hz));
    auto next = Clock::now();
    while (!m_stop.load(std::memory_order_acquire)) {
        tick();
        next += step;
        const auto now = Clock::now();
        if (next < now) {
            next = now; // fell behind (a stall): carry on from here, no burst of catch-up
        }
        std::this_thread::sleep_until(next);
    }
}

} // namespace adx::midi

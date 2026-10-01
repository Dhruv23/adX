#include "engine/transport/TimeSource.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace adx::transport {
namespace {

// The map a source uses before any snapshot has bound it: 120 bpm from tick 0, the
// same default a new TempoMap has. Constant, so borrowing it is always safe.
constexpr std::array<core::TempoEvent, 1> kDefaultTempoEvents{
    core::TempoEvent{.at = core::Ticks{0}, .bpm = 120.0, .ramp = false}};
constexpr std::array<double, 1> kDefaultCumSeconds{0.0};

constexpr std::int64_t kNoBoundary = std::numeric_limits<std::int64_t>::max();

[[nodiscard]] bool sameTempo(const core::TempoView& lhs, const core::TempoView& rhs) noexcept {
    if (lhs.events.size() != rhs.events.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.events.size(); ++i) {
        if (!(lhs.events[i] == rhs.events[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

TimeSource::TimeSource() noexcept
    : m_tempo{.events = kDefaultTempoEvents, .cumSeconds = kDefaultCumSeconds} {
    recomputeNextTempoChange();
}

void TimeSource::reset() noexcept {
    m_tempo = core::TempoView{.events = kDefaultTempoEvents, .cumSeconds = kDefaultCumSeconds};
    m_sampleRate = 48000;
    m_positionSamples = 0;
    m_fractionalSample = 0.0;
    m_state = PlayState::Stopped;
    m_loop = LoopRegion{};
    m_rate = 1.0;
    m_loopStartSamples = 0;
    m_loopEndSamples = 0;
    ++m_seekGeneration;
    ++m_loopGeneration;
    m_hasPendingSeek = false;
    m_hasPendingState = false;
    m_hasPendingLoop = false;
    m_hasPendingRate = false;
    m_wasRolling = false;
    recomputeNextTempoChange();
    publish();
}

void TimeSource::bindTempo(core::TempoView tempo, std::uint32_t sampleRate) noexcept {
    if (!tempo.valid() || sampleRate == 0) {
        return;
    }
    const bool sameMap = sameTempo(m_tempo, tempo) && sampleRate == m_sampleRate;
    if (sameMap) {
        // Same content, different storage: a new snapshot that did not touch tempo.
        // Just follow the pointer - moving the position here would make every
        // unrelated edit nudge the playhead by a rounding error.
        m_tempo = tempo;
        return;
    }

    // Keep the musical position, not the sample count: after a tempo edit, "bar 17"
    // is what the user is listening to, and it must stay bar 17.
    const core::Ticks here = positionTicks();
    m_tempo = tempo;
    m_sampleRate = sampleRate;
    m_positionSamples = samplesAt(here);
    m_fractionalSample = 0.0;
    ++m_seekGeneration;
    recomputeLoopSamples();
    recomputeNextTempoChange();
}

void TimeSource::requestSeek(core::Ticks at) noexcept {
    m_hasPendingSeek = true;
    m_pendingSeek = at;
}

void TimeSource::requestState(PlayState state) noexcept {
    m_hasPendingState = true;
    m_pendingState = state;
}

void TimeSource::requestLoop(LoopRegion loop) noexcept {
    m_hasPendingLoop = true;
    m_pendingLoop = loop;
}

void TimeSource::requestRate(double rate) noexcept {
    // A rate of zero or below is a stop by another name, and a stop has its own
    // request. Clamped rather than rejected: this arrives from a UI knob.
    m_hasPendingRate = true;
    m_pendingRate = rate > 0.0 ? rate : 1.0;
}

BlockTransition TimeSource::beginBlock() noexcept {
    BlockTransition transition;
    const bool rollingBefore = m_wasRolling;

    if (m_hasPendingRate) {
        m_hasPendingRate = false;
        m_rate = m_pendingRate;
    }
    if (m_hasPendingLoop) {
        m_hasPendingLoop = false;
        m_loop = m_pendingLoop;
        recomputeLoopSamples();
    }
    if (m_hasPendingSeek) {
        m_hasPendingSeek = false;
        transition.seeked = true;
        transition.wasRollingBeforeSeek = isRolling(m_state);
        m_positionSamples = samplesAt(m_pendingSeek);
        m_fractionalSample = 0.0;
        ++m_seekGeneration;
        recomputeNextTempoChange();
    }
    if (m_hasPendingState) {
        m_hasPendingState = false;
        m_state = m_pendingState;
    }

    const bool rollingNow = isRolling(m_state);
    transition.stopped = rollingBefore && !rollingNow;
    transition.started = !rollingBefore && rollingNow;
    m_wasRolling = rollingNow;
    return transition;
}

std::uint32_t TimeSource::framesToNextBoundary(std::uint32_t frames) const noexcept {
    if (!rolling() || frames == 0) {
        return frames;
    }

    std::int64_t boundary = kNoBoundary;
    if (m_loop.active() && m_positionSamples < m_loopEndSamples) {
        boundary = m_loopEndSamples;
    }
    if (m_nextTempoChangeSamples > m_positionSamples && m_nextTempoChangeSamples < boundary) {
        boundary = m_nextTempoChangeSamples;
    }
    if (boundary == kNoBoundary) {
        return frames;
    }

    const std::int64_t distance = boundary - m_positionSamples;
    std::int64_t needed = distance;
    if (m_rate != 1.0) {
        // Output frames until the source's own position reaches the boundary.
        needed = static_cast<std::int64_t>(
            std::ceil((static_cast<double>(distance) - m_fractionalSample) / m_rate));
    }
    needed = std::max<std::int64_t>(needed, 1);
    return std::cmp_less(needed, frames) ? static_cast<std::uint32_t>(needed) : frames;
}

bool TimeSource::advance(std::uint32_t frames) noexcept {
    if (!rolling() || frames == 0) {
        return false;
    }

    const std::int64_t before = m_positionSamples;
    if (m_rate == 1.0) {
        m_positionSamples += frames;
    } else {
        const double total = (static_cast<double>(frames) * m_rate) + m_fractionalSample;
        const double whole = std::floor(total);
        m_fractionalSample = total - whole;
        m_positionSamples += static_cast<std::int64_t>(whole);
    }

    bool wrapped = false;
    if (m_loop.active() && before < m_loopEndSamples && m_positionSamples >= m_loopEndSamples) {
        // Not a seek: the generation is untouched, so voices sustain and the cursors
        // jump to their cached loop-start position without searching (§4.11).
        m_positionSamples = m_loopStartSamples + (m_positionSamples - m_loopEndSamples);
        wrapped = true;
        recomputeNextTempoChange();
    } else if (m_positionSamples >= m_nextTempoChangeSamples) {
        recomputeNextTempoChange();
    }
    return wrapped;
}

core::Ticks TimeSource::positionTicks() const noexcept {
    return m_tempo.toTicks(core::Samples{m_positionSamples}, m_sampleRate);
}

void TimeSource::publish() noexcept {
    m_publishedTicks.store(positionTicks().value, std::memory_order_release);
    m_publishedSamples.store(m_positionSamples, std::memory_order_release);
    m_publishedState.store(static_cast<std::uint8_t>(m_state), std::memory_order_release);
}

void TimeSource::recomputeLoopSamples() noexcept {
    if (m_loop.active()) {
        m_loopStartSamples = samplesAt(m_loop.start);
        m_loopEndSamples = samplesAt(m_loop.end);
    } else {
        m_loopStartSamples = 0;
        m_loopEndSamples = 0;
    }
    ++m_loopGeneration;
}

void TimeSource::recomputeNextTempoChange() noexcept {
    const core::Ticks here = positionTicks();
    const core::Ticks sentinel{std::numeric_limits<std::int64_t>::max()};
    const core::Ticks next = m_tempo.nextChangeAfter(here, sentinel);
    m_nextTempoChangeSamples = next == sentinel ? kNoBoundary : samplesAt(next);
}

} // namespace adx::transport

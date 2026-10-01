#include "engine/core/TempoMath.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "engine/core/Config.h"

namespace adx::core {
namespace {

/// Seconds per tick at a constant tempo.
[[nodiscard]] double secondsPerTick(double bpm) noexcept {
    return 60.0 / (bpm * static_cast<double>(kPpq));
}

/// Below this, a ramp's endpoints are close enough that the closed form's
/// log(b1/b0)/k is all rounding error and the constant-tempo formula is both
/// cheaper and more accurate.
constexpr double kRampEpsilon = 1e-9;

} // namespace

void computeCumulativeSeconds(std::span<const TempoEvent> events,
                              std::span<double> cumSeconds) noexcept {
    ADX_ASSERT(events.size() == cumSeconds.size());
    if (cumSeconds.empty()) {
        return;
    }
    cumSeconds[0] = 0.0;
    for (std::size_t i = 1; i < events.size() && i < cumSeconds.size(); ++i) {
        const TempoEvent& previous = events[i - 1];
        const double span = static_cast<double>((events[i].at - previous.at).value);
        double elapsed = 0.0;
        if (previous.ramp) {
            // Closed form for a tempo that varies linearly in the tick domain:
            //   dt/dx = 60 / (PPQ * bpm(x)),  bpm(x) = b0 + k x
            //   t(L)  = 60 / (PPQ * k) * ln(b1 / b0)
            const double slope = (events[i].bpm - previous.bpm) / span;
            if (std::abs(slope) > kRampEpsilon && span > 0.0) {
                elapsed = (60.0 / (static_cast<double>(kPpq) * slope)) *
                          std::log(events[i].bpm / previous.bpm);
            } else {
                elapsed = span * secondsPerTick(previous.bpm);
            }
        } else {
            elapsed = span * secondsPerTick(previous.bpm);
        }
        cumSeconds[i] = cumSeconds[i - 1] + elapsed;
    }
}

std::size_t TempoView::segmentAt(Ticks at) const noexcept {
    const auto position = std::ranges::upper_bound(events, at, {}, &TempoEvent::at);
    if (position == events.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(events.begin(), position)) - 1;
}

double TempoView::bpmAt(Ticks at) const noexcept {
    const std::size_t index = segmentAt(at);
    const TempoEvent& event = events[index];
    if (!event.ramp || index + 1 >= events.size()) {
        return event.bpm;
    }
    const double span = static_cast<double>((events[index + 1].at - event.at).value);
    if (span <= 0.0) {
        return event.bpm;
    }
    const double offset = std::clamp(static_cast<double>((at - event.at).value), 0.0, span);
    return event.bpm + ((events[index + 1].bpm - event.bpm) * (offset / span));
}

double TempoView::secondsAt(Ticks at) const noexcept {
    const std::size_t index = segmentAt(at);
    const TempoEvent& event = events[index];
    const double offset = static_cast<double>((at - event.at).value);

    const bool ramping = event.ramp && index + 1 < events.size();
    if (!ramping) {
        return cumSeconds[index] + (offset * secondsPerTick(event.bpm));
    }

    const double span = static_cast<double>((events[index + 1].at - event.at).value);
    const double slope = span > 0.0 ? (events[index + 1].bpm - event.bpm) / span : 0.0;
    if (std::abs(slope) <= kRampEpsilon) {
        return cumSeconds[index] + (offset * secondsPerTick(event.bpm));
    }
    const double bpmHere = std::max(event.bpm + (slope * offset), kMinBpm);
    return cumSeconds[index] +
           ((60.0 / (static_cast<double>(kPpq) * slope)) * std::log(bpmHere / event.bpm));
}

Ticks TempoView::ticksAtSeconds(double seconds) const noexcept {
    // The segment whose start is at or before `seconds`. Binary search: this now runs
    // on the audio thread as well as the main one, and although a project has a
    // handful of tempo changes, "a handful" is not a bound the callback can rely on.
    const auto position = std::ranges::upper_bound(cumSeconds, seconds);
    const std::size_t index =
        position == cumSeconds.begin()
            ? 0
            : static_cast<std::size_t>(std::distance(cumSeconds.begin(), position)) - 1;

    const TempoEvent& event = events[index];
    const double elapsed = seconds - cumSeconds[index];

    double offset = 0.0;
    const bool ramping = event.ramp && index + 1 < events.size();
    if (ramping) {
        const double span = static_cast<double>((events[index + 1].at - event.at).value);
        const double slope = span > 0.0 ? (events[index + 1].bpm - event.bpm) / span : 0.0;
        if (std::abs(slope) > kRampEpsilon) {
            // Inverse of the closed form above.
            const double growth = std::exp(elapsed * static_cast<double>(kPpq) * slope / 60.0);
            offset = (event.bpm * (growth - 1.0)) / slope;
        } else {
            offset = elapsed / secondsPerTick(event.bpm);
        }
    } else {
        offset = elapsed / secondsPerTick(event.bpm);
    }

    return event.at + Ticks{static_cast<std::int64_t>(std::llround(offset))};
}

Samples TempoView::toSamples(Ticks at, std::uint32_t sampleRate) const noexcept {
    ADX_ASSERT(sampleRate > 0);
    if (sampleRate == 0) {
        return Samples{0};
    }
    return Samples{std::llround(secondsAt(at) * static_cast<double>(sampleRate))};
}

Ticks TempoView::toTicks(Samples at, std::uint32_t sampleRate) const noexcept {
    ADX_ASSERT(sampleRate > 0);
    if (sampleRate == 0) {
        return Ticks{0};
    }
    return ticksAtSeconds(static_cast<double>(at.value) / static_cast<double>(sampleRate));
}

Ticks TempoView::nextChangeAfter(Ticks at, Ticks fallback) const noexcept {
    const auto position = std::ranges::upper_bound(events, at, {}, &TempoEvent::at);
    return position == events.end() ? fallback : position->at;
}

} // namespace adx::core

#include "engine/core/TempoMap.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Config.h"

namespace adx::core {
namespace {

[[nodiscard]] std::int64_t ticksPerBar(const MeterEvent& meter) noexcept {
    return static_cast<std::int64_t>(meter.numerator) * ticksPerBeat(meter.denominator);
}

} // namespace

TempoMap::TempoMap() {
    m_tempo.push_back(TempoEvent{.at = Ticks{0}, .bpm = 120.0, .ramp = false});
    m_meter.push_back(MeterEvent{.at = Ticks{0}, .numerator = 4, .denominator = 4});
    rebuild();
}

void TempoMap::setTempo(Ticks at, double bpm, bool ramp) {
    const double clamped = std::clamp(bpm, kMinBpm, kMaxBpm);
    const auto position = std::ranges::lower_bound(m_tempo, at, {}, &TempoEvent::at);
    if (position != m_tempo.end() && position->at == at) {
        position->bpm = clamped;
        position->ramp = ramp;
    } else {
        m_tempo.insert(position, TempoEvent{.at = at, .bpm = clamped, .ramp = ramp});
    }
    rebuild();
}

bool TempoMap::removeTempo(Ticks at) {
    if (at == Ticks{0}) {
        return false;
    }
    const auto position = std::ranges::lower_bound(m_tempo, at, {}, &TempoEvent::at);
    if (position == m_tempo.end() || position->at != at) {
        return false;
    }
    m_tempo.erase(position);
    rebuild();
    return true;
}

void TempoMap::setMeter(Ticks at, std::uint16_t numerator, std::uint16_t denominator) {
    MeterEvent event{.at = at, .numerator = numerator, .denominator = denominator};
    if (event.numerator == 0) {
        event.numerator = 4;
    }
    // Only denominators that divide a whole note exactly are representable in ticks,
    // and every meter anyone writes is one of them.
    if (event.denominator != 1 && event.denominator != 2 && event.denominator != 4 &&
        event.denominator != 8 && event.denominator != 16 && event.denominator != 32 &&
        event.denominator != 64) {
        event.denominator = 4;
    }
    const auto position = std::ranges::lower_bound(m_meter, at, {}, &MeterEvent::at);
    if (position != m_meter.end() && position->at == at) {
        *position = event;
    } else {
        m_meter.insert(position, event);
    }
    rebuild();
}

bool TempoMap::removeMeter(Ticks at) {
    if (at == Ticks{0}) {
        return false;
    }
    const auto position = std::ranges::lower_bound(m_meter, at, {}, &MeterEvent::at);
    if (position == m_meter.end() || position->at != at) {
        return false;
    }
    m_meter.erase(position);
    rebuild();
    return true;
}

void TempoMap::rebuild() {
    m_cumSeconds.assign(m_tempo.size(), 0.0);
    computeCumulativeSeconds(m_tempo, m_cumSeconds);

    m_cumBars.assign(m_meter.size(), 0);
    for (std::size_t i = 1; i < m_meter.size(); ++i) {
        const std::int64_t span = (m_meter[i].at - m_meter[i - 1].at).value;
        const std::int64_t perBar = ticksPerBar(m_meter[i - 1]);
        // Ceiling division: a meter change part-way through a bar still ends that
        // bar, so no two ticks ever share a bar number across the boundary.
        const std::int64_t bars = perBar > 0 ? (span + perBar - 1) / perBar : 0;
        m_cumBars[i] = m_cumBars[i - 1] + std::max<std::int64_t>(bars, 0);
    }
}

std::size_t TempoMap::meterIndexAt(Ticks at) const noexcept {
    const auto position = std::ranges::upper_bound(m_meter, at, {}, &MeterEvent::at);
    if (position == m_meter.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(m_meter.begin(), position)) - 1;
}

double TempoMap::bpmAt(Ticks at) const noexcept {
    return view().bpmAt(at);
}

MeterEvent TempoMap::meterAt(Ticks at) const noexcept {
    return m_meter[meterIndexAt(at)];
}

double TempoMap::secondsAt(Ticks at) const noexcept {
    return view().secondsAt(at);
}

Ticks TempoMap::ticksAtSeconds(double seconds) const noexcept {
    return view().ticksAtSeconds(seconds);
}

Samples TempoMap::toSamples(Ticks at, std::uint32_t sampleRate) const noexcept {
    return view().toSamples(at, sampleRate);
}

Ticks TempoMap::toTicks(Samples at, std::uint32_t sampleRate) const noexcept {
    return view().toTicks(at, sampleRate);
}

BarBeatTick TempoMap::toBarBeat(Ticks at) const noexcept {
    const std::size_t index = meterIndexAt(at);
    const MeterEvent& meter = m_meter[index];
    const std::int64_t perBar = ticksPerBar(meter);
    const std::int64_t perBeat = ticksPerBeat(meter.denominator);
    std::int64_t offset = (at - meter.at).value;

    // Negative positions belong to the first segment and count backwards through it,
    // so a pickup bar is -1:x:y rather than a wrapped positive number. Flooring
    // division, not truncating: C++ rounds toward zero and that puts ticks -1 and +1
    // in the same bar.
    std::int64_t bars = 0;
    if (perBar > 0) {
        bars = offset >= 0 ? offset / perBar : -(((-offset) + perBar - 1) / perBar);
        offset -= bars * perBar;
    }

    return BarBeatTick{.bar = m_cumBars[index] + bars,
                       .beat = perBeat > 0 ? offset / perBeat : 0,
                       .tick = perBeat > 0 ? offset % perBeat : offset};
}

Ticks TempoMap::fromBarBeat(BarBeatTick position) const noexcept {
    std::size_t index = 0;
    for (std::size_t i = 1; i < m_cumBars.size(); ++i) {
        if (m_cumBars[i] > position.bar) {
            break;
        }
        index = i;
    }

    const MeterEvent& meter = m_meter[index];
    const std::int64_t offset = ((position.bar - m_cumBars[index]) * ticksPerBar(meter)) +
                                (position.beat * ticksPerBeat(meter.denominator)) + position.tick;
    return meter.at + Ticks{offset};
}

} // namespace adx::core

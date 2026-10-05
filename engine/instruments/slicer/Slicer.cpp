// adx-thread: main
#include "engine/instruments/slicer/SliceLayout.h"

#include <algorithm>
#include <cmath>

namespace adx::instruments {

std::vector<project::SampleZone> evenSlices(core::SampleId sample, std::uint32_t frames,
                                            std::uint32_t count, std::uint8_t baseKey) {
    std::vector<std::uint32_t> points;
    count = std::max<std::uint32_t>(count, 1);
    points.reserve(count);
    for (std::uint32_t s = 0; s < count; ++s) {
        points.push_back(static_cast<std::uint32_t>((std::uint64_t{frames} * s) / count));
    }
    return slicesAt(sample, points, frames, baseKey);
}

std::vector<project::SampleZone> slicesAt(core::SampleId sample,
                                          std::span<const std::uint32_t> points,
                                          std::uint32_t frames, std::uint8_t baseKey) {
    std::vector<project::SampleZone> zones;
    zones.reserve(points.size());
    for (std::size_t s = 0; s < points.size() && baseKey + s <= 127; ++s) {
        const std::uint32_t start = points[s];
        const std::uint32_t end = s + 1 < points.size() ? points[s + 1] : frames;
        if (end <= start) {
            continue;
        }
        project::SampleZone zone;
        zone.sample = sample;
        zone.keyLow = static_cast<std::uint8_t>(baseKey + s);
        zone.keyHigh = zone.keyLow;
        zone.rootKey = zone.keyLow;
        zone.start = start;
        zone.end = end;
        zones.push_back(zone);
    }
    return zones;
}

std::vector<SliceNote> slicePattern(std::span<const project::SampleZone> slices,
                                    std::uint32_t sampleRate, double sourceBpm) {
    std::vector<SliceNote> notes;
    notes.reserve(slices.size());
    // Frames to ticks of the source loop: frames / rate seconds, x bpm / 60 beats.
    const double ticksPerFrame =
        sourceBpm / 60.0 * static_cast<double>(core::kPpq) / static_cast<double>(sampleRate);
    for (const project::SampleZone& slice : slices) {
        const auto start = std::llround(static_cast<double>(slice.start) * ticksPerFrame);
        const auto end = std::llround(static_cast<double>(slice.end) * ticksPerFrame);
        notes.push_back(SliceNote{.start = core::Ticks{start},
                                  .length = core::Ticks{std::max<long long>(end - start, 1)},
                                  .pitch = slice.rootKey});
    }
    return notes;
}

} // namespace adx::instruments

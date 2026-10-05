// adx-thread: main
#include "engine/instruments/pool/PoolZones.h"

namespace adx::instruments {

std::vector<project::SampleZone> poolZones(std::span<const core::SampleId> samples,
                                           std::uint8_t baseKey) {
    std::vector<project::SampleZone> zones;
    zones.reserve(samples.size());
    for (std::size_t s = 0; s < samples.size() && baseKey + s <= 127; ++s) {
        project::SampleZone zone;
        zone.sample = samples[s];
        zone.keyLow = static_cast<std::uint8_t>(baseKey + s);
        zone.keyHigh = zone.keyLow;
        zone.rootKey = zone.keyLow;
        zones.push_back(zone);
    }
    return zones;
}

} // namespace adx::instruments

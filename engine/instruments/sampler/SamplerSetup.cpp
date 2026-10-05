// adx-thread: main
#include "engine/instruments/sampler/SamplerSetup.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "engine/dsp/Math.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/instruments/sampler/ZoneSet.h"
#include "engine/project/Resources.h"

namespace adx::instruments {

/// What keeps a Sampler node's samples alive, and what it was built from - which is
/// what samplerMatches compares, since the node's own copy is realtime data.
struct SamplerPins {
    std::vector<std::shared_ptr<const format::SampleEntry>> entries;
    std::vector<project::SampleZone> zones;
    std::vector<std::filesystem::path> files;
};

void destroySamplerPins(SamplerPins* pins) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - the node's opaque owner.
    delete pins;
}

ZoneSet::~ZoneSet() {
    destroySamplerPins(m_pins);
}

void ZoneSet::setZones(rt::OwnedArray<SamplerZone> zones, SamplerPins* pins) noexcept {
    m_zones = std::move(zones);
    destroySamplerPins(m_pins);
    m_pins = pins;
}

namespace {

/// The file each zone plays, resolved; empty for a zone whose sample is not in the
/// pool (Validate reports it).
std::vector<std::filesystem::path> resolveFiles(const project::Channel& channel,
                                                const project::Resources* resources,
                                                format::SamplePool& pool) {
    std::vector<std::filesystem::path> files;
    files.reserve(channel.instrument.zones.size());
    for (const project::SampleZone& zone : channel.instrument.zones) {
        const project::SampleRef* ref =
            resources != nullptr ? resources->find(zone.sample) : nullptr;
        files.push_back(ref != nullptr ? pool.resolve(*resources, ref->path)
                                       : std::filesystem::path{});
    }
    return files;
}

} // namespace

void configureSampler(ZoneSet& node, const project::Channel& channel,
                      const project::Resources* resources, format::SamplePool* pool) {
    format::SamplePool& samples = pool != nullptr ? *pool : format::SamplePool::global();
    auto pins = std::make_unique<SamplerPins>();
    pins->zones = channel.instrument.zones;
    pins->files = resolveFiles(channel, resources, samples);

    rt::OwnedArray<SamplerZone> zones;
    zones.allocate(channel.instrument.zones.size());
    const std::span<SamplerZone> out = zones.view();
    for (std::size_t z = 0; z < channel.instrument.zones.size(); ++z) {
        const project::SampleZone& zone = channel.instrument.zones[z];
        const format::SampleHandle* handle = nullptr;
        if (!pins->files[z].empty()) {
            std::shared_ptr<const format::SampleEntry> entry = samples.request(pins->files[z]);
            handle = &entry->handle();
            pins->entries.push_back(std::move(entry));
        }
        // Linear balance, as the mixer pans (graph/nodes/Gain.h): centre is unity.
        const float pan = std::clamp(zone.pan, -1.0F, 1.0F);
        const float linear = dsp::dbToGainF(zone.gainDb);
        out[z] = SamplerZone{.sample = handle,
                             .zone = zone,
                             .gainLeft = linear * (pan > 0.0F ? 1.0F - pan : 1.0F),
                             .gainRight = linear * (pan < 0.0F ? 1.0F + pan : 1.0F)};
    }
    node.setZones(std::move(zones), pins.release());
}

bool samplerMatches(const ZoneSet& node, const project::Channel& channel,
                    const project::Resources* resources, format::SamplePool* pool) {
    const SamplerPins* pins = node.pins();
    if (pins == nullptr) {
        return channel.instrument.zones.empty();
    }
    if (pins->zones != channel.instrument.zones) {
        return false;
    }
    format::SamplePool& samples = pool != nullptr ? *pool : format::SamplePool::global();
    return pins->files == resolveFiles(channel, resources, samples);
}

} // namespace adx::instruments

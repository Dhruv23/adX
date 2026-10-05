// Zones resolved to pool samples, and the pins that keep those samples alive.
//
// Every instrument that plays pool samples - the Sampler, and the Slicer, sample-pool
// channel and Granular built on the same zones - holds its zones the same way: baked
// into the node when it is built (SamplerSetup.cpp, main thread) and never changed
// after prepare, so the audio thread reads an immutable table and a handle per zone. A
// zone edit makes a new node (instruments::configMatches). The pins are an object the
// node cannot see the type of; a node is destroyed by the Reaper, on the main thread,
// which is where they are released.
#pragma once

#include <span>

#include "engine/format/audio/SampleView.h"
#include "engine/project/SampleZone.h"
#include "engine/rt/OwnedArray.h"

namespace adx::instruments {

/// One zone as the audio thread reads it: the project's zone, plus its sample.
struct SamplerZone {
    const format::SampleHandle* sample;
    project::SampleZone zone;
    /// dB to linear, and the pan split into left and right gains, precomputed.
    float gainLeft;
    float gainRight;
};

/// The pins: whatever keeps the zones' samples alive. Defined, created and destroyed
/// in SamplerSetup.cpp, on the main thread.
struct SamplerPins;
void destroySamplerPins(SamplerPins* pins) noexcept;

class ZoneSet {
public:
    ZoneSet() = default;
    virtual ~ZoneSet();
    ZoneSet(const ZoneSet&) = delete;
    ZoneSet& operator=(const ZoneSet&) = delete;
    ZoneSet(ZoneSet&&) = delete;
    ZoneSet& operator=(ZoneSet&&) = delete;

    /// Main thread, before prepare(). Takes ownership of `pins`.
    void setZones(rt::OwnedArray<SamplerZone> zones, SamplerPins* pins) noexcept;
    [[nodiscard]] std::span<const SamplerZone> zones() const noexcept {
        return m_zones.view();
    }
    [[nodiscard]] const SamplerPins* pins() const noexcept {
        return m_pins;
    }

private:
    rt::OwnedArray<SamplerZone> m_zones;
    SamplerPins* m_pins{nullptr};
};

} // namespace adx::instruments

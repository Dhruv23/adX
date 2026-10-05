// The sample-pool playback channel: plain one-shots, one sample per key (FINAL_PLAN
// §5.3).
//
// What a drum rack or a sound-effect lane is: drop samples on a channel, each lands on
// its own key, and a note plays its sample once, at its recorded pitch, to the end.
// No envelope to set, no loop, no key spanning - the Sampler with those taken away
// (kPoolParams), so it shares the Sampler's playback, its pool pins and its
// allocation-free voice path. poolZones() (PoolZones.h) is the main-thread half: one zone per
// sample, on consecutive keys.
#pragma once

#include <string_view>

#include "engine/instruments/sampler/SamplerInstrument.h"

namespace adx::instruments {

class SamplePoolChannel final : public SamplerInstrument {
public:
    using SamplerInstrument::SamplerInstrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "pool";
    }
};

} // namespace adx::instruments

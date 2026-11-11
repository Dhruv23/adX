// adx-thread: main
#include "engine/format/soundfont/Generators.h"

#include "engine/dsp/Math.h"

namespace adx::format::soundfont {

double timecentsToSeconds(double timecents) noexcept {
    if (timecents <= -12000.0) {
        return 0.0;
    }
    return dsp::exp2(timecents / 1200.0);
}

double absoluteCentsToHz(double cents) noexcept {
    return 8.175798915643707 * dsp::exp2(cents / 1200.0);
}

std::int16_t generatorDefault(std::uint16_t gen) noexcept {
    switch (static_cast<Gen>(gen)) {
    case Gen::InitialFilterFc:
        return 13500;
    case Gen::DelayModLfo:
    case Gen::DelayVibLfo:
    case Gen::DelayModEnv:
    case Gen::AttackModEnv:
    case Gen::HoldModEnv:
    case Gen::DecayModEnv:
    case Gen::ReleaseModEnv:
    case Gen::DelayVolEnv:
    case Gen::AttackVolEnv:
    case Gen::HoldVolEnv:
    case Gen::DecayVolEnv:
    case Gen::ReleaseVolEnv:
        return -12000;
    case Gen::KeyRange:
    case Gen::VelRange:
        return static_cast<std::int16_t>(127 << 8); // lo 0, hi 127
    case Gen::Keynum:
    case Gen::Velocity:
    case Gen::OverridingRootKey:
        return -1;
    case Gen::ScaleTuning:
        return 100;
    default:
        return 0;
    }
}

bool presetAdditive(std::uint16_t gen) noexcept {
    switch (static_cast<Gen>(gen)) {
    // Sample addresses, the sample, its modes, root key and exclusive class are the
    // instrument's alone (SoundFont 2.04 §8.5).
    case Gen::StartAddrsOffset:
    case Gen::EndAddrsOffset:
    case Gen::StartloopAddrsOffset:
    case Gen::EndloopAddrsOffset:
    case Gen::StartAddrsCoarseOffset:
    case Gen::EndAddrsCoarseOffset:
    case Gen::StartloopAddrsCoarseOffset:
    case Gen::EndloopAddrsCoarseOffset:
    case Gen::Keynum:
    case Gen::Velocity:
    case Gen::SampleId:
    case Gen::SampleModes:
    case Gen::ExclusiveClass:
    case Gen::OverridingRootKey:
    case Gen::KeyRange:
    case Gen::VelRange:
    case Gen::Instrument:
        return false;
    default:
        return gen < kGenCount;
    }
}

} // namespace adx::format::soundfont

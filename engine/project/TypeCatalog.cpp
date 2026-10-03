#include "engine/project/TypeCatalog.h"

#include <algorithm>
#include <array>

#include "engine/effects/Delay.h"
#include "engine/effects/Dynamics.h"
#include "engine/effects/ParametricEq.h"
#include "engine/effects/Ported.h"
#include "engine/instruments/additive/AdditiveParams.h"
#include "engine/instruments/sampler/SamplerParams.h"
#include "engine/instruments/va/VaParams.h"

namespace adx::project {
namespace {

// NOLINTBEGIN(modernize-use-designated-initializers) - one row per type.
constexpr auto kInstruments = std::to_array<TypeInfo>({
    {"additive", TypeKind::Instrument, instruments::kAdditiveParams,
     "iteration one's voice: harmonics, sub, noise, unison saw, resonant filter, formants"},
    {"sampler", TypeKind::Instrument, instruments::kSamplerParams,
     "multi-sample playback: zones, velocity layers, round robin, loop modes"},
    {"va", TypeKind::Instrument, instruments::kVaParams,
     "subtractive: two oscillators, unison, ladder and SVF filters, two LFOs"},
    {"testtone",
     TypeKind::Instrument,
     {},
     "a sine with a linear envelope; the scheduler's sample-accuracy reference"},
});

constexpr auto kEffects = std::to_array<TypeInfo>({
    {"Bitcrush", TypeKind::Effect, effects::kBitcrushParams, "sample-rate and bit-depth reduction"},
    {"Chorus", TypeKind::Effect, effects::kChorusParams, "modulated delay, quadrature stereo"},
    {"Compressor", TypeKind::Effect, effects::kCompressorParams,
     "peak/RMS compressor, soft knee, lookahead, external key"},
    {"Delay", TypeKind::Effect, effects::kDelayParams,
     "ping-pong or straight delay, tempo sync, filtered feedback"},
    {"Distortion", TypeKind::Effect, effects::kDistortionParams, "normalised tanh drive"},
    {"Ducker", TypeKind::Effect, effects::kDuckerParams, "sidechain ducker: v1's master pump"},
    {"EQ", TypeKind::Effect, effects::kEqParams, "v1's three-band EQ: 250 Hz, 1.2 kHz, 4 kHz"},
    {"Gate", TypeKind::Effect, effects::kGateParams,
     "gate / downward expander with hysteresis, hold and lookahead"},
    {"Limiter", TypeKind::Effect, effects::kLimiterParams, "true-peak lookahead brickwall"},
    {"ParametricEq", TypeKind::Effect, effects::kParametricEqParams,
     "eight fully parametric bands, spectrum tap"},
    {"Reverb", TypeKind::Effect, effects::kReverbParams, "Freeverb: 8 combs, 4 allpasses"},
});
// NOLINTEND(modernize-use-designated-initializers)

template<std::size_t N>
const TypeInfo* findIn(const std::array<TypeInfo, N>& table, std::string_view name) noexcept {
    const auto match = std::ranges::find(table, name, &TypeInfo::name);
    return match == table.end() ? nullptr : &*match;
}

} // namespace

std::span<const TypeInfo> instrumentTypes() noexcept {
    return kInstruments;
}

std::span<const TypeInfo> effectTypes() noexcept {
    return kEffects;
}

const TypeInfo* findInstrumentType(std::string_view name) noexcept {
    return findIn(kInstruments, name);
}

const TypeInfo* findEffectType(std::string_view name) noexcept {
    return findIn(kEffects, name);
}

std::uint32_t paramIndexOf(const TypeInfo& type, std::string_view name) noexcept {
    for (std::size_t i = 0; i < type.params.size(); ++i) {
        if (type.params[i].curve == CurvePart::None && type.params[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kNoParam;
}

const ParamDescriptor* findParam(const TypeInfo& type, std::string_view name) noexcept {
    const std::uint32_t index = paramIndexOf(type, name);
    return index == kNoParam ? nullptr : &type.params[index];
}

} // namespace adx::project

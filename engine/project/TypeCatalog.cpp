#include "engine/project/TypeCatalog.h"

#include <algorithm>
#include <array>

#include "engine/effects/Convolution.h"
#include "engine/effects/Delay.h"
#include "engine/effects/Drive.h"
#include "engine/effects/Dynamics.h"
#include "engine/effects/GrossBeat.h"
#include "engine/effects/Modulation.h"
#include "engine/effects/Multiband.h"
#include "engine/effects/ParametricEq.h"
#include "engine/effects/Ported.h"
#include "engine/effects/Spectral.h"
#include "engine/effects/Vocal.h"
#include "engine/instruments/additive/AdditiveParams.h"
#include "engine/instruments/drumsynth/DrumSynthParams.h"
#include "engine/instruments/fm/FmParams.h"
#include "engine/instruments/granular/GranularParams.h"
#include "engine/instruments/sampler/SamplerParams.h"
#include "engine/instruments/va/VaParams.h"
#include "engine/instruments/voice/VoiceParams.h"
#include "engine/instruments/wavetable/WavetableParams.h"

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
    {"slicer", TypeKind::Instrument, instruments::kSlicerParams,
     "beat-sliced audio as pads: one slice per key, choking, tempo-following"},
    {"pool", TypeKind::Instrument, instruments::kPoolParams,
     "sample-pool playback: one one-shot per key at its recorded pitch"},
    {"drumsynth", TypeKind::Instrument, instruments::kDrumSynthParams,
     "kick, snare, hat, clap and tom models, or a General MIDI kit"},
    {"granular", TypeKind::Instrument, instruments::kGranularParams,
     "grains of a sample or a tone: density, size, spray, jitter, spread"},
    {"fm", TypeKind::Instrument, instruments::kFmParams,
     "six operators, the DX7's 32 algorithms, feedback, ratio or fixed frequency"},
    {"wavetable", TypeKind::Instrument, instruments::kWavetableParams,
     "mipmapped 2D tables, linear or spectral morph, .wav import, unison"},
    {"voice", TypeKind::Instrument, instruments::kVoiceParams,
     "sung vocals from an UTAU voicebank, rendered through WORLD and cached"},
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
    {"Convolution", TypeKind::Effect, effects::kConvolutionParams,
     "partitioned FFT convolution with an IR file or a synthetic room"},
    {"Delay", TypeKind::Effect, effects::kDelayParams,
     "ping-pong or straight delay, tempo sync, filtered feedback"},
    {"Distortion", TypeKind::Effect, effects::kDistortionParams, "normalised tanh drive"},
    {"Ducker", TypeKind::Effect, effects::kDuckerParams, "sidechain ducker: v1's master pump"},
    {"EQ", TypeKind::Effect, effects::kEqParams, "v1's three-band EQ: 250 Hz, 1.2 kHz, 4 kHz"},
    {"Flanger", TypeKind::Effect, effects::kFlangerParams, "swept short delay with feedback"},
    {"FormantFilter", TypeKind::Effect, effects::kFormantFilterParams,
     "vowel formants, morphing between two, with an LFO"},
    {"FrequencyShifter", TypeKind::Effect, effects::kFrequencyShifterParams,
     "single-sideband shift: every partial moves by the same hertz"},
    {"Gate", TypeKind::Effect, effects::kGateParams,
     "gate / downward expander with hysteresis, hold and lookahead"},
    {"GrossBeat", TypeKind::Effect, effects::kGrossBeatParams,
     "time and volume patterns over a beat grid: half speed, stutter, tape stop, gate"},
    {"Limiter", TypeKind::Effect, effects::kLimiterParams, "true-peak lookahead brickwall"},
    {"MultibandComp", TypeKind::Effect, effects::kMultibandParams,
     "3-6 Linkwitz-Riley bands, each compressed"},
    {"Overdrive", TypeKind::Effect, effects::kOverdriveParams,
     "4x oversampled pedal: tightness, soft/tube/hard clip, tone"},
    {"ParametricEq", TypeKind::Effect, effects::kParametricEqParams,
     "eight fully parametric bands, spectrum tap"},
    {"Phaser", TypeKind::Effect, effects::kPhaserParams, "2-12 swept allpasses with feedback"},
    {"PitchShifter", TypeKind::Effect, effects::kPitchShifterParams,
     "phase-vocoder pitch shift, +-24 semitones"},
    {"Reverb", TypeKind::Effect, effects::kReverbParams, "Freeverb: 8 combs, 4 allpasses"},
    {"RingMod", TypeKind::Effect, effects::kRingModParams,
     "ring or amplitude modulation by a sine or the sidechain"},
    {"Saturation", TypeKind::Effect, effects::kSaturationParams,
     "4x oversampled tube, tape or transformer colour"},
    {"SpectralFreeze", TypeKind::Effect, effects::kSpectralFreezeParams,
     "holds the spectrum and replays it with random phase"},
    {"StereoImager", TypeKind::Effect, effects::kStereoImagerParams,
     "M/S width, with its own width below a crossover"},
    {"TransientShaper", TypeKind::Effect, effects::kTransientParams,
     "attack and sustain from two envelope followers"},
    {"Tremolo", TypeKind::Effect, effects::kTremoloParams, "LFO amplitude, six shapes, stereo"},
    {"Vocoder", TypeKind::Effect, effects::kVocoderParams,
     "16-32 band channel vocoder: sidechain or built-in carrier, sibilance path"},
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

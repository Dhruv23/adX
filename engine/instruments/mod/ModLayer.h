// The per-voice modulation layer shared by va, wavetable, fm, additive and physical
// (phase_12.md §4.3): a pitch envelope, two extra envelopes, two LFOs that can follow
// the tempo and arrive late, and the eight-slot matrix that routes them.
//
// An instrument appends kModParams to its own descriptor table, keeps a ModVoice in
// its per-voice state, and calls three functions: modStart at the note, modBegin at
// the top of each renderVoice call, and modTick once per frame. modTick hands back the
// frame's modulation as a ModFrame; what a destination means is the instrument's own
// business, and one it has no use for is simply not read.
//
// Every default is inert. With no slot routed and no pitch envelope, modTick returns
// the neutral frame and an instrument renders exactly what it did before the layer
// existed, which is what keeps every earlier golden hash where it was.
//
// The VA's own `lfoN.target` and `lfoN.depth` are sugar for one slot each. They go
// through the same ModFrame::add as a matrix slot, so the two spellings of one routing
// render bit for bit the same (va_lfo_target_equals_matrix_slot).
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/dsp/Lfo.h"
#include "engine/dsp/ModMatrix.h"
#include "engine/graph/nodes/ChannelNode.h"
#include "engine/project/ParamDescriptor.h"
#include "engine/project/TypeCatalog.h"

namespace adx::instruments {

/// The block's parameters, as offsets from where an instrument's table puts it.
enum class ModParam : std::uint32_t {
    PitchEnvAmount,
    PitchEnvTime,
    PitchEnvCurve,
    ModEnv1Attack,
    ModEnv1Decay,
    ModEnv1Sustain,
    ModEnv1Release,
    ModEnv2Attack,
    ModEnv2Decay,
    ModEnv2Sustain,
    ModEnv2Release,
    Lfo1Sync,
    Lfo1Delay,
    Lfo1Fade,
    Lfo2Sync,
    Lfo2Delay,
    Lfo2Fade,
    ModWheel,
    Aftertouch,
    VoiceMode,
    GlideMode,
    /// Eight slots of source, destination, amount.
    Slot1Source,
    SlotsEnd = Slot1Source + (3 * 8),
    /// The LFOs' own rate and shape, for an instrument that had no `lfo1`/`lfo2`. The
    /// VA has, and its table stops at SlotsEnd.
    Lfo1Rate = SlotsEnd,
    Lfo1Shape,
    Lfo2Rate,
    Lfo2Shape,
    Count,
};

inline constexpr std::uint32_t kModParamCount = static_cast<std::uint32_t>(ModParam::Count);
inline constexpr std::uint32_t kModParamCountNoLfo = static_cast<std::uint32_t>(ModParam::SlotsEnd);

/// 0 poly, 1 mono (each note restarts the envelopes), 2 legato (an overlapping note
/// only moves the pitch).
enum class VoiceMode : std::uint8_t { Poly, Mono, Legato };

namespace detail {

constexpr project::ParamDescriptor mod(std::string_view name, float lo, float hi, float def,
                                       project::Unit unit,
                                       project::ScaleKind scale = project::ScaleKind::Linear) {
    return project::ParamDescriptor{.name = name,
                                    .minimum = lo,
                                    .maximum = hi,
                                    .defaultValue = def,
                                    .unit = unit,
                                    .scale = scale,
                                    .rate = project::RateClass::Block,
                                    .curve = project::CurvePart::None};
}
constexpr project::ParamDescriptor modChoice(std::string_view name, float hi) {
    return mod(name, 0, hi, 0, project::Unit::Count, project::ScaleKind::Stepped);
}
constexpr project::ParamDescriptor modAmount(std::string_view name) {
    return mod(name, -1, 1, 0, project::Unit::Normalized);
}

} // namespace detail

inline constexpr auto kModParams = std::to_array<project::ParamDescriptor>({
    // Semitones the note starts away from its pitch; 0 is off.
    detail::mod("pitchEnv.amount", -48, 48, 0, project::Unit::Semitones),
    // Seconds to come within 1 % of the note's pitch.
    detail::mod("pitchEnv.time", 0.001F, 2, 0.05F, project::Unit::Seconds,
                project::ScaleKind::Logarithmic),
    // 0 exponential, 1 linear.
    detail::modChoice("pitchEnv.curve", 1),
    detail::mod("modEnv1.attack", 0, 10, 0.005F, project::Unit::Seconds,
                project::ScaleKind::Logarithmic),
    detail::mod("modEnv1.decay", 0, 10, 0.3F, project::Unit::Seconds,
                project::ScaleKind::Logarithmic),
    detail::mod("modEnv1.sustain", 0, 1, 0, project::Unit::Normalized),
    detail::mod("modEnv1.release", 0, 10, 0.3F, project::Unit::Seconds,
                project::ScaleKind::Logarithmic),
    detail::mod("modEnv2.attack", 0, 10, 0.005F, project::Unit::Seconds,
                project::ScaleKind::Logarithmic),
    detail::mod("modEnv2.decay", 0, 10, 0.3F, project::Unit::Seconds,
                project::ScaleKind::Logarithmic),
    detail::mod("modEnv2.sustain", 0, 1, 0, project::Unit::Normalized),
    detail::mod("modEnv2.release", 0, 10, 0.3F, project::Unit::Seconds,
                project::ScaleKind::Logarithmic),
    // 0 free (Hz); 1..12 are 1/64, 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/8D, 1/4, 1/2, 1 bar,
    // 2 bars, 4 bars (dsp::kLfoSyncWholes).
    detail::modChoice("lfo1.sync", 12),
    detail::mod("lfo1.delay", 0, 5, 0, project::Unit::Seconds),
    detail::mod("lfo1.fade", 0, 5, 0, project::Unit::Seconds),
    detail::modChoice("lfo2.sync", 12),
    detail::mod("lfo2.delay", 0, 5, 0, project::Unit::Seconds),
    detail::mod("lfo2.fade", 0, 5, 0, project::Unit::Seconds),
    detail::mod("mod.wheel", 0, 1, 0, project::Unit::Normalized),
    detail::mod("mod.pressure", 0, 1, 0, project::Unit::Normalized),
    detail::modChoice("voice.mode", 2),
    // 0 glides only into an overlapping note; 1 always.
    detail::modChoice("glide.mode", 1),
    detail::modChoice("mod.1.source", dsp::kModSourceCount - 1),
    detail::modChoice("mod.1.dest", dsp::kModDestCount - 1),
    detail::modAmount("mod.1.amount"),
    detail::modChoice("mod.2.source", dsp::kModSourceCount - 1),
    detail::modChoice("mod.2.dest", dsp::kModDestCount - 1),
    detail::modAmount("mod.2.amount"),
    detail::modChoice("mod.3.source", dsp::kModSourceCount - 1),
    detail::modChoice("mod.3.dest", dsp::kModDestCount - 1),
    detail::modAmount("mod.3.amount"),
    detail::modChoice("mod.4.source", dsp::kModSourceCount - 1),
    detail::modChoice("mod.4.dest", dsp::kModDestCount - 1),
    detail::modAmount("mod.4.amount"),
    detail::modChoice("mod.5.source", dsp::kModSourceCount - 1),
    detail::modChoice("mod.5.dest", dsp::kModDestCount - 1),
    detail::modAmount("mod.5.amount"),
    detail::modChoice("mod.6.source", dsp::kModSourceCount - 1),
    detail::modChoice("mod.6.dest", dsp::kModDestCount - 1),
    detail::modAmount("mod.6.amount"),
    detail::modChoice("mod.7.source", dsp::kModSourceCount - 1),
    detail::modChoice("mod.7.dest", dsp::kModDestCount - 1),
    detail::modAmount("mod.7.amount"),
    detail::modChoice("mod.8.source", dsp::kModSourceCount - 1),
    detail::modChoice("mod.8.dest", dsp::kModDestCount - 1),
    detail::modAmount("mod.8.amount"),
    detail::mod("lfo1.rate", 0.01F, 40, 5, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    detail::modChoice("lfo1.shape", dsp::kLfoShapeCount - 1),
    detail::mod("lfo2.rate", 0.01F, 40, 0.5F, project::Unit::Hertz,
                project::ScaleKind::Logarithmic),
    detail::modChoice("lfo2.shape", dsp::kLfoShapeCount - 1),
});
static_assert(kModParams.size() == kModParamCount);

/// `own` followed by the first `ModCount` rows of kModParams: an instrument's table.
template<std::size_t Own, std::size_t ModCount = kModParamCount>
constexpr auto withModParams(const std::array<project::ParamDescriptor, Own>& own) {
    std::array<project::ParamDescriptor, Own + ModCount> table{};
    for (std::size_t i = 0; i < Own; ++i) {
        table[i] = own[i];
    }
    for (std::size_t i = 0; i < ModCount; ++i) {
        table[Own + i] = kModParams[i];
    }
    return table;
}

/// Where an instrument's table has the block, and where the LFOs' own knobs are.
struct ModLayout {
    /// The index of `pitchEnv.amount`.
    std::uint32_t base{0};
    std::array<std::uint32_t, 2> lfoRate{project::kNoParam, project::kNoParam};
    std::array<std::uint32_t, 2> lfoShape{project::kNoParam, project::kNoParam};
    /// An LFO without a retrig knob restarts at every note.
    std::array<std::uint32_t, 2> lfoRetrig{project::kNoParam, project::kNoParam};
    /// The VA's `lfoN.target` and `lfoN.depth`; kNoParam everywhere else.
    std::array<std::uint32_t, 2> legacyTarget{project::kNoParam, project::kNoParam};
    std::array<std::uint32_t, 2> legacyDepth{project::kNoParam, project::kNoParam};

    [[nodiscard]] constexpr std::uint32_t at(ModParam param) const noexcept {
        return base + static_cast<std::uint32_t>(param);
    }
    /// The layout of a table built by withModParams(own) with the whole block.
    [[nodiscard]] static constexpr ModLayout appendedTo(std::uint32_t ownCount) noexcept {
        ModLayout layout;
        layout.base = ownCount;
        layout.lfoRate = {ownCount + static_cast<std::uint32_t>(ModParam::Lfo1Rate),
                          ownCount + static_cast<std::uint32_t>(ModParam::Lfo2Rate)};
        layout.lfoShape = {ownCount + static_cast<std::uint32_t>(ModParam::Lfo1Shape),
                           ownCount + static_cast<std::uint32_t>(ModParam::Lfo2Shape)};
        return layout;
    }
};

/// A voice's modulation state. Plain data: it lives in an instrument's per-voice state.
struct ModVoice {
    std::array<dsp::Envelope, 2> env;
    std::array<dsp::EnvelopeShape, 2> shape;
    std::array<dsp::Lfo, 2> lfo;
    /// Turns per frame, and the shape, as read at the top of the renderVoice call.
    std::array<float, 2> lfoIncrement;
    std::array<dsp::LfoShape, 2> lfoShape;
    /// Frames before the LFO starts, and frames it takes to reach full depth.
    std::array<float, 2> lfoDelay;
    std::array<float, 2> lfoFade;
    std::array<std::uint8_t, 2> lfoSync;
    /// The explicit slots that do something, refreshed on the voice's control grid.
    std::array<dsp::ModSlot, dsp::kModSlots> slots;
    std::uint32_t slotCount;
    /// The pitch envelope: semitones away from the note now, and how it falls.
    double pitchOffset;
    double pitchFactor;
    double pitchStep;
    float pitchAmount;
    float velocity;
    float key;
    float random;
    float wheel;
    float pressure;
    bool usesEnvelopes;
    bool released;
};

/// At the note: resets and triggers everything. `seed` is the voice's identity hash;
/// `clockSeconds` the channel's time, which a free-running LFO keeps.
void modStart(ModVoice& state, const graph::Voice& voice, const graph::VoiceRender& render,
              const ModLayout& layout, std::uint32_t seed, double clockSeconds) noexcept;

/// A mono retrigger: the envelopes restart from where they are, the LFOs keep running.
void modRetrigger(ModVoice& state, const graph::VoiceRender& render,
                  const ModLayout& layout) noexcept;

/// At the top of each renderVoice call: the release, and the LFO rates and shapes.
void modBegin(ModVoice& state, const graph::Voice& voice, const graph::VoiceRender& render,
              const ModLayout& layout) noexcept;

/// One frame. Advances the LFOs, the envelopes and the pitch envelope, and returns the
/// frame's modulation: `scratch` filled in, or the neutral frame when nothing is
/// routed. `control` is the instrument's control(voice, frame).
[[nodiscard]] const dsp::ModFrame& modTick(ModVoice& state, const graph::Voice& voice,
                                           const graph::VoiceRender& render,
                                           const ModLayout& layout, std::uint32_t frame,
                                           bool control, dsp::ModFrame& scratch) noexcept;

/// `voice.mode` and `glide.mode` from an instrument's parameters, for ChannelNode.
[[nodiscard]] VoiceMode modVoiceMode(std::span<const float> params,
                                     const ModLayout& layout) noexcept;
[[nodiscard]] bool modGlideAlways(std::span<const float> params, const ModLayout& layout) noexcept;

} // namespace adx::instruments

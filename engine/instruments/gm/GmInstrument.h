// The `gm` instrument (phase_12.md §4.7.5, closes G8): a General MIDI player, FL's Fruity
// LSD, over an SF2 or DLS bank.
//
// The channel's `BANK=` names the bank (relative to the project, or absolute);
// `system:gm.dls` - and an empty BANK - is the Windows GS Wavetable bank, read in place,
// never copied (gm_dls_never_copied). A bank is parsed once and shared by every channel
// that names it (GmBankCache, gm_bank_shared).
//
// A note plays every zone of the program's preset that covers its key and velocity (an
// SF2 stereo pair is two), each with its own articulation as the bank describes it:
// a DAHDSR volume envelope with SF2's key scaling, a modulation envelope to pitch and
// cutoff, a resonant low-pass, a vibrato LFO and a modulation LFO, and an exclusive class
// that cuts the other notes of its class (sf2_exclusive_class). SF2's default modulators
// are applied: velocity to attenuation and to cutoff.
//
// `program`, `bankMsb`/`bankLsb` and `drums` are read when a note starts, so a program
// change - an automation step on `program` - changes the next note, never one that is
// ringing (gm_program_change).
//
// `part=1` makes the channel LSD's 16-part player: a note plays the part its MIDI channel
// names (a note's `ch=`, a live message's channel), and each part has its own program,
// bank, volume, expression, pan and drum switch (`p1.program` ... `p16.drums`). Part 10
// is the kit unless told otherwise (gm_drums_channel10). Volume and expression are read
// every block, so a CC7 move reaches ringing notes; the program, as in single mode, is
// read when a note starts. An exclusive class cuts only its own part.
//
// Live MIDI reaches it as Controller events: program change, bank select (CC0), the mod
// wheel (CC1, SF2's default modulator: up to 50 cents of vibrato), volume (CC7), pan
// (CC10), expression (CC11), Reset All Controllers (CC121), All Sound Off (CC120) and
// All Notes Off (CC123). A live value overrides the part's parameter until the next
// snapshot replaces the node; pitch bend arrives as a pitch glide, its range from RPN 0.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/dsp/SvFilter.h"
#include "engine/format/soundfont/BankZone.h"
#include "engine/instruments/Instrument.h"

namespace adx::instruments {

/// A 16-part channel's parts, and what each one has: GM's program change, bank select
/// (CC0), volume (CC7), expression (CC11), pan (CC10) and its drum switch.
inline constexpr std::uint32_t kGmParts = 16;
enum class GmPartParam : std::uint32_t {
    Program,
    BankMsb,
    Volume,
    Expression,
    Pan,
    Drums,
    /// CC1: SF2's default modulator, up to 50 cents of vibrato.
    ModWheel,
    Count,
};
inline constexpr std::uint32_t kGmPartParams = static_cast<std::uint32_t>(GmPartParam::Count);

enum class GmParam : std::uint32_t {
    Program,
    BankMsb,
    BankLsb,
    Drums,
    Gain,
    Tune,
    Velocity,
    /// CC1 on a single-part channel.
    ModWheel,
    /// 0 single: one program, every note. 1 sixteen parts: a note plays the part its
    /// MIDI channel names (Note::part, a live message's channel).
    Part,
    /// p1.program, then p1's other five, then p2 ... p16.
    FirstPart,
    Count = FirstPart + (kGmParts * kGmPartParams),
};

/// The index of part `part` (0-15)'s parameter `which`.
[[nodiscard]] constexpr std::uint32_t gmPartParam(std::uint32_t part, GmPartParam which) noexcept {
    return static_cast<std::uint32_t>(GmParam::FirstPart) + (part * kGmPartParams) +
           static_cast<std::uint32_t>(which);
}

namespace detail {
constexpr project::ParamDescriptor gm(std::string_view name, float lo, float hi, float def,
                                      project::Unit unit,
                                      project::ScaleKind scale = project::ScaleKind::Stepped,
                                      project::RateClass rate = project::RateClass::Block) {
    return project::ParamDescriptor{.name = name,
                                    .minimum = lo,
                                    .maximum = hi,
                                    .defaultValue = def,
                                    .unit = unit,
                                    .scale = scale,
                                    .rate = rate,
                                    .curve = project::CurvePart::None};
}
} // namespace detail

inline constexpr auto kGmParams = std::to_array<project::ParamDescriptor>({
    detail::gm("program", 0, 127, 0, project::Unit::Count),
    detail::gm("bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("bankLsb", 0, 127, 0, project::Unit::Count),
    detail::gm("drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("gain", -48, 12, 0, project::Unit::Decibels, project::ScaleKind::Linear,
               project::RateClass::Sample),
    detail::gm("tune", -2400, 2400, 0, project::Unit::Cents, project::ScaleKind::Linear),
    // How much velocity scales the level: 1 is GM's (velocity / 127) squared.
    detail::gm("velocity", 0, 1, 1, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("part", 0, 1, 0, project::Unit::Count),
    // The parts. Volume 100 (GM's reset value) and expression 127 are unity, so a part
    // left alone plays as loud as a single-part channel; the curve is GM's square law
    // either side. Part 10 is the drum kit, as GM has it.
    detail::gm("p1.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p1.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p1.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p1.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p1.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p1.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p1.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p2.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p2.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p2.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p2.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p2.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p2.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p2.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p3.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p3.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p3.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p3.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p3.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p3.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p3.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p4.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p4.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p4.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p4.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p4.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p4.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p4.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p5.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p5.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p5.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p5.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p5.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p5.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p5.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p6.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p6.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p6.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p6.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p6.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p6.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p6.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p7.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p7.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p7.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p7.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p7.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p7.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p7.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p8.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p8.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p8.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p8.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p8.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p8.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p8.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p9.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p9.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p9.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p9.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p9.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p9.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p9.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p10.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p10.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p10.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p10.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p10.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p10.drums", 0, 1, 1, project::Unit::Boolean),
    detail::gm("p10.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p11.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p11.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p11.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p11.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p11.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p11.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p11.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p12.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p12.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p12.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p12.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p12.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p12.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p12.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p13.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p13.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p13.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p13.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p13.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p13.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p13.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p14.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p14.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p14.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p14.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p14.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p14.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p14.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p15.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p15.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p15.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p15.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p15.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p15.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p15.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p16.program", 0, 127, 0, project::Unit::Count),
    detail::gm("p16.bankMsb", 0, 127, 0, project::Unit::Count),
    detail::gm("p16.volume", 0, 127, 100, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p16.expression", 0, 127, 127, project::Unit::Count, project::ScaleKind::Linear),
    detail::gm("p16.pan", -1, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear),
    detail::gm("p16.drums", 0, 1, 0, project::Unit::Boolean),
    detail::gm("p16.modWheel", 0, 127, 0, project::Unit::Count, project::ScaleKind::Linear),
});
static_assert(kGmParams.size() == static_cast<std::size_t>(GmParam::Count));

inline constexpr std::size_t kGmLayers = 4;

/// A bank as the audio thread reads it: flat, immutable views into what the node's
/// pins keep alive (GmSetup.h builds them on the main thread).
struct GmSampleView {
    const float* data;
    std::uint32_t frames;
    std::uint32_t rate;
};
struct GmPresetView {
    std::uint16_t bank;
    std::uint8_t program;
    std::uint32_t firstZone;
    std::uint32_t zoneCount;
};
struct GmBankView {
    std::span<const GmPresetView> presets;
    std::span<const format::soundfont::BankZone> zones;
    std::span<const GmSampleView> samples;
};

/// GM drum kits are bank 128.
inline constexpr std::uint16_t kGmDrumBank = 128;

/// What keeps the views' storage alive. Defined, created and destroyed in
/// GmBankCache.cpp, on the main thread.
struct GmPins;
void destroyGmPins(GmPins* pins) noexcept;

/// One volume or modulation envelope's running state.
struct GmEnvelope {
    std::uint8_t stage; // 0 delay, 1 attack, 2 hold, 3 decay, 4 sustain, 5 release, 6 done
    float level;
    std::uint32_t left;
    float step;
};

struct GmLayer {
    const format::soundfont::BankZone* zone;
    GmSampleView sample;
    double position;
    double rateRatio;
    float baseCents;
    float increment;
    float gainLeft;
    float gainRight;
    GmEnvelope volume;
    GmEnvelope modulation;
    float vibPhase;
    float modPhase;
    std::uint32_t vibDelay;
    std::uint32_t modDelay;
    std::array<dsp::SvFilter, 2> filter;
    dsp::SvfCoefficients coefficients;
    bool filtered;
    bool active;
};

struct GmVoice {
    std::array<GmLayer, kGmLayers> layers;
    std::uint32_t layerCount;
    std::uint16_t exclusiveClass;
    /// The part this voice plays (0 for a single-part channel).
    std::uint8_t part;
    float velocity;
    bool released;
    bool cut;
};

class GmInstrument final : public Instrument<GmVoice> {
public:
    using Instrument::Instrument;
    ~GmInstrument() override;
    GmInstrument(const GmInstrument&) = delete;
    GmInstrument& operator=(const GmInstrument&) = delete;
    GmInstrument(GmInstrument&&) = delete;
    GmInstrument& operator=(GmInstrument&&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "gm";
    }

    /// Main thread, before prepare(): the bank's views (empty for none) and what keeps
    /// them alive. Takes ownership of `pins`.
    void setBank(const GmBankView& view, GmPins* pins) noexcept;
    [[nodiscard]] const GmPins* pins() const noexcept {
        return m_pins;
    }

protected:
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;

private:
    void startLayer(GmLayer& layer, const format::soundfont::BankZone& zone,
                    const graph::Voice& voice, float velocity, float tune) const noexcept;
    static void refreshLayer(GmLayer& layer, float cents) noexcept;

    [[nodiscard]] const GmPresetView* find(std::uint16_t bank, std::uint8_t program) const noexcept;

    /// What live MIDI set on each part, over its parameters; -1 (or !panSet) for
    /// nothing. Single mode keeps its own in part 0.
    struct Live {
        std::int16_t program{-1};
        std::int16_t bank{-1};
        std::int16_t volume{-1};
        std::int16_t expression{-1};
        std::int16_t wheel{-1};
        float pan{0.0F};
        bool panSet{false};
    };

    void controller(const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;

    GmBankView m_view{};
    GmPins* m_pins{nullptr};
    std::array<Live, kGmParts> m_live{};
};

} // namespace adx::instruments

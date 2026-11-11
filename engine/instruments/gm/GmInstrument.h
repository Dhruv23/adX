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
// that cuts the other notes of its class (gm_exclusive_class). SF2's default modulators
// are applied: velocity to attenuation and to cutoff.
//
// `program`, `bankMsb`/`bankLsb` and `drums` are read when a note starts, so a program
// change - an automation step on `program` - changes the next note, never one that is
// ringing (gm_program_change).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "engine/dsp/SvFilter.h"
#include "engine/format/soundfont/BankModel.h"
#include "engine/instruments/Instrument.h"
#include "engine/project/Channel.h"

namespace adx::instruments {

enum class GmParam : std::uint32_t {
    Program,
    BankMsb,
    BankLsb,
    Drums,
    Gain,
    Tune,
    Velocity,
    Count,
};

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
});
static_assert(kGmParams.size() == static_cast<std::size_t>(GmParam::Count));

inline constexpr std::size_t kGmLayers = 4;

/// Banks by file, parsed once (main thread) and shared while any node holds one.
class GmBankCache {
public:
    /// The bank at `path`, loading it if no node holds it. Null (with `error`) when it
    /// cannot be read.
    [[nodiscard]] static std::shared_ptr<const format::soundfont::Bank>
    get(const std::filesystem::path& path, std::string& error);
};

/// The file a channel's BANK= names: `system:gm.dls` and empty are the Windows bank;
/// a relative path is relative to `base`. Empty when there is no such file.
[[nodiscard]] std::filesystem::path gmBankPath(const std::string& written,
                                               const std::filesystem::path& base);

/// One volume or modulation envelope's running state.
struct GmEnvelope {
    std::uint8_t stage; // 0 delay, 1 attack, 2 hold, 3 decay, 4 sustain, 5 release, 6 done
    float level;
    std::uint32_t left;
    float step;
};

struct GmLayer {
    const format::soundfont::BankZone* zone;
    const format::soundfont::BankSample* sample;
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
    float velocity;
    bool released;
    bool cut;
};

class GmInstrument final : public Instrument<GmVoice> {
public:
    using Instrument::Instrument;
    ~GmInstrument() override = default;
    GmInstrument(const GmInstrument&) = delete;
    GmInstrument& operator=(const GmInstrument&) = delete;
    GmInstrument(GmInstrument&&) = delete;
    GmInstrument& operator=(GmInstrument&&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "gm";
    }

    /// Main thread, before prepare(): the bank (null for none) and the path it came from.
    void setBank(std::shared_ptr<const format::soundfont::Bank> bank,
                 std::filesystem::path path) noexcept;
    [[nodiscard]] const std::filesystem::path& bankPath() const noexcept {
        return m_path;
    }
    [[nodiscard]] const format::soundfont::Bank* bank() const noexcept {
        return m_bank.get();
    }

protected:
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;

private:
    void startLayer(GmLayer& layer, const format::soundfont::BankZone& zone,
                    const graph::Voice& voice, float velocity, float tune) const noexcept;
    void refreshLayer(GmLayer& layer, float cents) const noexcept;

    std::shared_ptr<const format::soundfont::Bank> m_bank;
    std::filesystem::path m_path;
};

/// Builds the node's bank from the channel (main thread); a missing bank leaves the node
/// silent and is reported by the caller's validation.
void configureGm(GmInstrument& node, const project::Channel& channel,
                 const std::filesystem::path& base);
[[nodiscard]] bool gmMatches(const GmInstrument& node, const project::Channel& channel,
                             const std::filesystem::path& base);

} // namespace adx::instruments

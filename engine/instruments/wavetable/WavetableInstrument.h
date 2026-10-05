// Wavetable: 2D tables (position x frame), mipmapped, morphed linearly or spectrally
// (phase_4.md §4.8).
//
// A table is a dsp::WaveTable of several frames, each band-limited per octave, so a
// note reads only harmonics below Nyquist. `position` picks a point between frames;
// the linear morph crossfades the two frames either side, the spectral morph reads a
// second table built with three spectrally interpolated frames between each source
// pair (WaveTable::buildSpectralMorph), so a sweep moves formants instead of fading
// between them. Four tables are built in; a fifth is the channel's own `.wav`, cut
// into 2048-sample frames - the de-facto convention - by configureWavetable() on the
// main thread (WavetableSetup.h), and kept alive by pins the node cannot see the type
// of, as a Sampler's samples are. Unison, a lowpass and an LFO on position complete
// the voice.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/dsp/Lfo.h"
#include "engine/dsp/SvFilter.h"
#include "engine/dsp/WaveTable.h"
#include "engine/instruments/Instrument.h"
#include "engine/instruments/wavetable/WavetableParams.h"

namespace adx::instruments {

/// Frames inserted between each source pair, plus one: the spectral table's density.
inline constexpr std::size_t kSpectralSteps = 4;
/// The most 2048-sample frames an imported table keeps.
inline constexpr std::size_t kMaxImportedFrames = 256;
inline constexpr std::size_t kImportFrameLength = 2048;

/// One table in both morph forms.
struct WavetablePair {
    dsp::WaveTable linear;
    dsp::WaveTable spectral;
    /// Frames between two source frames in `spectral`.
    std::size_t steps{kSpectralSteps};
};

/// The built-in tables, built on first use (WavetableSetup.cpp). Main thread.
[[nodiscard]] const WavetablePair& builtinWavetable(WavetableBank bank);

/// What keeps a user table alive, and the file it came from. WavetableSetup.cpp.
struct WavetablePins;
void destroyWavetablePins(WavetablePins* pins) noexcept;

struct WavetableVoice {
    std::array<float, kWavetableMaxUnison> phase;
    std::array<float, kWavetableMaxUnison> detune;
    std::array<float, kWavetableMaxUnison> panLeft;
    std::array<float, kWavetableMaxUnison> panRight;
    std::uint32_t unison;
    float unisonNorm;
    std::array<dsp::SvFilter, 2> filter;
    dsp::SvfCoefficients coefficients;
    dsp::Envelope env;
    dsp::EnvelopeShape shape;
    dsp::Lfo lfo;
    float velocity;
    bool released;
};

class WavetableInstrument final : public Instrument<WavetableVoice> {
public:
    using Instrument::Instrument;
    ~WavetableInstrument() override;
    WavetableInstrument(const WavetableInstrument&) = delete;
    WavetableInstrument& operator=(const WavetableInstrument&) = delete;
    WavetableInstrument(WavetableInstrument&&) = delete;
    WavetableInstrument& operator=(WavetableInstrument&&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "wavetable";
    }

    /// Main thread, before prepare(): the channel's own table (null for none), and
    /// what keeps it alive. Takes ownership of `pins`.
    void setUserTable(const WavetablePair* table, WavetablePins* pins) noexcept;
    [[nodiscard]] const WavetablePins* pins() const noexcept {
        return m_pins;
    }

protected:
    void prepareInstrument(const graph::PrepareInfo& info) override;
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;
    [[nodiscard]] float portamentoSeconds(std::span<const float> params) const noexcept override;

private:
    void refresh(WavetableVoice& state, const graph::VoiceRender& render,
                 std::uint32_t frame) noexcept;

    std::array<const WavetablePair*, kWavetableBankCount> m_tables{};
    const WavetablePair* m_user{nullptr};
    WavetablePins* m_pins{nullptr};
};

} // namespace adx::instruments

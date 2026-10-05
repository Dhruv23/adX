// Voice: sung vocals from an UTAU voicebank, as a renderer plus a sampler
// (phase_4.md §4.13).
//
// The rendering happens off the audio thread (VoiceSetup.h, VoiceRenderCache.h); this
// node only plays what has been rendered. It holds a table, sorted by note id, of each
// note's clip; a note-on finds its clip and plays it, allocation-free, under the
// Phase 3 voice rules. A clip that is not ready when its note starts plays silence -
// the callback never waits.
//
// A sung note starts *before* its beat: the consonant leads the vowel by the
// preutterance, up to a few hundred milliseconds. A node cannot play before the event
// that starts it, so Voice declares a fixed latency, kVoicePrerollSeconds, and PDC
// delays everything else by the same amount (phase_4.md §4.9: "declare the latency
// honestly and it disappears"). A note-on at t then starts its clip at
// t + preroll - lead, which, after compensation, puts the vowel on the beat.
#pragma once

#include <cstdint>

#include "engine/instruments/Instrument.h"
#include "engine/instruments/voice/VoiceClip.h"
#include "engine/instruments/voice/VoiceParams.h"
#include "engine/rt/OwnedArray.h"

namespace adx::instruments {

/// The fixed lead every Voice channel declares. Teto's longest preutterance is under
/// 0.3 s; a lead longer than this is clipped from its start.
inline constexpr double kVoicePrerollSeconds = 0.4;

/// What keeps the clips alive, and what they were rendered from. VoiceSetup.cpp.
struct VoicePins;
void destroyVoicePins(VoicePins* pins) noexcept;

struct VoiceVoice {
    const VoiceClip* clip;
    /// Frames of silence before the clip starts, then the read position in it.
    std::uint32_t wait;
    double position;
    float gain;
};

class VoiceInstrument final : public Instrument<VoiceVoice> {
public:
    using Instrument::Instrument;
    ~VoiceInstrument() override;
    VoiceInstrument(const VoiceInstrument&) = delete;
    VoiceInstrument& operator=(const VoiceInstrument&) = delete;
    VoiceInstrument(VoiceInstrument&&) = delete;
    VoiceInstrument& operator=(VoiceInstrument&&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "voice";
    }
    [[nodiscard]] std::uint32_t latencySamples() const noexcept override {
        return m_preroll;
    }

    /// Main thread, before prepare(): the notes' clips, sorted by note id, and what
    /// keeps them alive. Takes ownership of `pins`.
    void setNotes(rt::OwnedArray<VoiceNoteRef> notes, VoicePins* pins) noexcept;
    [[nodiscard]] const VoicePins* pins() const noexcept {
        return m_pins;
    }

protected:
    void prepareInstrument(const graph::PrepareInfo& info) override;
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;

private:
    [[nodiscard]] const VoiceClip* find(std::uint32_t noteId) const noexcept;

    rt::OwnedArray<VoiceNoteRef> m_notes;
    VoicePins* m_pins{nullptr};
    std::uint32_t m_preroll{0};
    /// Render frames per output frame: 1 at the render rate.
    double m_step{1.0};
};

} // namespace adx::instruments

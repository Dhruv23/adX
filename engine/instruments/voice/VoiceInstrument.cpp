#include "engine/instruments/voice/VoiceInstrument.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/dsp/Math.h"

namespace adx::instruments {
namespace {

using P = VoiceParam;

[[nodiscard]] constexpr std::uint32_t idx(P param) noexcept {
    return static_cast<std::uint32_t>(param);
}

/// The render rate (UtauResampler.h), restated here so this realtime file does not
/// include a main-thread header.
constexpr double kRenderRate = 48000.0;

} // namespace

VoiceInstrument::~VoiceInstrument() {
    destroyVoicePins(m_pins);
}

void VoiceInstrument::setNotes(rt::OwnedArray<VoiceNoteRef> notes, VoicePins* pins) noexcept {
    m_notes = std::move(notes);
    destroyVoicePins(m_pins);
    m_pins = pins;
}

void VoiceInstrument::prepareInstrument(const graph::PrepareInfo& info) {
    m_preroll = static_cast<std::uint32_t>(std::lround(kVoicePrerollSeconds * info.sampleRate));
    m_step = kRenderRate / static_cast<double>(info.sampleRate);
}

const VoiceClip* VoiceInstrument::find(std::uint32_t noteId) const noexcept {
    const std::span<const VoiceNoteRef> notes = m_notes.view();
    const auto found = std::ranges::lower_bound(notes, noteId, {}, &VoiceNoteRef::noteId);
    return found != notes.end() && found->noteId == noteId ? found->clip : nullptr;
}

void VoiceInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                 const graph::VoiceRender& render) noexcept {
    VoiceVoice& state = stateOf(voice);
    const VoiceClip* clip = find(voice.key.noteId);
    state.clip = clip != nullptr && clip->isReady() && clip->frames > 0 ? clip : nullptr;
    state.position = 0.0;
    state.wait = m_preroll;
    if (state.clip != nullptr) {
        // The lead, in output frames; a lead longer than the preroll loses its start.
        const auto lead = static_cast<std::uint32_t>(std::lround(state.clip->lead / m_step));
        if (lead <= m_preroll) {
            state.wait = m_preroll - lead;
        } else {
            state.wait = 0;
            state.position = static_cast<double>(lead - m_preroll) * m_step;
        }
    }
    const float sensitivity = std::clamp(render.param(idx(P::Velocity)), 0.0F, 1.0F);
    const float velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.gain = 1.0F - sensitivity + (sensitivity * velocity);
    voice.level = 0.0F;
}

bool VoiceInstrument::renderVoice(graph::Voice& voice, std::span<float> left,
                                  std::span<float> right,
                                  const graph::VoiceRender& render) noexcept {
    VoiceVoice& state = stateOf(voice);
    if (state.clip == nullptr) {
        std::ranges::fill(left, 0.0F);
        std::ranges::fill(right, 0.0F);
        voice.level = 0.0F;
        return false; // not rendered in time: silent, and done
    }
    const float* samples = state.clip->samples;
    const auto frames = static_cast<double>(state.clip->frames);
    const float level = dsp::dbToGainF(render.param(idx(P::Level)));
    const bool levelAutomated =
        idx(P::Level) < render.automation.size() && render.automation[idx(P::Level)] != nullptr;
    bool alive = true;
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        float s = 0.0F;
        if (state.wait > 0) {
            --state.wait;
        } else if (state.position < frames - 1.0) {
            // The clip is at the render rate; read it at the output rate, linearly - at
            // the render rate itself every read lands on a sample.
            const auto index = static_cast<std::size_t>(state.position);
            const auto t = static_cast<float>(state.position - static_cast<double>(index));
            s = samples[index] + ((samples[index + 1] - samples[index]) * t);
            state.position += m_step;
        } else {
            alive = false;
        }
        const float gain =
            state.gain *
            (levelAutomated ? dsp::dbToGainF(render.paramAt(idx(P::Level), i)) : level);
        left[i] = s * gain;
        right[i] = s * gain;
    }
    voice.level = alive ? state.gain : 0.0F;
    return alive;
}

} // namespace adx::instruments

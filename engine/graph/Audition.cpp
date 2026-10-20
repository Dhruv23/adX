// adx-thread: realtime
#include "engine/graph/Audition.h"

#include <algorithm>
#include <utility>

#include "engine/dsp/Math.h"

namespace adx::graph {

// --- Audition ----------------------------------------------------------------

void Audition::start(std::uint32_t channel, std::uint8_t pitch, std::uint8_t velocity,
                     std::uint32_t frames) noexcept {
    Voice* slot = nullptr;
    for (Voice& voice : m_voices) {
        if (!voice.active) {
            slot = &voice;
            break;
        }
    }
    if (slot == nullptr) {
        // Full: the oldest (lowest id) goes. Its note-off is never sent; the channel's
        // own voice stealing takes care of a voice that is never released.
        slot = &*std::ranges::min_element(m_voices, {}, &Voice::noteId);
    }
    m_nextId = (m_nextId + 1) & (kAuditionNoteBit - 1U);
    *slot = Voice{.channel = channel,
                  .noteId = kAuditionNoteBit | m_nextId,
                  .remaining = std::max<std::uint32_t>(frames, 1),
                  .pitch = std::min<std::uint8_t>(pitch, 127),
                  .velocity = std::clamp<std::uint8_t>(velocity, 1, 127),
                  .started = false,
                  .active = true};
}

void Audition::releaseAll() noexcept {
    for (Voice& voice : m_voices) {
        voice.remaining = std::min<std::uint32_t>(voice.remaining, 1);
    }
}

std::span<const AuditionEvent> Audition::beginBlock(std::uint32_t frames) noexcept {
    std::size_t count = 0;
    const auto emit = [&](const Voice& voice, BlockEventKind kind) {
        BlockEvent event{};
        event.noteId = voice.noteId;
        event.timeSource = kAuditionSource;
        event.kind = kind;
        event.pitch = voice.pitch;
        event.velocity = kind == BlockEventKind::NoteOn ? voice.velocity : 0;
        m_events.at(count++) = AuditionEvent{.channel = voice.channel, .event = event};
    };
    for (Voice& voice : m_voices) {
        if (!voice.active) {
            continue;
        }
        if (!voice.started) {
            emit(voice, BlockEventKind::NoteOn);
            voice.started = true;
            continue;
        }
        if (voice.remaining <= frames) {
            emit(voice, BlockEventKind::NoteOff);
            voice.active = false;
            continue;
        }
        voice.remaining -= frames;
    }
    return {m_events.data(), count};
}

// --- Metronome ---------------------------------------------------------------

namespace {

/// A click's length, and the two pitches: the bar's first beat is higher.
constexpr float kClickSeconds = 0.035F;
constexpr float kBeatHz = 1000.0F;
constexpr float kBarHz = 1600.0F;
constexpr float kClickGain = 0.35F;

} // namespace

void Metronome::setEnabled(bool enabled, std::int64_t beatTicks,
                           std::uint32_t beatsPerBar) noexcept {
    m_enabled = enabled;
    m_beatTicks = std::max<std::int64_t>(beatTicks, 1);
    m_beatsPerBar = std::max<std::uint32_t>(beatsPerBar, 1);
    if (!enabled) {
        m_remaining = 0;
    }
}

void Metronome::render(float* out, std::uint32_t first, std::uint32_t frames,
                       std::uint32_t channels, std::uint32_t sampleRate) noexcept {
    const float increment = m_hz / static_cast<float>(std::max<std::uint32_t>(sampleRate, 1));
    for (std::uint32_t i = first; i < frames && m_remaining > 0; ++i) {
        // A sine under a squared linear decay: short, and no transcendental per sample
        // beyond the deterministic sine.
        const float envelope = static_cast<float>(m_remaining) / static_cast<float>(m_length);
        const float sample = kClickGain * envelope * envelope * dsp::sinTurnsF(m_phase);
        m_phase += increment;
        m_phase -= static_cast<float>(static_cast<int>(m_phase));
        for (std::uint32_t c = 0; c < channels; ++c) {
            out[(static_cast<std::size_t>(i) * channels) + c] += sample;
        }
        --m_remaining;
    }
}

void Metronome::mix(const transport::TimeSource& time, std::int64_t from, std::int64_t to,
                    std::int64_t fromSamples, float* out, std::uint32_t frames,
                    std::uint32_t channels) noexcept {
    if (out == nullptr || channels == 0) {
        return;
    }
    const std::uint32_t rate = time.sampleRate();
    std::uint32_t at = 0;
    if (m_enabled && to > from) {
        // The first beat at or after `from`.
        std::int64_t beat = ((from + m_beatTicks - 1) / m_beatTicks) * m_beatTicks;
        for (; beat < to; beat += m_beatTicks) {
            const std::int64_t offset = time.samplesAt(core::Ticks{beat}) - fromSamples;
            if (offset < 0 || std::cmp_greater_equal(offset, frames)) {
                continue;
            }
            render(out, at, static_cast<std::uint32_t>(offset), channels, rate);
            at = static_cast<std::uint32_t>(offset);
            const bool downbeat = (beat / m_beatTicks) % m_beatsPerBar == 0;
            m_hz = downbeat ? kBarHz : kBeatHz;
            m_phase = 0.0F;
            m_length = std::max<std::uint32_t>(
                1, static_cast<std::uint32_t>(kClickSeconds * static_cast<float>(rate)));
            m_remaining = m_length;
        }
    }
    render(out, at, frames, channels, rate);
}

} // namespace adx::graph

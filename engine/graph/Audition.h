// adx-thread: realtime
//
// Two things the audio thread plays that are not in the arrangement:
//
//   Audition   a note played through one channel's instrument when the user draws it,
//              clicks a key or previews a preset (P5-1). It reaches the channel's node
//              as ordinary note events, on a time source of its own, so a transport stop
//              does not cut it and it needs no snapshot rebuild.
//   Metronome  a click on every beat while the arrangement rolls (P5-3). Mixed into the
//              device output after the graph, never into the master bus or a render:
//              only the realtime engine ever enables it.
//
// Both are fixed-size and allocation-free; both are driven by EngineMessages.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/graph/Node.h"
#include "engine/transport/TimeSource.h"

namespace adx::graph {

/// The time source id audition voices carry: none the transport owns, so a stop or a
/// loop wrap of the arrangement never releases them.
inline constexpr std::uint32_t kAuditionSource = 0xFFFFU;
/// Audition note ids live above this bit, clear of model and arpeggiator ids.
inline constexpr std::uint32_t kAuditionNoteBit = 0x40000000U;

/// One audition event for the step that plays `channel`.
struct AuditionEvent {
    std::uint32_t channel{0};
    BlockEvent event{};
};

class Audition {
public:
    static constexpr std::size_t kMaxVoices = 16;

    /// A note on `channel` for `frames` samples. The oldest is dropped when full.
    void start(std::uint32_t channel, std::uint8_t pitch, std::uint8_t velocity,
               std::uint32_t frames) noexcept;
    /// Releases every sounding audition note at the next block.
    void releaseAll() noexcept;

    /// The events this block delivers - note-ons for new notes, note-offs for expired
    /// ones - and advances every timer by `frames`. Valid until the next call.
    [[nodiscard]] std::span<const AuditionEvent> beginBlock(std::uint32_t frames) noexcept;

private:
    struct Voice {
        std::uint32_t channel{0};
        std::uint32_t noteId{0};
        std::uint32_t remaining{0};
        std::uint8_t pitch{0};
        std::uint8_t velocity{0};
        bool started{false};
        bool active{false};
    };
    std::array<Voice, kMaxVoices> m_voices{};
    std::array<AuditionEvent, kMaxVoices * 2> m_events{};
    std::uint32_t m_nextId{0};
};

class Metronome {
public:
    void setEnabled(bool enabled, std::int64_t beatTicks, std::uint32_t beatsPerBar) noexcept;
    [[nodiscard]] bool enabled() const noexcept {
        return m_enabled;
    }

    /// Adds the clicks that fall in a block the arrangement just played from tick `from`
    /// to tick `to` (exclusive) into the interleaved `out`. `time` converts ticks to
    /// samples; `fromSamples` is where the block started.
    void mix(const transport::TimeSource& time, std::int64_t from, std::int64_t to,
             std::int64_t fromSamples, float* out, std::uint32_t frames,
             std::uint32_t channels) noexcept;

private:
    void render(float* out, std::uint32_t first, std::uint32_t frames, std::uint32_t channels,
                std::uint32_t sampleRate) noexcept;

    bool m_enabled{false};
    std::int64_t m_beatTicks{3840};
    std::uint32_t m_beatsPerBar{4};
    /// The click in progress: samples left, its pitch, and its phase.
    std::uint32_t m_remaining{0};
    std::uint32_t m_length{0};
    float m_hz{1000.0F};
    float m_phase{0.0F};
};

} // namespace adx::graph

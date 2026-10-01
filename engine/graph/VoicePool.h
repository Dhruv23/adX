// Voice allocation, identity and stealing - solved once, so an instrument implements
// one voice's DSP and never the pool (phase_3.md §9).
//
// Two iteration-one defects end here. Note-offs matched voices by MIDI pitch alone
// (`AudioEngine.cpp:1131`), so two channels playing the same pitch released each
// other's notes - the archived code called that an "accepted consequence"
// (FINAL_PLAN §3.3.4). And one 64-voice pool served the whole project, so a busy pad
// stole the kick (§3.3.6). Here a voice is found by exact (channel, note, placement)
// identity, and every channel owns its own pool.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/project/VoiceStealMode.h"

namespace adx::graph {

/// Exact voice identity. `instance` is the playlist placement, so the same note in two
/// overlapping placements of one pattern is two voices, not one voice released twice.
struct VoiceKey {
    std::uint32_t channelId{0};
    std::uint32_t noteId{0};
    std::uint32_t instance{0};

    [[nodiscard]] friend constexpr bool operator==(const VoiceKey&,
                                                   const VoiceKey&) noexcept = default;
};

enum class VoicePhase : std::uint8_t {
    Free,
    /// Sounding, gate held.
    Active,
    /// Gate released; the instrument's release is running.
    Released,
    /// Taken by the stealer: fading out over kStealFadeSeconds, then freed. Does not
    /// count against polyphony - the note that stole it already does.
    Stolen,
};

/// A stolen voice fades rather than cuts. An instantaneous cut is a click, and clicks
/// under load are how a DAW earns a reputation (phase_3.md §4.7).
inline constexpr double kStealFadeSeconds = 0.002;

/// Floats of per-voice state an instrument may keep inline. Enough for an oscillator
/// phase, an envelope and a filter's memory; Phase 4 instruments that need more keep a
/// parallel array indexed by the voice's slot.
inline constexpr std::size_t kVoiceStateFloats = 8;

struct Voice {
    VoiceKey key;
    VoicePhase phase{VoicePhase::Free};
    std::uint8_t pitch{0};
    std::uint8_t velocity{0};
    std::uint32_t timeSource{0};
    /// The pool clock, in samples, when the note started. Age for stealing is this, a
    /// sample count - never wall-clock time, which would make steals differ between an
    /// offline render and its realtime capture (phase_3.md §4.10 condition 7).
    std::uint64_t startedAt{0};
    /// The tick of the note-off, from the note-on event.
    std::int64_t endTick{0};
    /// The voice's current amplitude, kept up to date by the instrument. What
    /// "quietest" means.
    float level{0.0F};
    /// Samples left in a steal fade.
    std::uint32_t fadeRemaining{0};
    std::array<float, kVoiceStateFloats> state{};
};

class VoicePool {
public:
    VoicePool() noexcept = default;

    /// `storage` must hold at least storageFor(maxPolyphony) voices and outlive the
    /// pool. Twice the polyphony, because every stolen voice needs somewhere to fade
    /// while the voice that replaced it starts.
    VoicePool(std::uint16_t maxPolyphony, std::span<Voice> storage,
              std::uint32_t fadeSamples) noexcept;

    [[nodiscard]] static constexpr std::size_t storageFor(std::uint16_t maxPolyphony) noexcept {
        return static_cast<std::size_t>(maxPolyphony == 0 ? 1 : maxPolyphony) * 2;
    }

    /// A voice for `key`, stealing if the pool is at its polyphony limit, in the order
    /// free -> oldest released -> quietest -> oldest. Never allocates. Returns nullptr
    /// only in VoiceStealMode::None at the limit, which is that mode's meaning: the new
    /// note is dropped.
    [[nodiscard]] Voice* allocate(VoiceKey key, project::VoiceStealMode mode) noexcept;

    /// The sounding voice with exactly this key, or nullptr. Released voices with the
    /// key are skipped: a note-off for a voice already in release has nothing to do.
    [[nodiscard]] Voice* find(VoiceKey key) noexcept;

    /// Starts the release of one voice. Idempotent.
    static void release(Voice& voice) noexcept;

    /// Releases every sounding voice driven by `timeSource`.
    void releaseAll(std::uint32_t timeSource) noexcept;

    /// Releases every sounding voice driven by `timeSource` whose note-off is at or
    /// after `tick`. A loop wrap, which would otherwise leave them hanging.
    void releaseEndingAtOrAfter(std::uint32_t timeSource, std::int64_t tick) noexcept;

    /// Returns a voice to the free list. Instruments call it when a release finishes;
    /// the pool calls it when a steal fade does.
    static void free(Voice& voice) noexcept;

    /// Moves the age clock forward. The owning node calls it once per process().
    void advanceClock(std::uint32_t frames) noexcept {
        m_clock += frames;
    }
    [[nodiscard]] std::uint64_t clock() const noexcept {
        return m_clock;
    }

    [[nodiscard]] std::span<Voice> voices() noexcept {
        return m_voices;
    }
    /// Voices counting against polyphony: Active and Released.
    [[nodiscard]] std::uint32_t soundingCount() const noexcept;
    [[nodiscard]] std::uint16_t maxPolyphony() const noexcept {
        return m_maxPolyphony;
    }
    [[nodiscard]] std::uint32_t fadeSamples() const noexcept {
        return m_fadeSamples;
    }

    /// Frees everything immediately. Main thread, or a node's reset().
    void clear() noexcept;

private:
    [[nodiscard]] Voice* chooseVictim(project::VoiceStealMode mode) noexcept;
    [[nodiscard]] Voice* freeSlot() noexcept;

    std::span<Voice> m_voices;
    std::uint16_t m_maxPolyphony{0};
    std::uint32_t m_fadeSamples{96};
    std::uint64_t m_clock{0};
};

} // namespace adx::graph

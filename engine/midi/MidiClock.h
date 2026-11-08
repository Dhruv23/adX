// adx-thread: realtime
//
// MIDI clock, both ways (phase_9.md §4.10).
//
// Following: incoming clock is 24 pulses a quarter note, each with the jitter of
// whatever produced it and the USB stack it crossed. Taking the tempo from the last
// interval is audible wow, so the follower runs the pulses through a delay-locked loop
// (F. Adriaensen, "Using a DLL to filter time", 2005): a second-order loop that tracks
// the pulse period and phase with a set bandwidth. It hands the arrangement's time
// source a rate and a position - an external clock is one more driver of a time source
// (Phase 3's checkpoint), so following is contained, not invasive.
//
// Sending: a pulse every kPpq / 24 ticks of the arrangement, placed at its sample in the
// block, plus start, stop, continue and song position.
#pragma once

#include <cstdint>
#include <span>

#include "engine/core/Time.h"

namespace adx::midi {

inline constexpr std::uint8_t kClockPulse = 0xF8;
inline constexpr std::uint8_t kClockStart = 0xFA;
inline constexpr std::uint8_t kClockContinue = 0xFB;
inline constexpr std::uint8_t kClockStop = 0xFC;
inline constexpr std::uint8_t kSongPosition = 0xF2;
inline constexpr std::int64_t kPulsesPerQuarter = 24;
inline constexpr std::int64_t kTicksPerPulse = core::kPpq / kPulsesPerQuarter;

class ClockFollower {
public:
    /// `bandwidth` in Hz: lower is smoother and slower to follow a tempo change. 0.5 Hz
    /// settles within a couple of bars and holds a jittered clock well inside 0.1 %.
    explicit ClockFollower(std::uint32_t sampleRate, double bandwidth = 0.5) noexcept
        : m_sampleRate(sampleRate), m_bandwidth(bandwidth) {}

    /// One clock pulse at stream sample `at` (MidiEvent::timestampSamples).
    void pulse(std::uint64_t at) noexcept;
    /// Start (position 0), continue, stop, and song position in MIDI beats (16ths).
    void start() noexcept;
    void stop() noexcept;
    void resume() noexcept;
    void songPosition(std::uint16_t sixteenths) noexcept;

    /// True once enough pulses have arrived to say a tempo.
    [[nodiscard]] bool locked() const noexcept {
        return m_pulses >= 3;
    }
    [[nodiscard]] bool running() const noexcept {
        return m_running;
    }
    /// The followed tempo, in beats per minute.
    [[nodiscard]] double bpm() const noexcept;
    /// Where the next pulse puts the arrangement, in ticks: the song position plus the
    /// pulses since it.
    [[nodiscard]] core::Ticks position() const noexcept {
        return core::Ticks{m_ticks};
    }
    /// The filtered stream frame of the last pulse, and the filtered pulse period in
    /// frames (0 until two pulses have arrived).
    [[nodiscard]] double lastPulse() const noexcept {
        return m_last;
    }
    [[nodiscard]] double periodSamples() const noexcept {
        return m_period;
    }

private:
    std::uint32_t m_sampleRate;
    double m_bandwidth;
    // The loop's state, in samples: the filtered time of the last pulse, the predicted
    // time of the next, and the filtered period.
    double m_last{0.0};
    double m_next{0.0};
    double m_period{0.0};
    double m_b{0.0};
    double m_c{0.0};
    std::uint64_t m_pulses{0};
    std::uint64_t m_firstAt{0};
    std::int64_t m_ticks{0};
    bool m_running{false};
    /// The next pulse is the downbeat after a start or a song position: it does not advance.
    bool m_startPending{false};
};

/// What a sender writes into one block: a status byte and where.
struct ClockMessage {
    std::uint32_t offset{0};
    std::uint8_t status{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
};

/// The pulses due while the arrangement plays [fromTick, toTick) over `frames` frames:
/// one at each multiple of kTicksPerPulse, at the frame linear interpolation puts it
/// (exact at constant tempo within a block, which is what a block is). Returns how many
/// were written; more than `out` holds are dropped, which at 24 PPQ needs a block of a
/// whole beat.
std::size_t clockPulses(std::int64_t fromTick, std::int64_t toTick, std::uint32_t frames,
                        std::span<ClockMessage> out) noexcept;

/// The song-position message for `tick`: MIDI beats (sixteenth notes), 14 bits.
[[nodiscard]] ClockMessage songPositionAt(std::int64_t tick, std::uint32_t offset) noexcept;

} // namespace adx::midi

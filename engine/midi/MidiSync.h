// adx-thread: realtime
//
// Sync out and MTC in (STATE.md P9-5, phase_9.md §4.10).
//
// ClockSender turns one block of the arrangement into MIDI clock: start or continue
// with a song position when it starts rolling, a pulse every kTicksPerPulse, stop when
// it stops, and stop / song position / continue when the position jumps (a seek, a loop
// wrap). MtcSender does the same for MIDI time code: quarter-frame messages at 30 fps
// non-drop, with a full-frame message wherever the position jumps.
//
// MtcFollower is the receiving half of MTC: it assembles quarter frames into a
// position, and measures the rate the sender is running at from how fast the code
// advances against the stream clock. Clock following is ClockFollower (MidiClock.h).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/core/TempoMath.h"
#include "engine/core/Time.h"

namespace adx::midi {

inline constexpr std::uint8_t kMtcQuarterFrame = 0xF1;
/// 30 fps non-drop: the rate MtcSender writes, and the default a follower assumes.
inline constexpr std::uint32_t kMtcFps = 30;

/// One message for this block, `offset` frames into it.
struct SyncMessage {
    std::uint32_t offset{0};
    std::array<std::uint8_t, 10> bytes{};
    std::uint8_t size{0};
};

/// The arrangement over one block, as the senders need it. `fromTicks`/`fromSamples`
/// are where the block started on the timeline, `toTicks`/`toSamples` where it ended.
struct SyncBlock {
    std::int64_t fromTicks{0};
    std::int64_t toTicks{0};
    std::int64_t fromSamples{0};
    std::int64_t toSamples{0};
    std::uint32_t frames{0};
    std::uint32_t sampleRate{48000};
    /// Timeline samples per block frame (TimeSource::rate()).
    double rate{1.0};
    bool rolling{false};
    /// The arrangement's tempo map: a pulse is placed on the frame its tick falls on,
    /// not by interpolating ticks, which at 120 bpm are six frames apart.
    core::TempoView tempo;
};

class ClockSender {
public:
    /// Writes this block's clock messages into `out`, in order; returns how many. More
    /// than `out` holds are dropped.
    std::size_t block(const SyncBlock& block, std::span<SyncMessage> out) noexcept;

private:
    bool m_wasRolling{false};
    std::int64_t m_lastTo{0};
};

class MtcSender {
public:
    std::size_t block(const SyncBlock& block, std::span<SyncMessage> out) noexcept;

private:
    bool m_wasRolling{false};
    std::int64_t m_lastTo{0};
};

/// Timecode at 30 fps for a frame count since zero.
struct Timecode {
    std::uint8_t hours{0};
    std::uint8_t minutes{0};
    std::uint8_t seconds{0};
    std::uint8_t frames{0};
};
[[nodiscard]] Timecode timecodeOf(std::int64_t frame, std::uint32_t fps) noexcept;
/// The full-frame SysEx for `code`: F0 7F 7F 01 01 hh mm ss ff F7, hh carrying the rate.
[[nodiscard]] SyncMessage fullFrame(const Timecode& code, std::uint32_t offset) noexcept;
/// The quarter-frame message for piece `piece` (0..7) of `code`.
[[nodiscard]] SyncMessage quarterFrame(const Timecode& code, std::uint32_t piece,
                                       std::uint32_t offset) noexcept;

class MtcFollower {
public:
    /// One quarter-frame data byte (the byte after F1), heard at stream frame `at`.
    void quarter(std::uint8_t data, std::uint64_t at, std::uint32_t sampleRate) noexcept;

    /// True once a whole code has been read and the code is still arriving: a quarter
    /// frame within the last tenth of a second of `now`.
    [[nodiscard]] bool running(std::uint64_t now, std::uint32_t sampleRate) const noexcept;
    /// Where the sender is at stream frame `now`, in seconds: the last whole code plus
    /// the time since, at the measured rate.
    [[nodiscard]] double secondsAt(std::uint64_t now, std::uint32_t sampleRate) const noexcept;
    /// The sender's speed against the stream clock: 1.0 when it runs at real time.
    [[nodiscard]] double rate() const noexcept {
        return m_rate;
    }
    [[nodiscard]] bool locked() const noexcept {
        return m_codes > 0;
    }

private:
    std::array<std::uint8_t, 8> m_pieces{};
    std::uint32_t m_seen{0}; // bit per piece received since piece 0
    std::uint32_t m_expect{0};
    std::uint64_t m_lastQuarter{0};
    std::uint64_t m_codes{0};
    double m_codeSeconds{0.0};
    std::uint64_t m_codeAt{0};
    double m_rate{1.0};
};

} // namespace adx::midi

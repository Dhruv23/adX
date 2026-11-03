// adx-thread: realtime
//
// The sandbox's failure policy (phase_9.md §4.4), as numbers and one counter:
//
//   crashed   the child's process handle is signalled -> bypass, notify, offer restart
//   hung      a block outstanding for kHangMicros     -> kill, restart, restore state
//   late      kLateLimit late blocks in kLateWindow   -> bypass, notify
//
// The counter is the audio thread's: it sees every block and needs no clock but the
// sample count. Crash and hang detection belong to HostProcess's watchdog thread,
// which reads the same LinkHealth.
#pragma once

#include <atomic>
#include <cstdint>

namespace adx::plugin::bridge {

inline constexpr std::int64_t kHangMicros = 2'000'000;
inline constexpr std::uint32_t kLateLimit = 50;
/// The late window, in seconds of audio.
inline constexpr std::uint32_t kLateWindowSeconds = 1;

/// How long the audio thread waits for one block, in microseconds: half the block's
/// duration in realtime - the rest of the graph needs the other half - and ten seconds
/// in an offline render, where nothing is waiting but the render itself.
[[nodiscard]] constexpr std::uint32_t deadlineMicros(std::uint32_t sampleRate, std::uint32_t frames,
                                                     bool offline) noexcept {
    constexpr std::uint32_t kOffline = 10'000'000;
    constexpr std::uint32_t kMinimum = 500;
    if (offline) {
        return kOffline;
    }
    const std::uint64_t rate = sampleRate == 0 ? 48000 : sampleRate;
    const auto half = static_cast<std::uint32_t>((500'000ULL * frames) / rate);
    return half < kMinimum ? kMinimum : half;
}

/// What the audio thread does with one instance.
enum class LinkState : std::uint32_t {
    /// Not loaded yet, or being restarted: output dry (an effect) or silence.
    Idle,
    Running,
    /// The child died. Dry until restarted.
    Crashed,
    /// Bypassed by the late-block rule. Dry until the user re-enables it.
    LateBypassed,
    /// It produced NaN or infinity: muted (silence), never propagated (§5,
    /// plugin_nan_is_contained). Until re-enabled.
    Muted,
};

/// Shared by the audio thread (writer of the counts, reader of the state) and the
/// watchdog and main threads.
struct LinkHealth {
    std::atomic<LinkState> state{LinkState::Idle};
    std::atomic<std::uint64_t> blocks{0};
    std::atomic<std::uint64_t> lateBlocks{0};
    std::atomic<std::uint64_t> nanBlocks{0};
    /// When the outstanding block was sent (nowMicros), or 0 when none is outstanding.
    std::atomic<std::int64_t> waitingSince{0};
    /// Bumped by every (re)load: the node resends every parameter it holds.
    std::atomic<std::uint32_t> generation{0};
    /// Set by the audio thread when it changed the state itself (late, NaN), for the
    /// watchdog to report.
    std::atomic<bool> stateChanged{false};
};

/// Late blocks within a sliding window of audio, counted by the audio thread.
class LateCounter {
public:
    void reset(std::uint32_t sampleRate) noexcept {
        m_windowFrames = static_cast<std::uint64_t>(sampleRate) * kLateWindowSeconds;
        m_elapsed = 0;
        m_late = 0;
    }
    /// One block of `frames`; true when this late block crosses the limit.
    [[nodiscard]] bool block(std::uint32_t frames, bool late) noexcept {
        m_elapsed += frames;
        if (m_elapsed > m_windowFrames) {
            m_elapsed = frames;
            m_late = 0;
        }
        if (!late) {
            return false;
        }
        ++m_late;
        return m_late >= kLateLimit;
    }

private:
    std::uint64_t m_windowFrames{48000};
    std::uint64_t m_elapsed{0};
    std::uint32_t m_late{0};
};

} // namespace adx::plugin::bridge

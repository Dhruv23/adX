// adx-thread: realtime
//
// Everything that runs inside the audio callback on the audio side of the engine,
// separated from the AudioThread that owns it.
//
// The split is what closes Phase 1's P1-2. The realtime ban list used to apply by
// directory, engine/audio/ was deliberately outside it - backends allocate when they
// open - and AudioThread::render, the one function in there that runs on every
// callback, was therefore checked by nobody but the runtime hook. This file carries
// the marker on its first line, tools/lint.py applies the ban to any file that does,
// and so the callback's own code is now under the same static rules as the graph it
// calls. AudioThread keeps the parts that allocate: the backend, the arena's storage.
#pragma once

#include <atomic>
#include <cstdint>

#include "engine/audio/AudioBackend.h"
#include "engine/audio/ScopeTap.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::audio {

/// How long one callback took, from the guards going up to the taps being written.
struct CallbackTiming {
    std::uint32_t durationNs{};
    std::uint32_t frames{};
};

class CallbackCore {
public:
    /// The per-block work, called with every guard installed, `out` cleared to
    /// silence, and the arena reset. This is the seam the render engine plugs into.
    using ProcessStep = void (*)(void* user, float* out, const float* in, std::uint32_t frames,
                                 std::uint32_t channels, const StreamTime& time,
                                 rt::BlockArena& arena) noexcept;

    CallbackCore() noexcept = default;

    /// Main thread, before the stream starts. `storage` outlives the core.
    void attachArena(std::byte* storage, std::size_t bytes) noexcept {
        m_arena = rt::BlockArena{storage, bytes};
    }
    /// Main thread, before the stream starts. The ring is a quarter of a megabyte, too
    /// big to sit inside an engine a test puts on the stack, so its owner keeps it on
    /// the heap; `tap` outlives the core.
    void attachScopeTap(ScopeTapRing& tap) noexcept {
        m_scopeTap = &tap;
    }
    void setProcessStep(ProcessStep step, void* user) noexcept;
    void setOutputChannels(std::uint32_t channels) noexcept {
        m_outputChannels = channels == 0 ? 1 : channels;
    }
    /// The block period the stream promised, for the over-deadline count.
    void setDeadline(std::uint32_t sampleRate, std::uint32_t blockFrames) noexcept;

    /// The RenderCallback every backend is handed. `user` is the CallbackCore.
    static void renderTrampoline(void* user, float* out, const float* in, std::uint32_t frames,
                                 const StreamTime& time) noexcept;

    void render(float* out, const float* in, std::uint32_t frames, const StreamTime& time) noexcept;

    [[nodiscard]] ScopeTapRing& scopeTap() noexcept {
        return *m_scopeTap;
    }
    [[nodiscard]] rt::OverwriteRing<rt::LevelFrame, 256>& levelTap() noexcept {
        return m_levelTap;
    }
    /// One entry per callback. P1-3: a stream that meets its deadline on average
    /// while missing it regularly is what a dropout actually is, and only per-callback
    /// durations can show it.
    [[nodiscard]] rt::OverwriteRing<CallbackTiming, 1024>& timingTap() noexcept {
        return m_timingTap;
    }

    /// The slowest callback since the last reset, in nanoseconds. Any thread.
    [[nodiscard]] std::uint64_t worstCallbackNs() const noexcept {
        return m_worstNs.load(std::memory_order_acquire);
    }
    /// Callbacks that took longer than their block's real-time duration. Any thread.
    [[nodiscard]] std::uint64_t overDeadlineCount() const noexcept {
        return m_overDeadline.load(std::memory_order_acquire);
    }
    /// Main thread, between runs.
    void resetTiming() noexcept {
        m_worstNs.store(0, std::memory_order_release);
        m_overDeadline.store(0, std::memory_order_release);
    }

    /// Fault injection (phase_10.5.md §5, xruns_exposed): every callback spins this long
    /// after its work, so a test can overload the stream on purpose and see the xrun
    /// count move. 0, the default, costs one relaxed load. Any thread.
    void setStallMicroseconds(std::uint32_t microseconds) noexcept {
        m_stallUs.store(microseconds, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t arenaHighWaterMark() const noexcept {
        return m_arena.highWaterMark();
    }
    [[nodiscard]] std::size_t arenaCapacity() const noexcept {
        return m_arena.capacity();
    }

private:
    static void noProcess(void* user, float* out, const float* in, std::uint32_t frames,
                          std::uint32_t channels, const StreamTime& time,
                          rt::BlockArena& arena) noexcept;

    void writeTaps(const float* out, std::uint32_t frames) noexcept;

    ProcessStep m_processStep{&CallbackCore::noProcess};
    void* m_processUser{nullptr};
    std::uint32_t m_outputChannels{2};
    std::uint64_t m_deadlineNs{0};

    rt::BlockArena m_arena;
    ScopeTapRing* m_scopeTap{nullptr};
    rt::OverwriteRing<rt::LevelFrame, 256> m_levelTap;
    rt::OverwriteRing<CallbackTiming, 1024> m_timingTap;
    std::atomic<std::uint64_t> m_worstNs{0};
    std::atomic<std::uint64_t> m_overDeadline{0};
    std::atomic<std::uint32_t> m_stallUs{0};
};

} // namespace adx::audio

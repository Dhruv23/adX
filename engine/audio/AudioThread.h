// The one entry point into the audio callback.
//
// Exactly one place installs the guards, and every backend routes through it
// (phase_1.md §3.11). That is the whole design: if there were two ways into the
// callback, one of them would eventually be missing the RT section, and the allocator
// hook would silently stop watching the code that needed watching most.
//
// Since Phase 3 the callback itself lives in CallbackCore, which the realtime ban list
// checks; this class owns what the callback needs and must not allocate for itself -
// the backend, and the storage behind the per-callback arena.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/audio/AudioBackend.h"
#include "engine/audio/CallbackCore.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/OverwriteRing.h"
#include "engine/rt/Reaper.h"

namespace adx::audio {

/// Bytes of scratch reserved for the per-callback arena.
///
/// The scheduler's per-block event lists and the instruments' voice scratch come from
/// here. Owned by whoever owns the callback, and reset in exactly one place, because
/// highWaterMark() only means something if both are true.
inline constexpr std::size_t kBlockArenaBytes = 1U << 20U;

class AudioThread {
public:
    /// The per-block work. See CallbackCore::ProcessStep.
    using ProcessStep = CallbackCore::ProcessStep;

    explicit AudioThread(std::unique_ptr<AudioBackend> backend);
    ~AudioThread();

    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;
    AudioThread(AudioThread&&) = delete;
    AudioThread& operator=(AudioThread&&) = delete;

    [[nodiscard]] AudioBackend& backend() noexcept {
        return *m_backend;
    }

    /// Replaces the per-block work. Call before start(): the value is read from the
    /// audio thread without synchronisation, which is safe only because creating that
    /// thread is what publishes it.
    void setProcessStep(ProcessStep step, void* user) noexcept {
        m_core.setProcessStep(step, user);
    }

    Error open(const StreamConfig& config);
    Error start();
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] StreamInfo info() const noexcept;

    /// The scope/spectrum tap. Written every callback, read by the UI at 60 Hz.
    [[nodiscard]] rt::OverwriteRing<rt::StereoFrame, 8192>& scopeTap() noexcept {
        return m_core.scopeTap();
    }

    /// Master peak/RMS, one entry per callback rather than per sample.
    [[nodiscard]] rt::OverwriteRing<rt::LevelFrame, 256>& levelTap() noexcept {
        return m_core.levelTap();
    }

    /// Per-callback durations, the slowest one, and how many missed their deadline.
    [[nodiscard]] CallbackCore& callbacks() noexcept {
        return m_core;
    }

    /// Destroys whatever the audio thread retired. Main thread; call on a timer, and
    /// it is called unconditionally at stop.
    std::size_t drainReaper() noexcept {
        return m_reaper.drain();
    }

    [[nodiscard]] rt::Reaper& reaper() noexcept {
        return m_reaper;
    }

    /// Peak arena usage since the arena was last reset to zero. The number that says
    /// whether kBlockArenaBytes is *right*, rather than merely untested.
    [[nodiscard]] std::size_t arenaHighWaterMark() const noexcept {
        return m_core.arenaHighWaterMark();
    }

private:
    std::unique_ptr<AudioBackend> m_backend;

    /// Reserved once, on the main thread, and never resized. The core's arena hands
    /// slices of it out; nothing on the audio thread touches this vector.
    std::vector<std::byte> m_arenaStorage;
    CallbackCore m_core;
    rt::Reaper m_reaper;
};

} // namespace adx::audio

// The one entry point into the audio callback.
//
// Exactly one place installs the guards, and every backend routes through it
// (phase_1.md §3.11). That is the whole design: if there were two ways into the
// callback, one of them would eventually be missing the RT section, and the allocator
// hook would silently stop watching the code that needed watching most.
//
// In Phase 1 the per-block work is nothing at all, and the deliverable is precisely
// that: a correctly guarded thread producing silence.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/audio/AudioBackend.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/OverwriteRing.h"
#include "engine/rt/Reaper.h"

namespace adx::audio {

/// Bytes of scratch reserved for the per-callback arena.
///
/// Nothing uses it yet - Phase 3's scheduler is the first customer - but it is
/// reserved and reset here, because the arena has to be owned by whoever owns the
/// callback, and because highWaterMark() only means something if the reset happens in
/// exactly one place.
inline constexpr std::size_t kBlockArenaBytes = 1U << 20U;

class AudioThread {
public:
    /// The per-block work, called from inside the callback with every guard already
    /// installed and `out` already cleared to silence.
    ///
    /// This is the seam Phase 3 fills with the node graph. It exists now rather than
    /// being introduced later for two reasons: it keeps AudioThread::render the one
    /// place guards are installed even once there is real work to do, and it makes the
    /// guards testable - a test can install a step that reports what the ambient
    /// realtime state looked like from inside a real callback.
    using ProcessStep = void (*)(void* user, float* out, const float* in, std::uint32_t frames,
                                 std::uint32_t channels, const StreamTime& time) noexcept;

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
    void setProcessStep(ProcessStep step, void* user) noexcept;

    Error open(const StreamConfig& config);
    Error start();
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] StreamInfo info() const noexcept;

    /// The scope/spectrum tap. Written every callback, read by the UI at 60 Hz.
    [[nodiscard]] rt::OverwriteRing<rt::StereoFrame, 8192>& scopeTap() noexcept {
        return m_scopeTap;
    }

    /// Master peak/RMS, one entry per callback rather than per sample.
    [[nodiscard]] rt::OverwriteRing<rt::LevelFrame, 256>& levelTap() noexcept {
        return m_levelTap;
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
        return m_arena.highWaterMark();
    }

private:
    /// The RenderCallback handed to every backend.
    static void renderTrampoline(void* user, float* out, const float* in, std::uint32_t frames,
                                 const StreamTime& time) noexcept;

    void render(float* out, const float* in, std::uint32_t frames, const StreamTime& time) noexcept;

    /// The default step: nothing. render() has already cleared the buffer, so doing
    /// nothing is what produces silence.
    static void noProcess(void* user, float* out, const float* in, std::uint32_t frames,
                          std::uint32_t channels, const StreamTime& time) noexcept;

    std::unique_ptr<AudioBackend> m_backend;
    ProcessStep m_processStep{&AudioThread::noProcess};
    void* m_processUser{nullptr};

    /// Cached at open() rather than read from the backend per callback: info() is a
    /// virtual call that builds a StreamInfo, and the channel count cannot change
    /// while a stream is open.
    std::uint32_t m_outputChannels{2};

    /// Reserved once, on the main thread, and never resized. The arena below hands
    /// slices of it out; nothing on the audio thread touches this vector.
    std::vector<std::byte> m_arenaStorage;
    rt::BlockArena m_arena;

    rt::Reaper m_reaper;
    rt::OverwriteRing<rt::StereoFrame, 8192> m_scopeTap;
    rt::OverwriteRing<rt::LevelFrame, 256> m_levelTap;
};

} // namespace adx::audio

#include "engine/audio/AudioThread.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Config.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/Denormal.h"
#include "engine/rt/RtSection.h"

namespace adx::audio {

AudioThread::AudioThread(std::unique_ptr<AudioBackend> backend)
    : m_backend(std::move(backend)), m_arenaStorage(kBlockArenaBytes),
      m_arena(m_arenaStorage.data(), m_arenaStorage.size()) {
    // Also the reference that drags AllocGuard.cpp - and with it the operator new
    // replacements - into this binary at all. See allocGuardLinked() for why that is
    // not a formality.
    rt::installRtGuards();
}

AudioThread::~AudioThread() {
    AudioThread::close();
}

Error AudioThread::open(const StreamConfig& config) {
    Error error = m_backend->open(config, &AudioThread::renderTrampoline, this);
    if (error.code != Error::Code::None) {
        return error;
    }
    // From the backend, not from the request: a device may hand back a different
    // block size or channel count than it was asked for, and the callback has to
    // agree with what it will actually be given.
    m_outputChannels = std::max(1U, m_backend->info().outputChannels);
    return error;
}

Error AudioThread::start() {
    return m_backend->start();
}

void AudioThread::stop() noexcept {
    m_backend->stop();
    // Unconditionally at stop, not only on the UI timer: whatever the audio thread
    // retired in its last callback has nobody else coming to collect it.
    static_cast<void>(m_reaper.drain());
}

void AudioThread::close() noexcept {
    m_backend->close();
    static_cast<void>(m_reaper.drain());
}

StreamInfo AudioThread::info() const noexcept {
    return m_backend->info();
}

void AudioThread::setProcessStep(ProcessStep step, void* user) noexcept {
    m_processStep = step != nullptr ? step : &AudioThread::noProcess;
    m_processUser = user;
}

void AudioThread::noProcess(void* /*user*/, float* /*out*/, const float* /*in*/,
                            std::uint32_t /*frames*/, std::uint32_t /*channels*/,
                            const StreamTime& /*time*/) noexcept {}

void AudioThread::renderTrampoline(void* user, float* out, const float* in, std::uint32_t frames,
                                   const StreamTime& time) noexcept {
    static_cast<AudioThread*>(user)->render(out, in, frames, time);
}

void AudioThread::render(float* out, const float* in, std::uint32_t frames,
                         const StreamTime& time) noexcept {
    // The two guards, in the one place they are installed.
    const rt::ScopedRtSection rtSection;
    const rt::ScopedFlushDenormals denormals;

    ADX_ASSERT(frames <= rt::kMaxBlockFrames);
    if (frames > rt::kMaxBlockFrames) {
        // The assertion records and returns rather than aborting inside a callback, so
        // the overflow still has to be handled: render nothing rather than write past
        // the end of the caller's buffer.
        return;
    }

    m_arena.reset();

    const std::size_t sampleCount = static_cast<std::size_t>(frames) * m_outputChannels;

    // Cleared before the work, not after: a graph that does not fill every channel
    // then leaves silence rather than whatever the device buffer last held.
    std::fill_n(out, sampleCount, 0.0F);
    m_processStep(m_processUser, out, in, frames, m_outputChannels, time);

    // The taps run even over silence, so the meters and the scope are exercised on
    // every callback from this phase onward rather than first being wired up in
    // Phase 6 - if writing them were ever going to allocate, the gate catches it here.
    float peakLeft = 0.0F;
    float peakRight = 0.0F;
    double sumSquaresLeft = 0.0;
    double sumSquaresRight = 0.0;

    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const std::size_t base = static_cast<std::size_t>(frame) * m_outputChannels;
        const float left = out[base];
        const float right = m_outputChannels > 1 ? out[base + 1] : left;

        m_scopeTap.write(rt::StereoFrame{.left = left, .right = right});

        peakLeft = std::max(peakLeft, std::abs(left));
        peakRight = std::max(peakRight, std::abs(right));
        sumSquaresLeft += static_cast<double>(left) * left;
        sumSquaresRight += static_cast<double>(right) * right;
    }

    const double inverseFrames = frames > 0 ? 1.0 / static_cast<double>(frames) : 0.0;
    m_levelTap.write(
        rt::LevelFrame{.peakLeft = peakLeft,
                       .peakRight = peakRight,
                       .rmsLeft = static_cast<float>(std::sqrt(sumSquaresLeft * inverseFrames)),
                       .rmsRight = static_cast<float>(std::sqrt(sumSquaresRight * inverseFrames))});
}

} // namespace adx::audio

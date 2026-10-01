#include "engine/audio/AudioThread.h"

#include <algorithm>
#include <utility>

#include "engine/rt/AllocGuard.h"

namespace adx::audio {

AudioThread::AudioThread(std::unique_ptr<AudioBackend> backend)
    : m_backend(std::move(backend)), m_arenaStorage(kBlockArenaBytes) {
    m_core.attachArena(m_arenaStorage.data(), m_arenaStorage.size());
    // Also the reference that drags AllocGuard.cpp - and with it the operator new
    // replacements - into this binary at all. See allocGuardLinked() for why that is
    // not a formality.
    rt::installRtGuards();
}

AudioThread::~AudioThread() {
    AudioThread::close();
}

Error AudioThread::open(const StreamConfig& config) {
    Error error = m_backend->open(config, &CallbackCore::renderTrampoline, &m_core);
    if (error.code != Error::Code::None) {
        return error;
    }
    // From the backend, not from the request: a device may hand back a different
    // block size or channel count than it was asked for, and the callback has to
    // agree with what it will actually be given.
    const StreamInfo opened = m_backend->info();
    m_core.setOutputChannels(std::max(1U, opened.outputChannels));
    m_core.setDeadline(opened.sampleRate, opened.blockFrames);
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

} // namespace adx::audio

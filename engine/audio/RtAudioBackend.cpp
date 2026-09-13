#include "engine/audio/RtAudioBackend.h"

#include <algorithm>

#include <RtAudio.h>

#include "engine/rt/ThreadId.h"

namespace adx::audio {
namespace {

Error fromRtAudio(RtAudioErrorType code, std::string_view context) {
    Error error;
    error.code = code == RTAUDIO_NO_ERROR ? Error::Code::None : Error::Code::BackendFailure;
    error.message.assign(context);
    return error;
}

} // namespace

RtAudioBackend::RtAudioBackend() {
    // RtAudio's constructor probes the system. It throws on some platforms in older
    // versions and reports through a callback in 6.x; either way, a machine with no
    // audio subsystem at all must leave this object usable-but-empty rather than
    // taking the process down, because enumerate() is called from the UI.
    try {
        m_rtaudio = std::make_unique<RtAudio>();
    } catch (...) {
        m_rtaudio.reset();
    }
}

RtAudioBackend::~RtAudioBackend() {
    RtAudioBackend::close();
}

std::string_view RtAudioBackend::name() const noexcept {
    return "RtAudio";
}

bool RtAudioBackend::hasDevices() {
    return !enumerate().empty();
}

std::span<const DeviceInfo> RtAudioBackend::enumerate() {
    m_devices.clear();
    if (!m_rtaudio) {
        return m_devices;
    }

    const std::string apiName = RtAudio::getApiDisplayName(m_rtaudio->getCurrentApi());
    const std::vector<unsigned int> ids = m_rtaudio->getDeviceIds();
    m_devices.reserve(ids.size());

    for (const unsigned int id : ids) {
        const RtAudio::DeviceInfo probed = m_rtaudio->getDeviceInfo(id);
        if (probed.outputChannels == 0 && probed.inputChannels == 0) {
            continue;
        }

        DeviceInfo device;
        device.id = id;
        device.name.assign(probed.name);
        device.apiName.assign(apiName);
        device.maxOutputChannels = probed.outputChannels;
        device.maxInputChannels = probed.inputChannels;
        device.preferredSampleRate = probed.preferredSampleRate;
        device.isDefaultOutput = probed.isDefaultOutput;
        device.isDefaultInput = probed.isDefaultInput;
        for (const unsigned int rate : probed.sampleRates) {
            if (!device.sampleRates.pushBack(rate)) {
                break; // FixedVector is full; the extras are exotic rates nobody picks
            }
        }
        m_devices.push_back(device);
    }

    return m_devices;
}

Error RtAudioBackend::open(const StreamConfig& config, RenderCallback callback, void* user) {
    if (!m_rtaudio) {
        return Error{.code = Error::Code::NoDevice,
                     .message = rt::FixedString<192>{"RtAudio could not initialise"}};
    }
    if (m_open) {
        return Error{.code = Error::Code::AlreadyOpen,
                     .message = rt::FixedString<192>{"stream is already open"}};
    }
    if (callback == nullptr || config.blockFrames == 0 || config.sampleRate == 0 ||
        config.outputChannels == 0) {
        return Error{.code = Error::Code::InvalidConfig,
                     .message = rt::FixedString<192>{
                         "blockFrames, sampleRate, outputChannels and callback must be non-zero"}};
    }
    if (config.blockFrames > rt::kMaxBlockFrames) {
        return Error{.code = Error::Code::InvalidConfig,
                     .message = rt::FixedString<192>{"blockFrames exceeds rt::kMaxBlockFrames"}};
    }

    unsigned int outputDevice = config.outputDeviceId;
    if (outputDevice == kDefaultDeviceId) {
        outputDevice = m_rtaudio->getDefaultOutputDevice();
    }
    if (outputDevice == 0) {
        return Error{.code = Error::Code::NoDevice,
                     .message = rt::FixedString<192>{"no output device available"}};
    }

    RtAudio::StreamParameters outputParams;
    outputParams.deviceId = outputDevice;
    outputParams.nChannels = config.outputChannels;
    outputParams.firstChannel = 0;

    RtAudio::StreamParameters inputParams;
    const bool wantsInput = config.inputChannels > 0 && config.inputDeviceId != kNoDevice;
    if (wantsInput) {
        inputParams.deviceId = config.inputDeviceId == kDefaultDeviceId
                                   ? m_rtaudio->getDefaultInputDevice()
                                   : config.inputDeviceId;
        inputParams.nChannels = config.inputChannels;
        inputParams.firstChannel = 0;
    }

    m_config = config;
    m_callback = callback;
    m_user = user;
    m_streamFrames = 0;
    m_callbackCount.store(0, std::memory_order_relaxed);
    m_xrunCount.store(0, std::memory_order_relaxed);

    // RtAudio may hand back a different block size than requested. Whatever it
    // chooses is what the config becomes, because the rest of the engine sizes
    // itself from StreamInfo rather than from what was asked for.
    unsigned int bufferFrames = config.blockFrames;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_SCHEDULE_REALTIME;
    options.priority = 1;
    options.streamName = "adX";

    const RtAudioErrorType result = m_rtaudio->openStream(
        &outputParams, wantsInput ? &inputParams : nullptr, RTAUDIO_FLOAT32, config.sampleRate,
        &bufferFrames, &RtAudioBackend::rtAudioTrampoline, this, &options);
    if (result != RTAUDIO_NO_ERROR) {
        m_callback = nullptr;
        return fromRtAudio(result, m_rtaudio->getErrorText());
    }

    m_config.blockFrames = bufferFrames;
    if (bufferFrames > rt::kMaxBlockFrames) {
        // The device insisted on a block larger than every fixed buffer in the engine
        // is sized for. Refusing is the only safe answer; silently truncating would
        // produce glitched audio and a passing test.
        m_rtaudio->closeStream();
        m_callback = nullptr;
        return Error{.code = Error::Code::UnsupportedFormat,
                     .message = rt::FixedString<192>{
                         "device requires a block larger than rt::kMaxBlockFrames"}};
    }

    m_open = true;
    updateLatency();
    return Error{};
}

void RtAudioBackend::updateLatency() noexcept {
    if (!m_rtaudio || m_config.sampleRate == 0) {
        return;
    }

    // getStreamLatency() reports frames, and reports 0 on any API that does not know -
    // which includes WASAPI before the stream is actually running. Zero is not a
    // latency; it is "no answer", and showing a user 0.00 ms is worse than showing them
    // the part we can prove. One block is the floor every stream pays regardless of what
    // the driver adds on top, so that is the fallback.
    const auto reportedFrames = static_cast<double>(m_rtaudio->getStreamLatency());
    const auto blockFrames = static_cast<double>(m_config.blockFrames);
    const double frames = reportedFrames > 0.0 ? reportedFrames : blockFrames;
    m_outputLatencyMs = 1000.0 * frames / static_cast<double>(m_config.sampleRate);
    m_latencyIsMeasured = reportedFrames > 0.0;
}

Error RtAudioBackend::start() {
    if (!m_open || !m_rtaudio) {
        return Error{.code = Error::Code::NotOpen,
                     .message = rt::FixedString<192>{"open() the stream first"}};
    }
    if (m_running) {
        return Error{};
    }
    const RtAudioErrorType result = m_rtaudio->startStream();
    if (result != RTAUDIO_NO_ERROR) {
        return fromRtAudio(result, m_rtaudio->getErrorText());
    }
    m_running = true;
    // Re-read now that the stream is live: WASAPI has nothing to report until then.
    updateLatency();
    return Error{};
}

void RtAudioBackend::stop() noexcept {
    if (!m_running || !m_rtaudio) {
        return;
    }
    static_cast<void>(m_rtaudio->stopStream());
    m_running = false;
}

void RtAudioBackend::close() noexcept {
    stop();
    if (m_rtaudio && m_rtaudio->isStreamOpen()) {
        m_rtaudio->closeStream();
    }
    m_open = false;
    m_callback = nullptr;
    m_user = nullptr;
    m_threadRegistered = false;
}

int RtAudioBackend::rtAudioTrampoline(void* outputBuffer, void* inputBuffer, unsigned int frames,
                                      double streamTime, unsigned int status, void* user) {
    return static_cast<RtAudioBackend*>(user)->handleCallback(outputBuffer, inputBuffer, frames,
                                                              streamTime, status);
}

int RtAudioBackend::handleCallback(void* outputBuffer, void* inputBuffer, unsigned int frames,
                                   double streamTime, unsigned int status) noexcept {
    // RtAudio owns this thread, so there is no runLoop to register it in. First
    // callback it is - a plain bool, because only this thread ever reads or writes it.
    if (!m_threadRegistered) {
        rt::registerAudioThread();
        m_threadRegistered = true;
    }

    if (status != 0) {
        m_xrunCount.fetch_add(1, std::memory_order_relaxed);
    }

    const StreamTime time{.streamFrames = m_streamFrames, .streamSeconds = streamTime};
    m_callback(m_user, static_cast<float*>(outputBuffer), static_cast<const float*>(inputBuffer),
               frames, time);

    m_streamFrames += frames;
    m_callbackCount.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

StreamInfo RtAudioBackend::info() const noexcept {
    StreamInfo out;
    out.isOpen = m_open;
    out.isRunning = m_running;
    out.sampleRate = m_config.sampleRate;
    out.blockFrames = m_config.blockFrames;
    out.outputChannels = m_config.outputChannels;
    out.inputChannels = m_config.inputChannels;
    out.outputLatencyMs = m_outputLatencyMs;
    out.inputLatencyMs = m_config.inputChannels > 0 ? m_outputLatencyMs : 0.0;
    out.roundTripLatencyMs = out.outputLatencyMs + out.inputLatencyMs;
    out.callbackCount = m_callbackCount.load(std::memory_order_relaxed);
    out.xrunCount = m_xrunCount.load(std::memory_order_relaxed);
    out.backendName.assign("RtAudio");
    return out;
}

} // namespace adx::audio

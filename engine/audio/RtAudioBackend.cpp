#include "engine/audio/RtAudioBackend.h"

#include <algorithm>
#include <utility>

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
} // namespace adx::audio

#if defined(ADX_ASIO_COMPILED)
// asio.cpp in RtAudio's static library defines it; asio.h itself needs windows.h.
// NOLINTNEXTLINE(google-runtime-int,readability-identifier-naming) - the SDK's own name and types
long ASIOGetLatencies(long* inputLatency, long* outputLatency);
#endif

namespace adx::audio {
namespace {

/// The driver's own split, when the stream is ASIO: {input, output} frames, or {0, 0}.
std::pair<double, double> asioLatencies(RtAudio& rtaudio) noexcept {
#if defined(ADX_ASIO_COMPILED) && defined(_WIN32)
    if (rtaudio.getCurrentApi() == RtAudio::WINDOWS_ASIO && rtaudio.isStreamOpen()) {
        long input = 0;  // NOLINT(google-runtime-int) - ASIO's own type
        long output = 0; // NOLINT(google-runtime-int)
        if (::ASIOGetLatencies(&input, &output) == 0 && input >= 0 && output >= 0) {
            return {static_cast<double>(input), static_cast<double>(output)};
        }
    }
#else
    static_cast<void>(rtaudio);
#endif
    return {0.0, 0.0};
}

} // namespace

RtAudioBackend::RtAudioBackend(AudioApi api) {
    // RtAudio's constructor probes the system. It throws on some platforms in older
    // versions and reports through a callback in 6.x; either way, a machine with no
    // audio subsystem at all must leave this object usable-but-empty rather than
    // taking the process down, because enumerate() is called from the UI.
    //
    // Named, never left to RtAudio: its own default is the first compiled API in its
    // list, and ASIO comes before WASAPI there - compiling ASIO in would otherwise move
    // every user to it.
    RtAudio::Api chosen = RtAudio::UNSPECIFIED;
#if defined(_WIN32)
    chosen =
        api == AudioApi::Asio && asioCompiled() ? RtAudio::WINDOWS_ASIO : RtAudio::WINDOWS_WASAPI;
#else
    static_cast<void>(api);
#endif
    try {
        m_rtaudio = std::make_unique<RtAudio>(chosen);
    } catch (...) {
        m_rtaudio.reset();
    }
}

bool RtAudioBackend::asioCompiled() noexcept {
    std::vector<RtAudio::Api> apis;
    RtAudio::getCompiledApi(apis);
    return std::ranges::find(apis, RtAudio::WINDOWS_ASIO) != apis.end();
}

std::string RtAudioBackend::apiName() const {
    return m_rtaudio ? RtAudio::getApiDisplayName(m_rtaudio->getCurrentApi()) : std::string{};
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
    //
    // With an input open, RtAudio's figure is input plus output (its getStreamLatency()
    // sums both directions), so it is the round trip and is split, not doubled. ASIO
    // says how it divides; for any other API the halves are an even split of a true sum.
    const auto reportedFrames = static_cast<double>(m_rtaudio->getStreamLatency());
    const auto blockFrames = static_cast<double>(m_config.blockFrames);
    const double toMs = 1000.0 / static_cast<double>(m_config.sampleRate);
    m_latencyIsMeasured = reportedFrames > 0.0;
    const bool duplex = m_config.inputChannels > 0 && m_config.inputDeviceId != kNoDevice;
    if (!duplex) {
        m_outputLatencyMs = toMs * (m_latencyIsMeasured ? reportedFrames : blockFrames);
        m_inputLatencyMs = 0.0;
        return;
    }
    if (!m_latencyIsMeasured) {
        m_outputLatencyMs = toMs * blockFrames;
        m_inputLatencyMs = toMs * blockFrames;
        return;
    }
    const auto [asioInput, asioOutput] = asioLatencies(*m_rtaudio);
    if (asioInput + asioOutput > 0.0) {
        m_inputLatencyMs = toMs * asioInput;
        m_outputLatencyMs = toMs * asioOutput;
    } else {
        m_inputLatencyMs = toMs * reportedFrames / 2.0;
        m_outputLatencyMs = toMs * reportedFrames / 2.0;
    }
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
    out.inputLatencyMs = m_inputLatencyMs;
    out.roundTripLatencyMs = out.outputLatencyMs + out.inputLatencyMs;
    out.callbackCount = m_callbackCount.load(std::memory_order_relaxed);
    out.xrunCount = m_xrunCount.load(std::memory_order_relaxed);
    out.backendName.assign("RtAudio");
    return out;
}

} // namespace adx::audio

#include "engine/audio/OfflineBackend.h"

#include <algorithm>

#include "engine/rt/ThreadId.h"

namespace adx::audio {
namespace {

DeviceInfo makeOfflineDevice() {
    DeviceInfo device;
    device.id = 0;
    device.name.assign("Offline render (no device)");
    device.apiName.assign("Offline");
    device.maxOutputChannels = 2;
    device.maxInputChannels = 0;
    device.preferredSampleRate = 48000;
    for (const std::uint32_t rate : {44100U, 48000U, 88200U, 96000U, 192000U}) {
        static_cast<void>(device.sampleRates.pushBack(rate));
    }
    return device;
}

} // namespace

OfflineBackend::OfflineBackend() {
    m_devices.push_back(makeOfflineDevice());
}

OfflineBackend::~OfflineBackend() {
    OfflineBackend::close();
}

std::string_view OfflineBackend::name() const noexcept {
    return "Offline";
}

std::span<const DeviceInfo> OfflineBackend::enumerate() {
    return m_devices;
}

Error OfflineBackend::open(const StreamConfig& config, RenderCallback callback, void* user) {
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

    m_config = config;
    m_callback = callback;
    m_user = user;
    m_block.assign(static_cast<std::size_t>(config.blockFrames) * config.outputChannels, 0.0F);
    m_input.assign(static_cast<std::size_t>(config.blockFrames) * config.inputChannels, 0.0F);
    m_rendered.clear();
    m_framesRendered = 0;
    m_callbackCount.store(0, std::memory_order_relaxed);
    m_open = true;
    return Error{};
}

Error OfflineBackend::start() {
    if (!m_open) {
        return Error{.code = Error::Code::NotOpen,
                     .message = rt::FixedString<192>{"open() the stream first"}};
    }
    return Error{};
}

void OfflineBackend::stop() noexcept {}

void OfflineBackend::close() noexcept {
    m_open = false;
    m_callback = nullptr;
    m_user = nullptr;
}

std::span<const float> OfflineBackend::renderFrames(std::uint64_t frames) {
    if (!m_open || m_callback == nullptr) {
        return {};
    }

    // Grown once, before any callback runs, so nothing allocates between entering and
    // leaving a render callback - which is what lets the allocator hook hold offline
    // render to the same standard as a live stream.
    const auto totalSamples =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(m_config.outputChannels);
    m_rendered.assign(totalSamples, 0.0F);

    // The calling thread *is* the audio thread for the duration of a render.
    rt::registerAudioThread();

    StreamTime time{};
    std::uint64_t done = 0;
    std::size_t writeOffset = 0;
    while (done < frames) {
        const std::uint64_t remaining = frames - done;
        const auto thisBlock = static_cast<std::uint32_t>(
            std::min(remaining, static_cast<std::uint64_t>(m_config.blockFrames)));

        time.streamFrames = m_framesRendered + done;
        time.streamSeconds =
            static_cast<double>(time.streamFrames) / static_cast<double>(m_config.sampleRate);

        const float* input = m_input.empty() ? nullptr : m_input.data();
        m_callback(m_user, m_block.data(), input, thisBlock, time);
        m_callbackCount.fetch_add(1, std::memory_order_relaxed);

        const auto produced =
            static_cast<std::size_t>(thisBlock) * static_cast<std::size_t>(m_config.outputChannels);
        std::copy_n(m_block.begin(), produced,
                    m_rendered.begin() + static_cast<std::ptrdiff_t>(writeOffset));
        writeOffset += produced;
        done += thisBlock;
    }

    rt::unregisterAudioThread();
    m_framesRendered += frames;
    return m_rendered;
}

StreamInfo OfflineBackend::info() const noexcept {
    StreamInfo out;
    out.isOpen = m_open;
    out.isRunning = false;
    out.sampleRate = m_config.sampleRate;
    out.blockFrames = m_config.blockFrames;
    out.outputChannels = m_config.outputChannels;
    out.inputChannels = m_config.inputChannels;
    // No device, so no device latency. Zero here is a fact, not a placeholder.
    out.outputLatencyMs = 0.0;
    out.inputLatencyMs = 0.0;
    out.roundTripLatencyMs = 0.0;
    out.callbackCount = m_callbackCount.load(std::memory_order_relaxed);
    out.xrunCount = 0;
    out.backendName.assign("Offline");
    return out;
}

} // namespace adx::audio

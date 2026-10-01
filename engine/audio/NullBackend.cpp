#include "engine/audio/NullBackend.h"

#include <algorithm>
#include <chrono>

#include "engine/rt/ThreadId.h"

namespace adx::audio {
namespace {

constexpr std::uint32_t kNullDeviceId = 0;

DeviceInfo makeNullDevice() {
    DeviceInfo device;
    device.id = kNullDeviceId;
    device.name.assign("Null output (no device)");
    device.apiName.assign("Null");
    device.maxOutputChannels = 2;
    device.maxInputChannels = 0;
    device.preferredSampleRate = 48000;
    for (const std::uint32_t rate : {44100U, 48000U, 88200U, 96000U, 192000U}) {
        static_cast<void>(device.sampleRates.pushBack(rate));
    }
    device.isDefaultOutput = true;
    device.isDefaultInput = false;
    return device;
}

} // namespace

NullBackend::NullBackend() {
    m_devices.push_back(makeNullDevice());
}

NullBackend::~NullBackend() {
    close();
}

std::string_view NullBackend::name() const noexcept {
    return "Null";
}

std::span<const DeviceInfo> NullBackend::enumerate() {
    return m_devices;
}

Error NullBackend::open(const StreamConfig& config, RenderCallback callback, void* user) {
    if (m_open.load(std::memory_order_acquire)) {
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
                     .message =
                         rt::FixedString<192>{"blockFrames exceeds adx::rt::kMaxBlockFrames"}};
    }

    m_config = config;
    m_callback = callback;
    m_user = user;

    // Sized once, here, on the main thread. The audio thread never resizes anything.
    m_output.assign(static_cast<std::size_t>(config.blockFrames) * config.outputChannels, 0.0F);
    m_input.assign(static_cast<std::size_t>(config.blockFrames) * config.inputChannels, 0.0F);

    m_callbackCount.store(0, std::memory_order_relaxed);
    m_lateCount.store(0, std::memory_order_relaxed);
    m_open.store(true, std::memory_order_release);
    return Error{};
}

Error NullBackend::start() {
    if (!m_open.load(std::memory_order_acquire)) {
        return Error{.code = Error::Code::NotOpen,
                     .message = rt::FixedString<192>{"open() the stream first"}};
    }
    if (m_running.load(std::memory_order_acquire)) {
        return Error{};
    }
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this]() noexcept { runLoop(); });
    return Error{};
}

void NullBackend::stop() noexcept {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void NullBackend::close() noexcept {
    stop();
    m_open.store(false, std::memory_order_release);
    m_callback = nullptr;
    m_user = nullptr;
}

void NullBackend::runLoop() noexcept {
    rt::registerAudioThread();

    using Clock = std::chrono::steady_clock;
    const auto startedAt = Clock::now();
    const auto framesPerSecond = static_cast<double>(m_config.sampleRate);

    std::uint64_t framesIssued = 0;
    StreamTime time{};

    while (m_running.load(std::memory_order_acquire)) {
        // The deadline for this buffer is computed from the *start* of the stream, not
        // by adding a period to the previous wake-up. Windows' timer granularity is
        // coarser than a 5.3 ms block, so sleep_until routinely overshoots; deriving
        // every deadline from the origin means an overshoot is absorbed by the next
        // iteration not sleeping at all, instead of accumulating into drift.
        //
        // That is what makes the 60-second gate assertable: the callback *count* after
        // 60 s is elapsed/period regardless of jitter, so the test can hold it to a
        // tight tolerance while the individual wake-ups stay as sloppy as the OS likes.
        const auto offsetSeconds = static_cast<double>(framesIssued) / framesPerSecond;
        const auto deadline = startedAt + std::chrono::duration_cast<Clock::duration>(
                                              std::chrono::duration<double>(offsetSeconds));

        const auto now = Clock::now();
        if (now < deadline) {
            std::this_thread::sleep_until(deadline);
        } else if (framesIssued > 0 && now - deadline > std::chrono::milliseconds(5)) {
            // Behind by more than a block. On a real device this is an xrun; here it is
            // only ever the host being busy, but it is reported the same way so the
            // number means the same thing in both.
            m_lateCount.fetch_add(1, std::memory_order_relaxed);
        }

        time.streamFrames = framesIssued;
        time.streamSeconds = offsetSeconds;

        const float* input = m_input.empty() ? nullptr : m_input.data();
        m_callback(m_user, m_output.data(), input, m_config.blockFrames, time);
        captureBlock();

        framesIssued += m_config.blockFrames;
        m_callbackCount.fetch_add(1, std::memory_order_relaxed);
    }

    rt::unregisterAudioThread();
}

void NullBackend::setCapture(std::span<float> capture) noexcept {
    m_capture = capture;
    m_capturedFrames.store(0, std::memory_order_release);
}

void NullBackend::captureBlock() noexcept {
    const std::size_t channels = m_config.outputChannels;
    const std::uint64_t captured = m_capturedFrames.load(std::memory_order_relaxed);
    const std::size_t capacity = channels == 0 ? 0 : m_capture.size() / channels;
    if (captured >= capacity) {
        return;
    }
    const std::size_t frames =
        std::min<std::size_t>(m_config.blockFrames, capacity - static_cast<std::size_t>(captured));
    // A copy into storage the main thread sized before start(): nothing here can
    // allocate, so capturing does not perturb the thing it is capturing.
    std::copy_n(m_output.data(), frames * channels,
                m_capture.begin() + static_cast<std::ptrdiff_t>(captured * channels));
    m_capturedFrames.store(captured + frames, std::memory_order_release);
}

StreamInfo NullBackend::info() const noexcept {
    StreamInfo out;
    out.isOpen = m_open.load(std::memory_order_acquire);
    out.isRunning = m_running.load(std::memory_order_acquire);
    out.sampleRate = m_config.sampleRate;
    out.blockFrames = m_config.blockFrames;
    out.outputChannels = m_config.outputChannels;
    out.inputChannels = m_config.inputChannels;

    // One block of output buffering and nothing else - there is no device queue to
    // add. Reported rather than left at zero so the readout has the same shape for
    // every backend.
    const double blockMs = m_config.sampleRate == 0
                               ? 0.0
                               : 1000.0 * static_cast<double>(m_config.blockFrames) /
                                     static_cast<double>(m_config.sampleRate);
    out.outputLatencyMs = blockMs;
    out.inputLatencyMs = m_config.inputChannels > 0 ? blockMs : 0.0;
    out.roundTripLatencyMs = out.outputLatencyMs + out.inputLatencyMs;
    out.callbackCount = m_callbackCount.load(std::memory_order_relaxed);
    out.xrunCount = m_lateCount.load(std::memory_order_relaxed);
    out.backendName.assign("Null");
    return out;
}

} // namespace adx::audio

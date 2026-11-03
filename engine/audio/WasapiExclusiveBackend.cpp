#include "engine/audio/WasapiExclusiveBackend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <thread>

#include "engine/rt/ThreadId.h"

#if defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>

#    include <audioclient.h>
#    include <avrt.h>
#    include <ks.h>
#    include <ksmedia.h>
#    include <mmdeviceapi.h>
#    include <mmreg.h>

// After mmdeviceapi.h, which brings in the property-key macros it uses.
#    include <functiondiscoverykeys_devpkey.h>
#endif

namespace adx::audio {

namespace {

Error failure(Error::Code code, std::string_view message) {
    Error error;
    error.code = code;
    error.message.assign(message);
    return error;
}

/// What the device takes, in the order tried: float first (no conversion), then the
/// integer layouts drivers that refuse float in exclusive mode usually accept.
enum class SampleFormat : std::uint8_t { Float32, Pcm32, Pcm24, Pcm16 };

constexpr std::string_view formatText(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Float32:
        return "float32";
    case SampleFormat::Pcm32:
        return "pcm24in32";
    case SampleFormat::Pcm24:
        return "pcm24";
    case SampleFormat::Pcm16:
        return "pcm16";
    }
    return "";
}

} // namespace

#if defined(_WIN32)

namespace {

template<class T> void release(T*& pointer) noexcept {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

WAVEFORMATEXTENSIBLE waveFormat(SampleFormat format, std::uint32_t rate, std::uint32_t channels) {
    WORD container = 32;
    WORD valid = 32;
    if (format == SampleFormat::Pcm32) {
        valid = 24;
    } else if (format == SampleFormat::Pcm24) {
        container = 24;
        valid = 24;
    } else if (format == SampleFormat::Pcm16) {
        container = 16;
        valid = 16;
    }
    WAVEFORMATEXTENSIBLE out{};
    out.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    out.Format.nChannels = static_cast<WORD>(channels);
    out.Format.nSamplesPerSec = rate;
    out.Format.wBitsPerSample = container;
    out.Format.nBlockAlign = static_cast<WORD>(channels * container / 8U);
    out.Format.nAvgBytesPerSec = rate * out.Format.nBlockAlign;
    out.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    out.Samples.wValidBitsPerSample = valid;
    out.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    out.SubFormat = format == SampleFormat::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                                    : KSDATAFORMAT_SUBTYPE_PCM;
    return out;
}

/// 100 ns units, WASAPI's REFERENCE_TIME.
REFERENCE_TIME durationOf(std::uint32_t frames, std::uint32_t rate) {
    return static_cast<REFERENCE_TIME>(
        std::llround(1.0e7 * static_cast<double>(frames) / static_cast<double>(rate)));
}

std::string narrow(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace

struct WasapiExclusiveBackend::Device {
    bool comOwned{false};
    IMMDevice* device{nullptr};
    IAudioClient* client{nullptr};
    IAudioRenderClient* render{nullptr};
    IAudioClock* clock{nullptr};
    HANDLE event{nullptr};
    std::thread thread;
    SampleFormat format{SampleFormat::Float32};
    std::uint32_t bufferFrames{0};
    std::uint64_t clockFrequency{0};
    /// Frames handed to the device since start, the silent first buffer included: the
    /// device clock counts from the same frame.
    std::uint64_t writtenFrames{0};

    void releaseAll() noexcept {
        release(clock);
        release(render);
        release(client);
        release(device);
        if (event != nullptr) {
            CloseHandle(event);
            event = nullptr;
        }
    }
};

WasapiExclusiveBackend::WasapiExclusiveBackend() : m_device(std::make_unique<Device>()) {}

WasapiExclusiveBackend::~WasapiExclusiveBackend() {
    WasapiExclusiveBackend::close();
}

std::string_view WasapiExclusiveBackend::name() const noexcept {
    return "WASAPI exclusive";
}

std::span<const DeviceInfo> WasapiExclusiveBackend::enumerate() {
    m_devices.clear();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        DeviceInfo info;
        info.id = kDefaultDeviceId;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value))) {
            info.name.assign(narrow(value.pwszVal));
        }
        PropVariantClear(&value);
        info.apiName.assign("WASAPI exclusive");
        info.maxOutputChannels = 2;
        info.preferredSampleRate = 48000;
        info.isDefaultOutput = true;
        m_devices.push_back(info);
    }
    release(props);
    release(device);
    release(enumerator);
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return m_devices;
}

Error WasapiExclusiveBackend::open(const StreamConfig& config, RenderCallback callback,
                                   void* user) {
    if (m_open) {
        return failure(Error::Code::AlreadyOpen, "stream is already open");
    }
    if (callback == nullptr || config.blockFrames == 0 || config.sampleRate == 0 ||
        config.outputChannels == 0) {
        return failure(Error::Code::InvalidConfig,
                       "blockFrames, sampleRate, outputChannels and callback must be non-zero");
    }
    if (config.inputChannels > 0 && config.inputDeviceId != kNoDevice) {
        return failure(Error::Code::UnsupportedFormat,
                       "exclusive mode is output-only; record in shared mode");
    }
    if (config.outputDeviceId != kDefaultDeviceId) {
        return failure(Error::Code::InvalidConfig, "exclusive mode opens the default output only");
    }

    Device& d = *m_device;
    // A thread Qt made single-threaded refuses MTA (RPC_E_CHANGED_MODE); COM is still
    // usable there, and the audio clients are free-threaded.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    d.comOwned = SUCCEEDED(com);

    const auto fail = [&](Error::Code code, std::string_view message) {
        d.releaseAll();
        if (d.comOwned) {
            CoUninitialize();
            d.comOwned = false;
        }
        return failure(code, message);
    };

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)))) {
        return fail(Error::Code::BackendFailure, "no MMDeviceEnumerator");
    }
    const HRESULT found = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &d.device);
    release(enumerator);
    if (FAILED(found)) {
        return fail(Error::Code::NoDevice, "no output device available");
    }
    const auto activate = [&] {
        return d.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&d.client));
    };
    if (FAILED(activate())) {
        return fail(Error::Code::BackendFailure, "could not activate the output device");
    }

    constexpr std::array kFormats{SampleFormat::Float32, SampleFormat::Pcm32, SampleFormat::Pcm24,
                                  SampleFormat::Pcm16};
    bool supported = false;
    WAVEFORMATEXTENSIBLE format{};
    for (const SampleFormat candidate : kFormats) {
        format = waveFormat(candidate, config.sampleRate, config.outputChannels);
        if (d.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr) ==
            S_OK) {
            d.format = candidate;
            supported = true;
            break;
        }
    }
    if (!supported) {
        return fail(Error::Code::UnsupportedFormat,
                    "the device takes no exclusive format at this rate and channel count");
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    d.client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    REFERENCE_TIME period =
        std::max(minimumPeriod, durationOf(config.blockFrames, config.sampleRate));
    HRESULT hr =
        d.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                             period, &format.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // The driver wants a period that is a whole number of its own frames: it says
        // which in GetBufferSize, and a fresh client takes that one.
        UINT32 aligned = 0;
        d.client->GetBufferSize(&aligned);
        release(d.client);
        if (FAILED(activate())) {
            return fail(Error::Code::BackendFailure, "could not re-activate the output device");
        }
        period = durationOf(aligned, config.sampleRate);
        hr = d.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  period, period, &format.Format, nullptr);
    }
    if (hr == AUDCLNT_E_DEVICE_IN_USE) {
        return fail(Error::Code::BackendFailure,
                    "another application is using the output device exclusively");
    }
    if (FAILED(hr)) {
        return fail(Error::Code::BackendFailure, "exclusive Initialize failed");
    }
    UINT32 frames = 0;
    d.client->GetBufferSize(&frames);
    if (frames == 0 || frames > rt::kMaxBlockFrames) {
        return fail(Error::Code::UnsupportedFormat,
                    "device requires a block larger than rt::kMaxBlockFrames");
    }
    d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (d.event == nullptr || FAILED(d.client->SetEventHandle(d.event)) ||
        FAILED(d.client->GetService(__uuidof(IAudioRenderClient),
                                    reinterpret_cast<void**>(&d.render))) ||
        FAILED(d.client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&d.clock)))) {
        return fail(Error::Code::BackendFailure, "could not set up the render client");
    }
    UINT64 frequency = 0;
    d.clock->GetFrequency(&frequency);
    d.clockFrequency = frequency;
    d.bufferFrames = frames;

    REFERENCE_TIME latency = 0;
    d.client->GetStreamLatency(&latency);
    // Event-driven exclusive mode double-buffers: one buffer playing, one filling.
    m_outputLatencyMs =
        static_cast<double>(latency + durationOf(frames, config.sampleRate)) / 1.0e4;

    m_config = config;
    m_config.blockFrames = frames;
    m_callback = callback;
    m_user = user;
    m_scratch.assign(static_cast<std::size_t>(frames) * config.outputChannels, 0.0F);
    m_streamFrames = 0;
    m_callbackCount.store(0, std::memory_order_relaxed);
    m_xrunCount.store(0, std::memory_order_relaxed);
    m_open = true;
    return Error{};
}

Error WasapiExclusiveBackend::start() {
    if (!m_open) {
        return failure(Error::Code::NotOpen, "open() the stream first");
    }
    if (m_running) {
        return Error{};
    }
    Device& d = *m_device;
    // Exclusive event mode needs its first buffer before Start: silence.
    BYTE* data = nullptr;
    if (FAILED(d.render->GetBuffer(d.bufferFrames, &data)) ||
        FAILED(d.render->ReleaseBuffer(d.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT))) {
        return failure(Error::Code::BackendFailure, "could not prime the device buffer");
    }
    d.writtenFrames = d.bufferFrames;
    m_stop.store(false, std::memory_order_relaxed);
    d.thread = std::thread([this] { run(); });
    if (FAILED(d.client->Start())) {
        m_stop.store(true, std::memory_order_relaxed);
        SetEvent(d.event);
        d.thread.join();
        return failure(Error::Code::BackendFailure, "exclusive Start failed");
    }
    m_running = true;
    return Error{};
}

void WasapiExclusiveBackend::stop() noexcept {
    if (!m_running) {
        return;
    }
    Device& d = *m_device;
    m_stop.store(true, std::memory_order_relaxed);
    SetEvent(d.event);
    if (d.thread.joinable()) {
        d.thread.join();
    }
    d.client->Stop();
    d.client->Reset();
    m_running = false;
}

void WasapiExclusiveBackend::close() noexcept {
    stop();
    Device& d = *m_device;
    d.releaseAll();
    if (d.comOwned) {
        CoUninitialize();
        d.comOwned = false;
    }
    m_open = false;
    m_callback = nullptr;
    m_user = nullptr;
}

void WasapiExclusiveBackend::run() noexcept {
    Device& d = *m_device;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (task != nullptr) {
        AvSetMmThreadPriority(task, AVRT_PRIORITY_CRITICAL);
    }
    rt::registerAudioThread();
    // Two periods without the device asking for audio means it has stopped asking.
    const auto wait = static_cast<DWORD>(
        std::max<std::int64_t>(20, 2 * (1000LL * d.bufferFrames / m_config.sampleRate)));
    while (!m_stop.load(std::memory_order_relaxed)) {
        if (WaitForSingleObject(d.event, wait) != WAIT_OBJECT_0) {
            m_xrunCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (m_stop.load(std::memory_order_relaxed)) {
            break;
        }
        BYTE* data = nullptr;
        if (FAILED(d.render->GetBuffer(d.bufferFrames, &data))) {
            m_xrunCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        renderInto(data, d.bufferFrames);
        d.render->ReleaseBuffer(d.bufferFrames, 0);
    }
    rt::unregisterAudioThread();
    if (task != nullptr) {
        AvRevertMmThreadCharacteristics(task);
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}

void WasapiExclusiveBackend::renderInto(std::uint8_t* target, std::uint32_t frames) noexcept {
    Device& d = *m_device;
    const std::uint32_t channels = m_config.outputChannels;
    const StreamTime time{.streamFrames = m_streamFrames,
                          .streamSeconds = static_cast<double>(m_streamFrames) /
                                           static_cast<double>(m_config.sampleRate)};
    m_callback(m_user, m_scratch.data(), nullptr, frames, time);

    const float threshold = m_onsetThreshold.load(std::memory_order_relaxed);
    if (threshold > 0.0F && m_onsetFrame.load(std::memory_order_relaxed) < 0) {
        for (std::uint32_t f = 0; f < frames; ++f) {
            const auto* frame = &m_scratch[static_cast<std::size_t>(f) * channels];
            if (std::any_of(frame, frame + channels,
                            [threshold](float v) { return std::fabs(v) > threshold; })) {
                m_onsetFrame.store(static_cast<std::int64_t>(d.writtenFrames + f),
                                   std::memory_order_relaxed);
                break;
            }
        }
    }

    const std::size_t count = static_cast<std::size_t>(frames) * channels;
    const auto clamp = [](float v) { return std::clamp(v, -1.0F, 1.0F); };
    switch (d.format) {
    case SampleFormat::Float32:
        std::copy_n(m_scratch.data(), count, reinterpret_cast<float*>(target));
        break;
    case SampleFormat::Pcm32: {
        auto* out = reinterpret_cast<std::int32_t*>(target);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = static_cast<std::int32_t>(std::lrint(clamp(m_scratch[i]) * 8388607.0F)) << 8;
        }
        break;
    }
    case SampleFormat::Pcm24:
        for (std::size_t i = 0; i < count; ++i) {
            const auto v = static_cast<std::int32_t>(std::lrint(clamp(m_scratch[i]) * 8388607.0F));
            target[(3 * i) + 0] = static_cast<std::uint8_t>(v & 0xFF);
            target[(3 * i) + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
            target[(3 * i) + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        }
        break;
    case SampleFormat::Pcm16: {
        auto* out = reinterpret_cast<std::int16_t*>(target);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = static_cast<std::int16_t>(std::lrint(clamp(m_scratch[i]) * 32767.0F));
        }
        break;
    }
    }
    d.writtenFrames += frames;
    m_streamFrames += frames;
    m_callbackCount.fetch_add(1, std::memory_order_relaxed);
}

std::int64_t WasapiExclusiveBackend::onsetPlayTime() const noexcept {
    const std::int64_t frame = m_onsetFrame.load(std::memory_order_relaxed);
    const Device& d = *m_device;
    if (frame < 0 || d.clock == nullptr || d.clockFrequency == 0) {
        return 0;
    }
    UINT64 position = 0;
    UINT64 qpc = 0;
    if (FAILED(d.clock->GetPosition(&position, &qpc))) {
        return 0;
    }
    // The clock says which stream position plays at `qpc`; the onset is that many
    // seconds before or after it.
    const double played = static_cast<double>(position) / static_cast<double>(d.clockFrequency);
    const double onset = static_cast<double>(frame) / static_cast<double>(m_config.sampleRate);
    return static_cast<std::int64_t>(qpc) + std::llround((onset - played) * 1.0e7);
}

#else

struct WasapiExclusiveBackend::Device {
    SampleFormat format{SampleFormat::Float32};
};

WasapiExclusiveBackend::WasapiExclusiveBackend() : m_device(std::make_unique<Device>()) {}
WasapiExclusiveBackend::~WasapiExclusiveBackend() = default;
std::string_view WasapiExclusiveBackend::name() const noexcept {
    return "WASAPI exclusive";
}
std::span<const DeviceInfo> WasapiExclusiveBackend::enumerate() {
    return m_devices;
}
Error WasapiExclusiveBackend::open(const StreamConfig&, RenderCallback, void*) {
    return failure(Error::Code::NoDevice, "WASAPI exclusive mode is Windows-only");
}
Error WasapiExclusiveBackend::start() {
    return failure(Error::Code::NotOpen, "open() the stream first");
}
void WasapiExclusiveBackend::stop() noexcept {}
void WasapiExclusiveBackend::close() noexcept {}
void WasapiExclusiveBackend::run() noexcept {}
void WasapiExclusiveBackend::renderInto(std::uint8_t*, std::uint32_t) noexcept {}
std::int64_t WasapiExclusiveBackend::onsetPlayTime() const noexcept {
    return 0;
}

#endif

void WasapiExclusiveBackend::armOnset(float threshold) noexcept {
    m_onsetFrame.store(-1, std::memory_order_relaxed);
    m_onsetThreshold.store(threshold, std::memory_order_relaxed);
}

std::string_view WasapiExclusiveBackend::formatName() const noexcept {
    return formatText(m_device->format);
}

StreamInfo WasapiExclusiveBackend::info() const noexcept {
    StreamInfo out;
    out.isOpen = m_open;
    out.isRunning = m_running;
    out.sampleRate = m_config.sampleRate;
    out.blockFrames = m_config.blockFrames;
    out.outputChannels = m_config.outputChannels;
    out.inputChannels = 0;
    out.outputLatencyMs = m_outputLatencyMs;
    out.roundTripLatencyMs = m_outputLatencyMs;
    out.callbackCount = m_callbackCount.load(std::memory_order_relaxed);
    out.xrunCount = m_xrunCount.load(std::memory_order_relaxed);
    out.backendName.assign("WASAPI exclusive");
    return out;
}

} // namespace adx::audio

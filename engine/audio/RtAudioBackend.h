// Real devices, via RtAudio.
//
// RtAudio is forward-declared rather than included: its Windows header pulls in
// windows.h, and FINAL_PLAN's non-goals keep Win32 out of the engine so that
// macOS/Linux is later work rather than a rewrite. Confining it to one .cpp behind
// the AudioBackend interface is what makes that true in practice - and what makes
// the backend replaceable, which Phase 9 relies on when it adds ASIO as a device-API
// flag on this same class rather than a new backend.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "engine/audio/AudioBackend.h"

class RtAudio;

namespace adx::audio {

class RtAudioBackend final : public AudioBackend {
public:
    RtAudioBackend();
    ~RtAudioBackend() override;

    RtAudioBackend(const RtAudioBackend&) = delete;
    RtAudioBackend& operator=(const RtAudioBackend&) = delete;
    RtAudioBackend(RtAudioBackend&&) = delete;
    RtAudioBackend& operator=(RtAudioBackend&&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::span<const DeviceInfo> enumerate() override;
    Error open(const StreamConfig& config, RenderCallback callback, void* user) override;
    Error start() override;
    void stop() noexcept override;
    void close() noexcept override;
    [[nodiscard]] StreamInfo info() const noexcept override;

    /// True when RtAudio found no usable device. CI runners are the normal case -
    /// which is what NullBackend exists for.
    [[nodiscard]] bool hasDevices();

private:
    static int rtAudioTrampoline(void* outputBuffer, void* inputBuffer, unsigned int frames,
                                 double streamTime, unsigned int status, void* user);

    int handleCallback(void* outputBuffer, void* inputBuffer, unsigned int frames,
                       double streamTime, unsigned int status) noexcept;

    /// Re-reads the driver's latency figure. Called at open and again at start,
    /// because some APIs have nothing to say until the stream is running.
    void updateLatency() noexcept;

    std::unique_ptr<RtAudio> m_rtaudio;
    std::vector<DeviceInfo> m_devices;

    StreamConfig m_config{};
    RenderCallback m_callback{nullptr};
    void* m_user{nullptr};

    bool m_open{false};
    bool m_running{false};
    bool m_threadRegistered{false};
    std::uint64_t m_streamFrames{0};
    std::atomic<std::uint64_t> m_callbackCount{0};
    std::atomic<std::uint64_t> m_xrunCount{0};
    double m_outputLatencyMs{0.0};
    /// False when the figure above is the one-block floor rather than the driver's
    /// own number. Phase 6's device panel should say which it is showing.
    bool m_latencyIsMeasured{false};
};

} // namespace adx::audio

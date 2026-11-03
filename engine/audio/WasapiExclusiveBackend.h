// The default output in WASAPI exclusive mode: the low-latency path (STATE.md P9-9).
//
// Shared mode through RtAudio measured about 60 ms from a note to the Windows mix on
// the user's machine: the mixer's fixed 10 ms period, a 22 ms device buffer RtAudio
// keeps full, and RtAudio's own ring of the same size behind it. Exclusive mode skips
// the mixer and both buffers. The device runs at its minimum period (3 ms on that
// machine) double-buffered, and the engine renders straight into the device buffer.
//
// Output only and the default device only: recording stays on RtAudioBackend. Takes
// the device away from every other application while it runs, which is why it is
// asked for, never the default.
//
// Win32 lives in the .cpp; elsewhere open() reports the backend unsupported.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "engine/audio/AudioBackend.h"

namespace adx::audio {

class WasapiExclusiveBackend final : public AudioBackend {
public:
    WasapiExclusiveBackend();
    ~WasapiExclusiveBackend() override;

    WasapiExclusiveBackend(const WasapiExclusiveBackend&) = delete;
    WasapiExclusiveBackend& operator=(const WasapiExclusiveBackend&) = delete;
    WasapiExclusiveBackend(WasapiExclusiveBackend&&) = delete;
    WasapiExclusiveBackend& operator=(WasapiExclusiveBackend&&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::span<const DeviceInfo> enumerate() override;
    /// The block size becomes the device period nearest `config.blockFrames` that the
    /// driver allows (never below its minimum); info() reports what was chosen.
    Error open(const StreamConfig& config, RenderCallback callback, void* user) override;
    Error start() override;
    void stop() noexcept override;
    void close() noexcept override;
    [[nodiscard]] StreamInfo info() const noexcept override;

    /// Latency measurement (tools/measure_midi_latency.py): after arm(), the audio
    /// thread notes the first output frame louder than `threshold`.
    void armOnset(float threshold) noexcept;
    /// When that frame plays, by the device's clock, in QPC 100 ns units; 0 while
    /// nothing has been heard since arm().
    [[nodiscard]] std::int64_t onsetPlayTime() const noexcept;
    /// The sample format the device accepted, e.g. "pcm24".
    [[nodiscard]] std::string_view formatName() const noexcept;

private:
    struct Device;

    void run() noexcept;
    void renderInto(std::uint8_t* target, std::uint32_t frames) noexcept;

    std::unique_ptr<Device> m_device;
    std::vector<DeviceInfo> m_devices;
    std::vector<float> m_scratch;

    StreamConfig m_config{};
    RenderCallback m_callback{nullptr};
    void* m_user{nullptr};
    bool m_open{false};
    bool m_running{false};
    double m_outputLatencyMs{0.0};

    std::uint64_t m_streamFrames{0};
    std::atomic<std::uint64_t> m_callbackCount{0};
    std::atomic<std::uint64_t> m_xrunCount{0};
    std::atomic<bool> m_stop{false};

    std::atomic<float> m_onsetThreshold{0.0F}; // 0: not armed
    std::atomic<std::int64_t> m_onsetFrame{-1};
};

} // namespace adx::audio

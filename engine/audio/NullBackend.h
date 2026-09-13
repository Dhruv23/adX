// A stream with a clock but no device.
//
// This is the CI path, and it exists for a specific reason: GitHub's Windows runners
// have no audio hardware, so without it the realtime-safety gate - the thing this
// whole phase is for - could not run in CI and would rot within a month
// (phase_1.md §3.10).
//
// It is also the right way to run the engine on a developer machine while something
// else owns the sound card.
#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/audio/AudioBackend.h"

namespace adx::audio {

class NullBackend final : public AudioBackend {
public:
    NullBackend();
    ~NullBackend() override;

    NullBackend(const NullBackend&) = delete;
    NullBackend& operator=(const NullBackend&) = delete;
    NullBackend(NullBackend&&) = delete;
    NullBackend& operator=(NullBackend&&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::span<const DeviceInfo> enumerate() override;
    Error open(const StreamConfig& config, RenderCallback callback, void* user) override;
    Error start() override;
    void stop() noexcept override;
    void close() noexcept override;
    [[nodiscard]] StreamInfo info() const noexcept override;

private:
    void runLoop() noexcept;

    std::vector<DeviceInfo> m_devices;
    /// Interleaved output, sized once at open(). The callback writes here and nothing
    /// reads it; the point is that the callback sees a real, writable buffer of the
    /// right size.
    std::vector<float> m_output;
    std::vector<float> m_input;

    StreamConfig m_config{};
    RenderCallback m_callback{nullptr};
    void* m_user{nullptr};

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_open{false};
    std::atomic<std::uint64_t> m_callbackCount{0};
    std::atomic<std::uint64_t> m_lateCount{0};
};

} // namespace adx::audio

// A stream with no device and no clock: the callback is driven as fast as the CPU
// allows, for a fixed number of frames.
//
// This is the offline render driver, and having it in Phase 1 rather than Phase 8 is
// a deliberate structural choice (phase_1.md §3.10): offline render goes through the
// same AudioBackend interface and the same AudioThread entry point as realtime from
// the very beginning, so there is never a second code path to keep in sync. Offline
// output drifting away from what the user heard is the classic DAW export bug, and
// the archived engine avoided it by construction (FINAL_PLAN §3.1, ExportRenderer).
// This keeps that property from commit one.
#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "engine/audio/AudioBackend.h"

namespace adx::audio {

class OfflineBackend final : public AudioBackend {
public:
    OfflineBackend();
    ~OfflineBackend() override;

    OfflineBackend(const OfflineBackend&) = delete;
    OfflineBackend& operator=(const OfflineBackend&) = delete;
    OfflineBackend(OfflineBackend&&) = delete;
    OfflineBackend& operator=(OfflineBackend&&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::span<const DeviceInfo> enumerate() override;
    Error open(const StreamConfig& config, RenderCallback callback, void* user) override;

    /// A no-op that exists to satisfy the interface. Nothing renders until
    /// renderFrames() is called, which is what makes this backend deterministic.
    Error start() override;
    void stop() noexcept override;
    void close() noexcept override;
    [[nodiscard]] StreamInfo info() const noexcept override;

    /// Drives the callback on the *calling* thread until `frames` have been rendered,
    /// and returns the interleaved output.
    ///
    /// Synchronous, and on the caller's thread on purpose: an offline render with its
    /// own thread and its own scheduling would be a second timing model, which is
    /// exactly what this class exists to avoid.
    std::span<const float> renderFrames(std::uint64_t frames);

private:
    std::vector<DeviceInfo> m_devices;
    std::vector<float> m_block;
    std::vector<float> m_input;
    std::vector<float> m_rendered;

    StreamConfig m_config{};
    RenderCallback m_callback{nullptr};
    void* m_user{nullptr};

    bool m_open{false};
    std::uint64_t m_framesRendered{0};
    std::atomic<std::uint64_t> m_callbackCount{0};
};

} // namespace adx::audio

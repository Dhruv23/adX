// The device interface, and the two value types that travel across it.
//
// One interface, three implementations, and all three matter (phase_1.md §3.10):
// RtAudioBackend for real devices, NullBackend for a clock without a device - which
// is what CI runs, because GitHub's Windows runners have no audio hardware and
// without it the realtime gate would be untestable and would therefore rot - and
// OfflineBackend for no clock at all, which is the render driver Phase 3 and Phase 8
// build on. Offline render is the same code path from the beginning rather than a
// parallel one bolted on later, which is how offline and realtime drift apart.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "engine/audio/DeviceInfo.h"
#include "engine/audio/StreamConfig.h"
#include "engine/rt/FixedString.h"

namespace adx::audio {

/// Where the stream is. Passed to the render callback per buffer.
///
/// Phase 1 fills in only the frame counter. The transport that gives musical time
/// meaning is Phase 3's, and this is a *parameter* rather than a global the callback
/// reads for a specific reason: FINAL_PLAN §7's Phase 3 checkpoint requires time to
/// be a set of sources, because Performance Mode needs N independent playback
/// positions and a single global "current position" is exactly what makes clip
/// launching impossible to add later.
struct StreamTime {
    /// Frames elapsed since start(). Monotonic across callbacks.
    std::uint64_t streamFrames{};
    /// The backend's own timestamp for this buffer in seconds, where it has one.
    double streamSeconds{};
};

/// The latency readout FINAL_PLAN §5.6 asks for, plus the counter that says whether
/// the number is actually being met.
struct StreamInfo {
    bool isOpen{false};
    bool isRunning{false};
    std::uint32_t sampleRate{};
    std::uint32_t blockFrames{};
    std::uint32_t outputChannels{};
    std::uint32_t inputChannels{};
    double outputLatencyMs{};
    double inputLatencyMs{};
    double roundTripLatencyMs{};
    std::uint64_t callbackCount{};
    std::uint64_t xrunCount{};
    rt::FixedString<32> backendName;
};

/// Called on the audio thread, once per buffer.
///
/// A raw function pointer plus a void*, not std::function: std::function can
/// allocate, and calling one is an indirect branch through a type-erased vtable on
/// the hottest path in the program.
///
/// `out` is interleaved, `frames * outputChannels` floats. `in` is null when the
/// stream has no input.
using RenderCallback = void (*)(void* user, float* out, const float* in, std::uint32_t frames,
                                const StreamTime& time) noexcept;

/// Why an open or start failed. A code plus a fixed message, because this is
/// returned from code that must not allocate to build an error string.
struct Error {
    enum class Code : std::uint8_t {
        None,
        NoDevice,
        UnsupportedFormat,
        AlreadyOpen,
        NotOpen,
        BackendFailure,
        InvalidConfig,
    };

    Code code{Code::None};
    rt::FixedString<192> message;
};

class AudioBackend {
public:
    AudioBackend() = default;
    virtual ~AudioBackend() = default;

    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;
    AudioBackend(AudioBackend&&) = delete;
    AudioBackend& operator=(AudioBackend&&) = delete;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Devices this backend can see. Re-probed on each call; the span is owned by the
    /// backend and is valid until the next call.
    ///
    /// O(devices) and driven by user interaction, so returning a list across the FFI
    /// boundary is legal - FINAL_PLAN §2.2 Rule 2 prohibits O(notes) and O(frames),
    /// not O(1)-ish.
    [[nodiscard]] virtual std::span<const DeviceInfo> enumerate() = 0;

    /// Main thread. Allocates freely - this is the one place in the audio path that
    /// may.
    virtual Error open(const StreamConfig& config, RenderCallback callback, void* user) = 0;

    virtual Error start() = 0;
    virtual void stop() noexcept = 0;
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual StreamInfo info() const noexcept = 0;
};

} // namespace adx::audio

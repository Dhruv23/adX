// adx-thread: realtime
//
// The audio thread's end of one sandboxed instance: one block out, one block back,
// against a deadline (phase_9.md §4.3). Nothing here allocates, locks or waits without
// a bound; the only system calls are a SetEvent and a timed wait.
#pragma once

#include <cstdint>
#include <span>

#include "engine/plugin/Bridge/SharedAudio.h"
#include "engine/plugin/Bridge/Watchdog.h"

namespace adx::plugin::bridge {

/// Signals an event handle (Platform's Event::handle()).
void signalHandle(void* event) noexcept;
/// Waits on `event` for at most `timeoutMicros`, with `timer` (a high-resolution
/// waitable timer, may be null) for sub-millisecond precision.
[[nodiscard]] bool waitHandle(void* event, void* timer, std::uint32_t timeoutMicros) noexcept;

/// Plain pointers into objects HostProcess owns and keeps alive for the link's life.
struct AudioLink {
    SharedAudio* shared{nullptr};
    void* go{nullptr};
    void* done{nullptr};
    void* timer{nullptr};
    LinkHealth* health{nullptr};
};

struct BlockRequest {
    std::span<const float* const> inputs;
    std::uint32_t frames{0};
    std::span<const WireEvent> events;
    std::span<const WireParam> params;
    WireTransport transport{};
    std::uint32_t deadlineMicros{0};
};

enum class BlockResult : std::uint8_t {
    /// Output is the plugin's.
    Done,
    /// Not answered by the deadline, or the child is still on an earlier block:
    /// output is silence.
    Late,
    /// The link is not running (state says why): output untouched.
    NotRunning,
};

/// Sends one block and waits for it. On Done, `outputs` hold the plugin's audio and
/// `outParams` its own parameter changes (the returned count).
///
/// One sender at a time per link: the audio thread, or - before the link is in a
/// published graph, or while nothing renders it - the main thread (measureLatency).
[[nodiscard]] BlockResult runBlock(const AudioLink& link, const BlockRequest& request,
                                   std::span<float* const> outputs, std::span<WireParam> outParams,
                                   std::uint32_t& outParamCount) noexcept;

} // namespace adx::plugin::bridge

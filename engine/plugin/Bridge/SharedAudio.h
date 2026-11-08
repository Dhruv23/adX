// adx-thread: realtime
//
// One plugin instance's shared-memory region (phase_9.md §4.3): the block the host
// hands the sandbox child, and the block it gets back.
//
//   host   writes input audio, events and parameter changes, then release-stores
//          `request` (the block's sequence number) and signals the child's event;
//   child  acquire-loads `request`, processes, writes output, release-stores
//          `reply = request` and signals back;
//   host   waits for `reply == request` until its deadline, and on a miss plays
//          silence for the block and counts it late.
//
// The region is exchanged a whole block at a time, never concurrently, so plain arrays
// guarded by the sequence pair do what §4.3 sketched as rings: a host that finds the
// child still on the previous block does not write at all (it plays that block late),
// so neither side ever reads what the other is writing.
//
// Everything here is fixed-size and trivially copyable: the region is the same bytes
// in two processes.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace adx::plugin::bridge {

inline constexpr std::uint32_t kRegionMagic = 0x41445850; // "ADXP"
inline constexpr std::uint32_t kRegionVersion = 1;

/// Main bus only: stereo in, stereo out. A mono plugin is fed left and gives left.
inline constexpr std::uint32_t kMaxChannels = 2;
inline constexpr std::uint32_t kMaxBlockFrames = 4096;
inline constexpr std::uint32_t kMaxEvents = 1024;
inline constexpr std::uint32_t kMaxParamChanges = 1024;
inline constexpr std::uint32_t kMaxOutParams = 256;

enum class WireEventKind : std::uint8_t {
    NoteOn,
    NoteOff,
    /// Release everything sounding (a stop, a seek).
    AllNotesOff,
    /// Any other channel message, raw: status in `channel`'s place, data1, data2.
    Midi,
    /// The note's tuning, `value` semitones from its key: a slide, a pitch curve, the
    /// channel's pitch offset (STATE.md P9-7). CLAP note expression TUNING; VST3's
    /// tuning note expression.
    Tuning,
};

struct WireEvent {
    std::uint32_t offset;
    std::int32_t noteId;
    WireEventKind kind;
    std::uint8_t channel;
    std::uint8_t key;
    std::uint8_t velocity;
    /// Midi: the status byte; data1 and data2 are key and velocity.
    std::uint8_t status;
    std::array<std::uint8_t, 3> pad;
    /// Tuning: semitones.
    float value;
};

struct WireParam {
    std::uint32_t offset;
    std::uint32_t id;
    /// In the plugin's own units (PluginDescriptor.h).
    double value;
    /// kParamGesture on a change the plugin made inside an edit gesture - its editor,
    /// a user's hand - rather than one it reported by itself (P9-7, P10.5-2).
    std::uint32_t flags;
    std::uint32_t reserved;
};

inline constexpr std::uint32_t kParamGesture = 1U << 0U;

enum TransportFlags : std::uint32_t {
    kPlaying = 1U << 0U,
    kLooping = 1U << 1U,
};

struct WireTransport {
    double tempo;
    double beats;
    std::int64_t samples;
    std::uint32_t flags;
    std::uint16_t numerator;
    std::uint16_t denominator;
};

/// What the child says about the block it just finished, as bits of `status`.
enum StatusFlags : std::uint32_t {
    /// The plugin reported a different latency; `latency` has the new one.
    kLatencyChanged = 1U << 0U,
    /// The plugin asked to be restarted (CLAP request_restart, VST3 kReloadComponent).
    kRestartRequested = 1U << 1U,
    /// The plugin's process failed and the child output silence.
    kProcessFailed = 1U << 2U,
};

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4324) // padded for alignas: intended
#endif

struct alignas(64) ControlBlock {
    std::uint32_t magic;
    std::uint32_t version;
    // Host -> child, published by `request`.
    std::atomic<std::uint32_t> request;
    std::uint32_t frames;
    std::uint32_t inputChannels;
    std::uint32_t outputChannels;
    std::uint32_t eventCount;
    std::uint32_t paramCount;
    WireTransport transport;
    // Child -> host, published by `reply`.
    alignas(64) std::atomic<std::uint32_t> reply;
    std::uint32_t outParamCount;
    std::atomic<std::uint32_t> status;
    std::atomic<std::uint32_t> latency;
    /// Blocks the child has finished: the watchdog's sign of life.
    std::atomic<std::uint64_t> processed;
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

struct SharedAudio {
    ControlBlock control;
    std::array<std::array<float, kMaxBlockFrames>, kMaxChannels> input;
    std::array<std::array<float, kMaxBlockFrames>, kMaxChannels> output;
    std::array<WireEvent, kMaxEvents> events;
    std::array<WireParam, kMaxParamChanges> params;
    std::array<WireParam, kMaxOutParams> outParams;
};

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

} // namespace adx::plugin::bridge

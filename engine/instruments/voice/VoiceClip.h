// What the audio thread sees of a rendered note: a published, immutable buffer.
//
// The render cache (VoiceRenderCache.h, main and worker threads) owns the samples;
// this is the realtime view of them. `ready` is set, with release ordering, after
// `samples`, `frames` and `lead` are written and never changes back; the audio thread
// reads it with acquire ordering and touches nothing else until it is true. That is
// how a note "plays silence plus a rendering... marker until ready" without the
// callback ever waiting (phase_4.md §4.13).
#pragma once

#include <atomic>
#include <cstdint>

namespace adx::instruments {

struct VoiceClip {
    std::atomic<bool> ready{false};
    const float* samples{nullptr};
    std::uint32_t frames{0};
    /// Frames before the note's start: the preutterance, when the consonant begins.
    std::uint32_t lead{0};

    [[nodiscard]] bool isReady() const noexcept {
        return ready.load(std::memory_order_acquire);
    }
};

/// One note's render, as a Voice node finds it: by note id.
struct VoiceNoteRef {
    std::uint32_t noteId{0};
    const VoiceClip* clip{nullptr};
};

} // namespace adx::instruments

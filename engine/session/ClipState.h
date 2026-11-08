// adx-thread: realtime
//
// The per-clip state machine (phase_11.md §4.2). Five states:
//
//   Stopped     nothing playing, nothing pending
//   Queued      launched; starts at the next quantize boundary
//   Playing     running on its own time source
//   QueuedStop  playing; stops at the next quantize boundary
//   Retriggered playing and launched again; restarts at the boundary (or, with legato,
//               carries on in phase) - kept apart from Queued so a relaunch never
//               reads as a cold start, which is the "retrigger kills my legato
//               bassline" bug
//
// Pure and table-driven: the session engine feeds it triggers and acts on what it says.
#pragma once

#include <cstdint>

namespace adx::session {

enum class ClipState : std::uint8_t { Stopped, Queued, Playing, QueuedStop, Retriggered };

enum class ClipTrigger : std::uint8_t {
    /// A launch request (a click, a pad, a scene).
    Launch,
    /// A stop request.
    Stop,
    /// The quantize boundary a pending request was waiting for.
    Boundary,
    /// A clip that does not loop reached its end.
    End,
};

/// What a transition asks the engine to do, besides moving to `next`.
struct ClipStep {
    ClipState next{ClipState::Stopped};
    /// Start the clip from its beginning now.
    bool start{false};
    /// Stop it now (a release, never a cut).
    bool stop{false};
    /// Run the clip's follow action (a non-looping clip ran out).
    bool follow{false};
};

[[nodiscard]] ClipStep step(ClipState from, ClipTrigger trigger, bool legato) noexcept;

/// Whether a request is waiting for a boundary in `state`.
[[nodiscard]] constexpr bool pending(ClipState state) noexcept {
    return state == ClipState::Queued || state == ClipState::QueuedStop ||
           state == ClipState::Retriggered;
}

/// Whether the clip is sounding (its source rolls) in `state`.
[[nodiscard]] constexpr bool sounding(ClipState state) noexcept {
    return state == ClipState::Playing || state == ClipState::QueuedStop ||
           state == ClipState::Retriggered;
}

[[nodiscard]] const char* toString(ClipState state) noexcept;

} // namespace adx::session

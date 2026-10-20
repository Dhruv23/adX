// Controllers: automation *sources* rather than drawn curves (FINAL_PLAN §5.1's envelope
// controller, LFO tool and peak controller; phase_6.md §4.5).
//
// A controller drives one parameter. Two of the three are pure functions of the
// arrangement and compile, when a snapshot is built, into the same per-frame automation
// lanes a drawn curve does - an LFO is a function of position, an envelope a function
// of where its source channel's notes start and end - so they play identically offline
// and in realtime, at any block size, and cost the audio thread nothing new. The third,
// the peak controller, follows a signal, and is a node in the graph: it reads an
// insert's output through a sidechain-shaped edge and writes its target once per block.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/ParamRef.h"

namespace adx::project {

enum class ControllerKind : std::uint8_t { Lfo, Envelope, Peak };
inline constexpr std::size_t kControllerKindCount = 3;

enum class LfoShape : std::uint8_t { Sine, Triangle, Saw, Square };
inline constexpr std::size_t kLfoShapeCount = 4;

[[nodiscard]] const char* toString(ControllerKind kind) noexcept;
[[nodiscard]] bool controllerKindFromString(std::string_view name, ControllerKind& out) noexcept;
[[nodiscard]] const char* toString(LfoShape shape) noexcept;
[[nodiscard]] bool lfoShapeFromString(std::string_view name, LfoShape& out) noexcept;

/// One quarter note: the LFO's default period.
inline constexpr core::Ticks kDefaultLfoPeriod{core::kPpq};

struct Controller {
    core::ControllerId id;
    /// Unique within the project, like a channel's: the file names a controller by it.
    std::string name;
    ControllerKind kind{ControllerKind::Lfo};

    /// The parameter driven, as written and as resolved - kept both ways for the same
    /// reason AutomationClip keeps both: a target that no longer resolves is preserved.
    std::string targetPath;
    ParamRef target;

    /// The output, in the target's normalised range (0..1 of its descriptor's span,
    /// logarithmic where the parameter is): `center + depth * shape`, clamped. For an
    /// LFO the shape swings -1..1; for an envelope or a peak follower it rises 0..1.
    float depth{0.5F};
    float center{0.5F};

    // --- LFO ---
    LfoShape shape{LfoShape::Sine};
    /// One cycle, in ticks: tempo-synced by construction.
    core::Ticks period{kDefaultLfoPeriod};
    /// Where in its cycle the LFO is at tick 0, 0..1.
    float phase{0.0F};

    // --- envelope: retriggered by every note of `sourceChannel` ---
    core::ChannelId sourceChannel;
    float attackMs{5.0F};
    float decayMs{200.0F};
    float sustain{0.0F};
    float releaseMs{100.0F};

    // --- peak: follows `sourceInsert`'s post-fader level ---
    core::InsertId sourceInsert;
    /// Applied to the level before it is shaped, so a quiet source can still swing the
    /// whole range.
    float gainDb{0.0F};
    // attackMs and releaseMs are the follower's, shared with the envelope.

    [[nodiscard]] friend bool operator==(const Controller&, const Controller&) noexcept = default;
};

} // namespace adx::project

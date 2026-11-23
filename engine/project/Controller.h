// Controllers: automation *sources* rather than drawn curves (FINAL_PLAN §5.1's envelope
// controller, LFO tool and peak controller; phase_6.md §4.5).
//
// A controller drives one parameter - or a macro, and through it every parameter the
// macro moves. Most kinds are pure functions of the arrangement and compile, when a
// snapshot is built, into the same per-frame automation lanes a drawn curve does - an LFO
// or a formula is a function of position, an envelope or a keyboard controller a function
// of its source channel's notes - so they play identically offline and in realtime, at
// any block size, and cost the audio thread nothing new. The peak controller follows a
// signal, and is a node in the graph: it reads an insert's output through a
// sidechain-shaped edge and writes its target once per block. The X-Y pad is a position,
// two values a lane, a knob or MIDI learn can move (`controller.<name>.x` and `.y`), each
// driving its own target.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/ParamRef.h"

namespace adx::project {

enum class ControllerKind : std::uint8_t { Lfo, Envelope, Peak, Formula, Keyboard, Xy };
inline constexpr std::size_t kControllerKindCount = 6;

enum class LfoShape : std::uint8_t {
    Sine,
    Triangle,
    Saw,
    Square,
    /// A falling ramp: 1 to -1 over the cycle.
    SawDown,
    /// Sample and hold: one random level per cycle, held.
    Random,
    /// One random level per cycle, glided between in straight lines.
    SmoothRandom,
};
inline constexpr std::size_t kLfoShapeCount = 7;

/// What a keyboard controller reads from each note.
enum class KeyboardInput : std::uint8_t { Pitch, Velocity };
inline constexpr std::size_t kKeyboardInputCount = 2;

[[nodiscard]] const char* toString(ControllerKind kind) noexcept;
[[nodiscard]] bool controllerKindFromString(std::string_view name, ControllerKind& out) noexcept;
[[nodiscard]] const char* toString(LfoShape shape) noexcept;
[[nodiscard]] bool lfoShapeFromString(std::string_view name, LfoShape& out) noexcept;
[[nodiscard]] const char* toString(KeyboardInput input) noexcept;
[[nodiscard]] bool keyboardInputFromString(std::string_view name, KeyboardInput& out) noexcept;

/// One quarter note: the LFO's default period.
inline constexpr core::Ticks kDefaultLfoPeriod{core::kPpq};

/// The X-Y pad's two axes, as a ParamRef's `index` under ParamKind::ControllerAxis.
inline constexpr std::uint16_t kAxisX = 0;
inline constexpr std::uint16_t kAxisY = 1;

struct Controller {
    core::ControllerId id;
    /// Unique within the project, like a channel's: the file names a controller by it.
    std::string name;
    ControllerKind kind{ControllerKind::Lfo};

    /// The parameter driven, as written and as resolved - kept both ways for the same
    /// reason AutomationClip keeps both: a target that no longer resolves is preserved.
    /// A macro is a valid target: the controller then moves every one of its targets.
    /// For an X-Y pad this is the X axis's target.
    std::string targetPath;
    ParamRef target;

    /// The output, in the target's normalised range (0..1 of its descriptor's span,
    /// logarithmic where the parameter is): `center + depth * shape`, clamped. For an
    /// LFO the shape swings -1..1; for an envelope or a peak follower it rises 0..1; a
    /// formula's is whatever it computes; a keyboard controller's is 0..1 over its key
    /// or velocity range. An X-Y pad ignores both: its position is the output.
    float depth{0.5F};
    float center{0.5F};

    // --- LFO ---
    LfoShape shape{LfoShape::Sine};
    /// One cycle, in ticks: tempo-synced by construction. A formula's `p` counts it too.
    core::Ticks period{kDefaultLfoPeriod};
    /// Where in its cycle the LFO is at tick 0, 0..1.
    float phase{0.0F};
    /// Picks the random shapes' sequence: the same seed, the same levels.
    std::uint32_t seed{1};

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

    // --- formula: an expression of position (Formula.h) ---
    std::string formula;
    /// Three free inputs the expression reads as `a`, `b` and `c`.
    float a{0.0F};
    float b{0.0F};
    float c{0.0F};

    // --- keyboard: `sourceChannel`'s notes, mapped onto the target ---
    KeyboardInput input{KeyboardInput::Pitch};
    /// The keys (or velocities) that map to 0 and to 1; outside them it clamps.
    std::uint8_t low{36};
    std::uint8_t high{84};
    /// How long the output takes to reach a new note's value. 0 jumps.
    float glideMs{0.0F};

    // --- X-Y pad: two values, each driving its own target ---
    float x{0.5F};
    float y{0.5F};
    std::string targetYPath;
    ParamRef targetY;

    [[nodiscard]] friend bool operator==(const Controller&, const Controller&) noexcept = default;
};

/// Whether `kind` becomes an automation lane when a snapshot is built (everything but
/// the peak follower, a graph node, and the X-Y pad, a position folded into its targets).
[[nodiscard]] constexpr bool compilesToLane(ControllerKind kind) noexcept {
    return kind != ControllerKind::Peak && kind != ControllerKind::Xy;
}

} // namespace adx::project

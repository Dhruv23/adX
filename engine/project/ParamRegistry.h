// Parameter paths in, 8-byte integers out.
//
// Iteration one carried automation targets as two strings and compared them per
// block (FINAL_PLAN §3.2). This is the fix, and the fix has two halves that have to
// stay together: resolution happens once, at load, and serialisation goes back
// through pathOf(), so the text stays human-readable and stable while the runtime
// stays integer. Renaming a channel therefore rewrites every path that referred to
// it, because the writer is the only thing that ever emits one.
//
// The Phase 3 guarantee this exists to make possible: the render path never sees a
// string, so no string comparison in it is *possible*, not merely absent.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "engine/project/ParamRef.h"

namespace adx::project {

class Project;

enum class Unit : std::uint8_t {
    Normalized,
    Hertz,
    Decibels,
    Milliseconds,
    Seconds,
    Ratio,
    Semitones,
    Cents,
    Percent,
    Count,
    Boolean,
};

enum class ScaleKind : std::uint8_t { Linear, Logarithmic, Stepped };

struct ParamDescriptor {
    std::string_view name;
    float minimum{0.0F};
    float maximum{1.0F};
    float defaultValue{0.0F};
    Unit unit{Unit::Normalized};
    ScaleKind scale{ScaleKind::Linear};
};

/// Why a path did not resolve, and which characters to underline.
///
/// A reason plus a span rather than a ready-made Diagnostic: the registry knows
/// which *segment* of the path is wrong but not which line and column the path
/// started at. The caller knows both, so it builds the diagnostic and the
/// underline lands on `filter.cutof` rather than on the whole line - which is the
/// half iteration one could not do at all.
enum class ResolveError : std::uint8_t {
    None,
    MalformedPath,
    UnknownChannel,
    UnknownInsert,
    UnknownSlot,
    UnknownSend,
    UnknownParameter,
};

struct ParamResolution {
    ParamRef ref;
    ResolveError error{ResolveError::None};
    /// Offset and length, in characters, of the offending segment within the path.
    std::uint32_t segmentOffset{0};
    std::uint32_t segmentLength{0};

    [[nodiscard]] bool ok() const noexcept {
        return error == ResolveError::None;
    }
};

/// Stateless in Phase 2 - the descriptor table is built in, because the only
/// parameters that exist so far are the ones this phase defines. Phase 4 gives it
/// state when instruments start registering their own descriptors, which is why it
/// is a class rather than three free functions.
class ParamRegistry {
public:
    /// "channel.Lead.filter.cutoff", "insert.3.slot.1.mix", "master.gain".
    [[nodiscard]] static ParamResolution resolve(std::string_view path, const Project& project);

    /// The inverse. Returns an empty string for a ref that no longer resolves -
    /// which happens when the entity it named has been deleted, and which the
    /// writer treats as "do not emit this lane".
    [[nodiscard]] static std::string pathOf(ParamRef ref, const Project& project);

    [[nodiscard]] static const ParamDescriptor& describe(ParamKind kind) noexcept;

    /// The descriptor for a named instrument or effect parameter, or nullptr when
    /// the name is not one this build knows. Unknown is not an error - it is an
    /// ADX1004 warning and the value is kept, because a newer adX's parameter must
    /// survive an older adX's save (FINAL_PLAN §6 rule 4).
    [[nodiscard]] static const ParamDescriptor* describeNamed(std::string_view name) noexcept;

    /// Quotes a name for use as a path segment, if it needs quoting.
    [[nodiscard]] static std::string quoteSegment(std::string_view name);
};

} // namespace adx::project

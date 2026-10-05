// A channel: one instrument instance, its settings, and where it goes.
//
// Iteration one's `Track` was simultaneously an instrument, a pattern, a playlist
// lane and a mixer strip (FINAL_PLAN §3.3.7). Every DAW feature that could not be
// built traces back to that one conflation. A Channel is only the first of those
// four things; the other three are Pattern, PlaylistTrack and Insert.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/core/Ids.h"
#include "engine/core/Rational.h"
#include "engine/project/Color.h"
#include "engine/project/SampleZone.h"
#include "engine/project/VoiceStealMode.h"

namespace adx::project {

/// One named instrument parameter.
///
/// A name and a number, not a typed field: the instruments that give these meaning
/// are Phase 4's, and inventing their parameter set here would mean inventing it
/// twice. What Phase 2 owns is that the value is *named* rather than positional -
/// `RESFILTER=0,1200,0.7,0.5,0.3` is unreadable in a diff and is the reason v1's
/// format could not grow without breaking.
struct ParamValue {
    std::string name;
    double value{0.0};

    /// Present only when the file said `curve=`. Carrying it here is what closes
    /// FINAL_PLAN §3.3.12: iteration one's Bezier envelope handles had nowhere to
    /// live on disk, so every save silently flattened them.
    bool hasCurve{false};
    core::Curve curve;

    [[nodiscard]] friend bool operator==(const ParamValue&, const ParamValue&) noexcept = default;
};

struct InstrumentSpec {
    /// A TypeCatalog instrument name; anything else round-trips untouched and plays
    /// as silence.
    std::string type{"additive"};
    std::vector<ParamValue> params;
    /// The key/velocity map of an instrument that plays pool samples - sampler,
    /// slicer, pool, granular - or a wavetable's file (phase_4.md §4.5, §4.6, §4.8).
    std::vector<SampleZone> zones;
    /// The Voice instrument's UTAU voicebank (phase_4.md §4.13): a folder, relative to
    /// the project like a sample. Only a path - a project never carries a bank, whose
    /// licence usually forbids redistributing it. Empty for every other type.
    std::string voicebank;

    [[nodiscard]] const ParamValue* find(std::string_view name) const noexcept;
    [[nodiscard]] ParamValue* find(std::string_view name) noexcept;

    [[nodiscard]] friend bool operator==(const InstrumentSpec&,
                                         const InstrumentSpec&) noexcept = default;
};

enum class ArpMode : std::uint8_t { Off, Up, Down, UpDown, DownUp, Random, Order };
inline constexpr std::size_t kArpModeCount = 7;

[[nodiscard]] const char* toString(ArpMode mode) noexcept;
[[nodiscard]] bool arpModeFromString(std::string_view name, ArpMode& out) noexcept;

struct ArpSettings {
    ArpMode mode{ArpMode::Off};
    /// As a fraction of a whole note: 1/16 is a sixteenth. Exact, so "is this on the
    /// grid?" is never a tolerance question.
    core::Rational rate{1, 16};
    std::uint16_t octaves{1};
    float gate{0.8F};

    [[nodiscard]] friend bool operator==(const ArpSettings&, const ArpSettings&) noexcept = default;
};

[[nodiscard]] const char* toString(VoiceStealMode mode) noexcept;
[[nodiscard]] bool voiceStealModeFromString(std::string_view name, VoiceStealMode& out) noexcept;

struct Channel {
    core::ChannelId id;
    /// Unique within the project, because automation paths address channels by name
    /// (`channel.Lead.filter.cutoff`) and a diff reader needs to see a name rather
    /// than an index. Validate enforces both the uniqueness and the character set.
    std::string name;
    Color color;

    InstrumentSpec instrument;

    /// Which mixer insert this channel feeds. Many channels may feed one insert -
    /// that is how a drum bus works, and it is impossible when the strip and the
    /// instrument are the same object.
    core::InsertId output;

    /// Per channel, not global. Iteration one had one 64-voice pool for the whole
    /// project, so a sustained pad stole the kick (FINAL_PLAN §3.3.6).
    std::uint16_t maxPolyphony{16};
    VoiceStealMode stealMode{VoiceStealMode::OldestReleased};

    ArpSettings arp;

    bool muted{false};
    bool soloed{false};
    float volume{1.0F};
    float pan{0.0F};
    float pitchOffsetCents{0.0F};

    [[nodiscard]] friend bool operator==(const Channel&, const Channel&) noexcept = default;
};

} // namespace adx::project

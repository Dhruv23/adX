// adx-thread: main
//
// A preset: one instrument's or one effect's settings, as named parameters, plus a
// sampler's zones (phase_4.md §4.12).
//
//   [PRESET]
//   NAME="Warm Pad"
//   TYPE=additive
//   PACK=c418
//   TAGS=pad, ambient, slow
//   MIX=0.35                     (an effect preset's wet/dry)
//
//   [PARAMS]
//   env.attack=1.2 curve=exponential(0.5)
//   filter.cutoff=2500
//
//   [ZONES]
//   ZONE "kit/kick.wav" key=36 root=36
//
// Same lexer, same value grammar, same diagnostics as `.adx` (Phase 2): a preset is a
// parameter block, so presets get round-tripping, unknown-key tolerance and
// line/column diagnostics without a second parser. Parameters are checked against the
// type's descriptor table (TypeCatalog): an unknown name is ADX1004, a value out of
// range ADX2001, an unknown TYPE ADX1001 - warnings, and the preset still loads.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/format/adx/Diagnostics.h"
#include "engine/project/Channel.h"
#include "engine/project/SampleZone.h"
#include "engine/project/TypeCatalog.h"

namespace adx::preset {

/// A zone and the file it names, relative to the preset file.
struct PresetZone {
    project::SampleZone zone;
    std::string path;

    [[nodiscard]] friend bool operator==(const PresetZone&, const PresetZone&) noexcept = default;
};

struct Preset {
    std::string name;
    /// An instrument type (`additive`) or an effect type (`Reverb`), as the catalog
    /// spells it.
    std::string type;
    std::string pack;
    std::vector<std::string> tags;
    /// An effect preset's wet/dry: the slot's `mix`, which is a field of the slot and
    /// not one of the effect's parameters. Unset for instruments.
    std::optional<double> mix;
    std::vector<project::ParamValue> params;
    std::vector<PresetZone> zones;
    /// Where it was read from. Not serialised.
    std::filesystem::path source;

    /// Instrument or effect, from the type; Instrument for an unknown type.
    [[nodiscard]] project::TypeKind kind() const noexcept;
    [[nodiscard]] bool hasTag(std::string_view tag) const noexcept;

    /// Equal content; `source` excluded.
    [[nodiscard]] friend bool operator==(const Preset& a, const Preset& b) noexcept {
        return a.name == b.name && a.type == b.type && a.pack == b.pack && a.tags == b.tags &&
               a.mix == b.mix && a.params == b.params && a.zones == b.zones;
    }
};

/// Parses preset text. Never throws: what cannot be understood is a diagnostic.
[[nodiscard]] Preset parsePreset(std::string_view text, format::DiagnosticList& diagnostics);

/// Canonical text. parsePreset(writePreset(p)) == p, and writePreset is idempotent.
[[nodiscard]] std::string writePreset(const Preset& preset);

/// Reads and parses a file; `source` is set. False when the file cannot be read.
[[nodiscard]] bool loadPreset(const std::filesystem::path& path, Preset& out,
                              format::DiagnosticList& diagnostics);

/// Writes `preset` to `path`, creating its directory. False on failure.
[[nodiscard]] bool savePreset(const Preset& preset, const std::filesystem::path& path);

/// The preset's instrument settings, ready to put on a channel. Zones' samples are
/// left to the caller, who owns the project's sample pool (Resources): it gets each
/// zone's path, resolved against the preset's directory.
[[nodiscard]] project::InstrumentSpec instrumentOf(const Preset& preset);

} // namespace adx::preset

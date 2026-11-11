// adx-thread: main
//
// The `.adxrack` file (phase_12.md §4.1): a rack as text.
//
//   [RACK]
//   NAME="Euphoric Kick"
//   PACK=hardcore
//   CATEGORY=Drums/Kick
//   TAGS=kick, hardstyle
//   TEMPO_HINT=150
//   ROOT_HINT=F1
//
//   [LAYER punch]
//   TYPE=drumsynth                  (or PRESET=pack/name)
//   KEYS=0..127
//   GAIN=-4
//   PARAM pitchDrop=30
//   SLOT ParametricEq band1.type=4 band1.freq=60     (the layer's own chain)
//
//   [CHAIN]
//   SLOT Clipper mode=1 drive=12
//
//   [SEND reverb]
//   LEVEL=-18
//   SLOT Reverb room=0.6
//   SLOT Ducker amount=0.8 key=rack
//
//   [MACRO Tail Length]
//   VALUE=0.5
//   TARGET layer.tail.env.decay range=0.15..0.60 curve=exponential(0.3)
//
// The lines are the ones `.adx` already has - PARAM and ZONE as a channel writes them,
// SLOT as the mixer does, TARGET as a [MACRO] does - read by the same lexer with the
// same diagnostics. The model is project::Rack (engine/project/Rack.h), which is also
// what InsertRack expands.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "engine/format/adx/Diagnostics.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/project/Rack.h"

namespace adx::preset {

inline constexpr std::string_view kRackExtension = ".adxrack";

/// Parses rack text. Never throws: what cannot be understood is a diagnostic, and an
/// unknown key is kept and written back.
[[nodiscard]] project::Rack parseRack(std::string_view text, format::DiagnosticList& diagnostics);

/// Canonical text. parseRack(writeRack(r)) == r, and writeRack is idempotent.
[[nodiscard]] std::string writeRack(const project::Rack& rack);

[[nodiscard]] bool loadRack(const std::filesystem::path& path, project::Rack& out,
                            format::DiagnosticList& diagnostics);
[[nodiscard]] bool saveRack(const project::Rack& rack, const std::filesystem::path& path);

/// Gives every `PRESET=pack/name` layer that preset's type, parameters and zones, with
/// the layer's own PARAM lines applied on top, and clears the reference. A preset that
/// is not in `library` is a diagnostic and the layer keeps its own TYPE.
void resolveRackPresets(project::Rack& rack, const PresetLibrary& library,
                        format::DiagnosticList& diagnostics);

/// Makes every relative sample and impulse path absolute against `directory` (the rack
/// file's own). `lib://` paths and absolute paths are left alone.
void anchorRackPaths(project::Rack& rack, const std::filesystem::path& directory);

} // namespace adx::preset

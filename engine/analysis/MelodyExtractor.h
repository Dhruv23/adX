// adx-thread: main
//
// Audio to notes through Spotify's basic-pitch model (phase_10.md §4.2), ported from
// _archive/src-cpp/src/MelodyExtractor.cpp. Compiled only with ADX_ENABLE_ONNX, which is
// off by default (FINAL_PLAN §8.2): with it off this header declares nothing, the app
// hides the entry point, and melodyExtractorAvailable() says so.
//
//   input     mono at 22050 Hz, in 2 s windows (43844 samples) overlapping by 30 frames
//   model     note, onset and contour activations per 11.6 ms frame and semitone;
//             contour (pitch bend) is discarded
//   output    NoteCreation.h's notes: the half of the port that needs no model
//
// What changed on port: the model path is passed in (the build's copy by default, the
// installer's optional component later) rather than found beside the executable, audio
// arrives as samples, and a cancel callback is polled between windows so an
// AnalysisJob can stop it. And v1's window stitching drifted (§11): it now pads the
// start as basic-pitch does and reads each frame from the window that covers it.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "engine/analysis/NoteCreation.h"

namespace adx::analysis {

inline constexpr std::uint32_t kMelodySampleRate = 22050;

/// True when this build has the extractor. The app shows the entry point only then.
[[nodiscard]] constexpr bool melodyExtractorAvailable() noexcept {
#if defined(ADX_ENABLE_ONNX)
    return true;
#else
    return false;
#endif
}

#if defined(ADX_ENABLE_ONNX)

/// The model the build fetched.
[[nodiscard]] std::filesystem::path defaultMelodyModel();

/// Called between windows with progress in 0..1; return false to cancel.
using MelodyProgress = std::function<bool(double)>;

/// Nullopt when the model cannot be loaded, inference fails, or `progress` cancels.
[[nodiscard]] std::optional<std::vector<DetectedNote>>
extractMelody(std::span<const float> mono22050,
              const std::filesystem::path& model = defaultMelodyModel(),
              const MelodyProgress& progress = {}, const NoteCreationOptions& options = {});

#endif

} // namespace adx::analysis

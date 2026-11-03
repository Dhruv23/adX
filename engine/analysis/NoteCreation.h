// adx-thread: main
//
// basic-pitch's note creation (note_creation.py, output_to_notes_polyphonic), ported from
// the second half of _archive/src-cpp/src/MelodyExtractor.cpp: frame and onset
// activations in, notes out. Split from the extractor (phase_10.md §11) because it is
// pure arithmetic and testable without the model; MelodyExtractor is the ONNX half and
// compiles only with ADX_ENABLE_ONNX.
//
//   onsets      the model's, or a frame-energy rise rescaled to the same peak,
//               whichever is larger, so a chord tone that fades in still starts a note
//   pass 1      onset peaks above 0.5, latest first, each grown forward while the frame
//               energy stays above 0.3 with up to 11 quiet frames tolerated
//   pass 2      the "melodia trick": the loudest unclaimed energy grown both ways, until
//               none is above 0.3; recovers tones whose onsets were never seen
//   claims      a note zeroes its bin and both neighbours, which the model bleeds into
//
// Frames are 86 per second (22050 / 256); bin 0 is A0 (MIDI 21).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace adx::analysis {

inline constexpr std::uint32_t kPitchBins = 88;
inline constexpr double kActivationFps = 22050.0 / 256.0;

/// frames x kPitchBins, row-major.
struct Activations {
    std::uint32_t frames{0};
    std::vector<float> note;
    std::vector<float> onset;

    [[nodiscard]] float noteAt(std::uint32_t frame, std::uint32_t bin) const noexcept {
        return note[(static_cast<std::size_t>(frame) * kPitchBins) + bin];
    }
};

struct DetectedNote {
    double startSeconds{0.0};
    double seconds{0.0};
    std::uint8_t pitch{60};
    std::uint8_t velocity{100};
};

struct NoteCreationOptions {
    float onsetThreshold{0.5F};
    float frameThreshold{0.3F};
    /// 127.7 ms at 86 fps.
    std::uint32_t minimumFrames{11};
    std::uint32_t energyTolerance{11};
    bool inferOnsets{true};
    bool melodiaTrick{true};
};

/// Sorted by start, then pitch.
[[nodiscard]] std::vector<DetectedNote>
notesFromActivations(const Activations& activations, const NoteCreationOptions& options = {});

} // namespace adx::analysis

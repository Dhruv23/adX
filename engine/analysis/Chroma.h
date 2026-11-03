// adx-thread: main
//
// Twelve-bin chroma (phase_10.md §3 step 1): how much of each pitch class sounds, frame
// by frame. Shared by key and chord detection.
//
//   frames      4096-point Hann windows every 2048 samples (85 ms and 43 ms at 48 kHz)
//   bins        every FFT bin between 55 Hz and 4.2 kHz contributes its magnitude to
//               the pitch class of its frequency, split between the two nearest
//               semitones by distance, so a note between bins is not lost
//   weight      magnitude, square-rooted: one loud bass note should not outvote a chord
//   tuning      A4 = 440 Hz plus `tuningCents`, so a detuned recording is read in tune
//
// Each frame is normalised to sum 1; silence is all zeros. Pitch class 0 is C.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace adx::analysis {

using ChromaVector = std::array<float, 12>;

struct ChromaOptions {
    std::uint32_t frameSize{8192};
    std::uint32_t hop{4096};
    float minimumHz{80.0F};
    float maximumHz{2000.0F};
    float tuningCents{0.0F};
    float bassMaximumHz{250.0F};
    /// A frame quieter than this (RMS) is silence: all zeros.
    float silence{1e-4F};
    /// Subtract each frame's third-quietest class before normalising. Square-rooted
    /// magnitudes lift a broadband floor (distortion products, noise, reverb) into
    /// every class at once. The median would be that floor too, but it also strips the
    /// partials the harmonic templates expect (81.7 % -> 73 % on the chord corpus);
    /// the third-quietest takes the floor alone (-> 92 %, phase_10.md §11).
    bool subtractFloor{true};
    /// Called every 64 frames with progress in 0..1; returning false stops the
    /// chromagram where it is (an AnalysisJob's cancel).
    std::function<bool(double)> progress;
};

struct Chromagram {
    std::vector<ChromaVector> frames;
    /// The same frames from the bass band alone (below `bassMaximumHz`), not
    /// normalised: how much bass sounds matters as well as which.
    std::vector<ChromaVector> bass;
    std::uint32_t sampleRate{48000};
    std::uint32_t hop{2048};
    /// Built from notes (chromagramOfNotes): pure pitch classes, no partials, so the
    /// detectors match them against plain templates rather than harmonic ones.
    bool fromNotes{false};

    /// Seconds from the start to frame `index`'s centre.
    [[nodiscard]] double timeOf(std::size_t index, std::uint32_t frameSize) const noexcept;
};

[[nodiscard]] Chromagram chromagram(std::span<const float> mono, std::uint32_t sampleRate,
                                    const ChromaOptions& options = {});

/// The frames summed and normalised: the profile of the whole passage.
[[nodiscard]] ChromaVector profileOf(const Chromagram& chroma);
/// The bass frames summed and normalised.
[[nodiscard]] ChromaVector bassProfileOf(const Chromagram& chroma);

/// From notes rather than audio: each pitch weighted by its duration and velocity.
struct ChromaNote {
    std::uint8_t pitch{60};
    double duration{1.0};
    float velocity{1.0F};
};
[[nodiscard]] ChromaVector profileOfNotes(std::span<const ChromaNote> notes);

/// Notes on a timeline, for a chromagram of notes rather than audio.
struct TimedChromaNote {
    double startSeconds{0.0};
    double seconds{0.0};
    std::uint8_t pitch{60};
    float velocity{1.0F};
};
/// One frame every `frameSeconds`: each note weighted by how much of the frame it
/// covers and by its velocity. The bass frames hold the lowest sounding note alone,
/// which is what a bass part is. `sampleRate` and `hop` are set so that
/// timeOf(i, 0) is the frame's start.
[[nodiscard]] Chromagram chromagramOfNotes(std::span<const TimedChromaNote> notes,
                                           double frameSeconds);

/// Sums to one; all zeros stays all zeros.
void normalise(ChromaVector& chroma) noexcept;

} // namespace adx::analysis

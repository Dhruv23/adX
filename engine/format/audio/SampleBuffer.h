// adx-thread: main
//
// A decoded sample: planar float storage plus the facts about it (phase_4.md §4.11).
// Immutable once the decoder publishes it; the audio thread reads it only through a
// SampleView.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/format/audio/SampleView.h"

namespace adx::format {

/// Zero frames stored before and after the audio, so an interpolator reading a few
/// taps either side of the first or last frame (Interpolate.h: the 8-point sinc reads
/// index - 3 .. index + 4) never leaves the buffer and never needs a bounds check.
inline constexpr std::size_t kSampleGuardFrames = 8;

struct SampleBuffer {
    std::uint32_t sampleRate{0};
    /// Channels in the file. Up to two are kept; a file with more is folded to stereo
    /// (odd channels left, even right), which is what a sampler can play.
    std::uint32_t sourceChannels{0};
    std::uint64_t frames{0};
    /// kSampleGuardFrames of silence, `frames` of audio, kSampleGuardFrames of silence.
    std::vector<float> left;
    /// Empty for a mono file: the view reads `left` twice.
    std::vector<float> right;

    /// Points at the first frame of audio, past the leading guard.
    [[nodiscard]] SampleView view() const noexcept {
        const float* l = left.data() + kSampleGuardFrames;
        return SampleView{.left = l,
                          .right = right.empty() ? l : right.data() + kSampleGuardFrames,
                          .frames = frames,
                          .sampleRate = sampleRate};
    }
    /// The audio itself, without the guards.
    [[nodiscard]] std::span<const float> leftAudio() const noexcept {
        return std::span<const float>{left}.subspan(kSampleGuardFrames, frames);
    }
    [[nodiscard]] std::span<const float> rightAudio() const noexcept {
        return right.empty() ? leftAudio()
                             : std::span<const float>{right}.subspan(kSampleGuardFrames, frames);
    }
};

} // namespace adx::format

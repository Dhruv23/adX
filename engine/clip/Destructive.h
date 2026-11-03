// adx-thread: main
//
// Destructive clip edits: reverse, normalize, DC removal, gain, silence trim
// (phase_8.md §3.2, FINAL_PLAN §5.5).
//
// "Destructive" to the cached buffer, never to the file. Each edit writes a new buffer
// from the one before it; the decoded original is untouched, and the edit list is part
// of the project. Undoing an edit is removing it from the list, which brings back the
// original bit for bit, because the original was never changed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/format/audio/SampleBuffer.h"
#include "engine/project/Playlist.h"

namespace adx::clip {

/// `source` with `edits` applied in order. The result keeps the source's rate and has
/// guard frames like any decoded buffer.
[[nodiscard]] format::SampleBuffer applyEdits(const format::SampleBuffer& source,
                                              std::span<const project::ClipEdit> edits);

/// Planar audio without guards: one vector per channel (one for mono).
struct Planar {
    std::uint32_t sampleRate{0};
    std::uint32_t sourceChannels{0};
    std::vector<std::vector<float>> channels;

    [[nodiscard]] std::size_t frames() const noexcept {
        return channels.empty() ? 0 : channels.front().size();
    }
};

[[nodiscard]] Planar toPlanar(const format::SampleBuffer& buffer);
/// Back to a SampleBuffer, guards added.
[[nodiscard]] format::SampleBuffer fromPlanar(const Planar& planar);

void reverse(Planar& audio) noexcept;
/// Scales so the largest absolute sample is `peakDb` dBFS. Silence is left alone.
void normalize(Planar& audio, float peakDb) noexcept;
void removeDc(Planar& audio) noexcept;
void applyGain(Planar& audio, float db) noexcept;
/// Drops leading and trailing frames whose every channel is below `thresholdDb` dBFS.
void trimSilence(Planar& audio, float thresholdDb);

} // namespace adx::clip

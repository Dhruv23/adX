// adx-thread: main
//
// Audio file decoding: ported from _archive/src-cpp/src/AudioFileLoader.cpp, which
// already wrapped miniaudio (phase_4.md §4.11, §5).
//
// What changed: the archive decoded to interleaved stereo at the engine's rate,
// discarding the file's own rate and channel count, and resampled on the way in. This
// decodes at the file's native rate into planar storage and leaves rate conversion to
// the instrument that plays it, which needs the source rate anyway to pitch it. WAV
// (8/16/24/32-bit integer and float), AIFF, FLAC, MP3 and Ogg Vorbis.
//
// Main thread or a decoder worker; never the audio thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "engine/format/audio/SampleBuffer.h"

namespace adx::format {

/// Decodes `path` into `out`. On failure returns false and says why in `error`.
[[nodiscard]] bool decodeFile(const std::filesystem::path& path, SampleBuffer& out,
                              std::string& error);

/// The same for an encoded file already in memory.
[[nodiscard]] bool decodeMemory(std::span<const std::byte> bytes, SampleBuffer& out,
                                std::string& error);

/// FNV-1a 64 of a file's bytes: what the pool deduplicates by, so the same recording
/// saved under two names decodes once. False when the file cannot be read.
[[nodiscard]] bool hashFile(const std::filesystem::path& path, std::uint64_t& out);

} // namespace adx::format

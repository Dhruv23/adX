// adx-thread: main
//
// adX's own UTAU resampler: one note of one alias, re-pitched and re-timed through
// WORLD (phase_4.md §4.13). Worker threads only.
//
// The oto segment runs from `offset` to its cutoff. Its first `consonant` ms keep their
// natural speed, so a consonant is never smeared; the rest - the vowel - is stretched
// or compressed to fill the note. The render starts `preutterance` ms before the note
// (that is when the consonant begins) and ends at `cutMs` after the note's start - the
// note's length, or, when the next note follows without a rest, the point where the
// next note's overlap ends - with a short fade. F0 is the note's pitch, plus the
// note's slide/pitch-curve and the channel's vibrato and tuning, on voiced frames;
// unvoiced frames stay unvoiced. The flags are UTAU's common subset: `g` gender warps
// the spectral envelope's frequency axis (positive is deeper), `B` breathiness scales
// the aperiodicity (50 is neutral), `t` tuning in cents, `P` peak compression
// (normalises the note's peak toward a fixed level; 0 leaves it alone).
//
// The output is at kVoiceRenderRate whatever the bank's rate: conversion happens once,
// at the cache boundary. Rendering is deterministic: WORLD's synthesis has no
// randomness this uses, and nothing reads a clock.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/instruments/voice/Voicebank.h"

namespace adx::instruments {

inline constexpr std::uint32_t kVoiceRenderRate = 48000;
/// Part of every render key: bump it when the resampler's output changes.
inline constexpr std::uint32_t kResamplerVersion = 1;

struct NoteRenderRequest {
    std::shared_ptr<const Voicebank> bank;
    OtoEntry oto;
    double noteHz{261.6};
    double lengthMs{500.0};
    /// Where the render stops, ms after the note's start.
    double cutMs{500.0};
    /// Fade-out length at the cut, ms: the next note's overlap, or a default.
    double fadeOutMs{30.0};
    /// Pitch offset in cents on WORLD's 5 ms frames, from the note's start.
    std::vector<float> cents;
    double gender{0.0};
    double breathiness{50.0};
    double tuningCents{0.0};
    double peakCompression{86.0};
};

struct NoteRender {
    std::vector<float> samples;
    /// Samples before the note's start: the preutterance, at kVoiceRenderRate.
    std::uint32_t lead{0};
    bool ok{false};
};

/// A key that changes whenever anything that changes the render does.
[[nodiscard]] std::uint64_t renderKey(const NoteRenderRequest& request);

[[nodiscard]] NoteRender renderNote(const NoteRenderRequest& request);

} // namespace adx::instruments

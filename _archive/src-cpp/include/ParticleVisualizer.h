#pragma once

#include "AudioEngine.h" // AudioTap
#include <imgui.h>

// UI-Refactor Phase 4: transient-reactive particle "fountain" -- a purple/
// cyan burst fired on kicks/transients (the reference screenshots' bottom-
// left panel). UI-Thread-only: reads M0's shared AudioTap for onset
// detection (short-window RMS vs. a decaying envelope follower -- simple
// and cheap, no FFT needed for this) and owns a fixed-size, pre-allocated
// particle pool (no per-frame allocation, no per-frame heap churn).
namespace ParticleVisualizer {

// Advances the simulation by `dt` seconds: reads the latest window from
// `tap`, flags a transient burst when its RMS jumps above the decaying
// envelope follower, spawns particles from the pool on a burst, and
// integrates/fades/recycles every live particle. Call once per rendered
// frame, before Draw().
void Update(const AudioTap& tap, float dt);

// Draws the current particle pool into `drawList`, mapped from each
// particle's normalized [0,1] x [0,1] position (origin bottom-center, so
// particles arc up and outward like a fountain) into the
// [origin, origin+size) rectangle -- callers don't need to clip separately
// since this is always called from within an ImGui window's own Begin/End,
// which already clips its draw list to the window's content region.
void Draw(ImDrawList* drawList, ImVec2 origin, ImVec2 size);

} // namespace ParticleVisualizer

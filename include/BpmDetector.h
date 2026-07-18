#pragma once

#include <vector>

// Tempo estimation for imported samples/loops (Phase 5). Main Thread (or a
// background std::thread) only — this decodes nothing itself but does heavy
// O(n * lags) math; never call from AudioEngine::process().
namespace BpmDetector {

// Estimates the tempo of a mono PCM buffer via autocorrelation of an
// onset-energy envelope. Returns 0.0f when no confident estimate exists
// (e.g. a one-shot kick shorter than ~2 seconds, or no periodic energy).
// The result is folded into the 70..200 BPM range typical for EDM material.
float EstimateBpm(const std::vector<float>& monoSamples, unsigned int sampleRate);

}

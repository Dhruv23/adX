/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// Ported from msfa (google/music-synthesizer-for-android, app/src/main/jni) for adX:
// exp2.h, sin.h, freqlut.h and the sample-rate constants of lfo.cc and pitchenv.cc.
// Changes (adX, 2026): namespaced; the rate-independent tables (exp2, sin) are built
// once and const after; the rate-dependent ones (freqlut, the LFO and pitch-EG units)
// are a RateTables value per instrument instead of globals, so two instruments at two
// rates (engine rate, FM-1 rate) can coexist. The arithmetic is upstream's, unchanged.
#pragma once

#include <array>
#include <cstdint>

namespace adx::dx7::msfa {

/// The block size msfa renders in: 64 samples (upstream's LG_N / N).
inline constexpr int kLgN = 6;
inline constexpr int kN = 1 << kLgN;

inline constexpr int kExp2LgSamples = 10;
inline constexpr int kExp2Samples = 1 << kExp2LgSamples;
inline constexpr int kSinLgSamples = 10;
inline constexpr int kSinSamples = 1 << kSinLgSamples;
inline constexpr int kFreqLgSamples = 10;
inline constexpr int kFreqSamples = 1 << kFreqLgSamples;

struct ConstTables {
    std::array<std::int32_t, static_cast<std::size_t>(kExp2Samples) * 2> exp2{};
    std::array<std::int32_t, static_cast<std::size_t>(kSinSamples) * 2> sin{};
};

/// Built on first use (main thread, at prepare), never written again.
[[nodiscard]] const ConstTables& constTables();

struct RateTables {
    std::array<std::int32_t, kFreqSamples + 1> freqlut{};
    std::uint32_t lfoUnit{0};
    std::int32_t pitchEnvUnit{0};
    double sampleRate{0.0};
};

/// Main thread: the tables for `sampleRate`, as upstream's Freqlut::init, Lfo::init and
/// PitchEnv::init compute them.
void buildRateTables(RateTables& out, double sampleRate);

/// Exp2::lookup: Q24 in, Q24 out.
[[nodiscard]] inline std::int32_t exp2Lookup(const ConstTables& t, std::int32_t x) noexcept {
    constexpr int kShift = 24 - kExp2LgSamples;
    const int lowbits = x & ((1 << kShift) - 1);
    const int xInt = (x >> (kShift - 1)) & ((kExp2Samples - 1) << 1);
    const int dy = t.exp2[static_cast<std::size_t>(xInt)];
    const int y0 = t.exp2[static_cast<std::size_t>(xInt) + 1];
    const auto y = static_cast<std::int32_t>(
        y0 + ((static_cast<std::int64_t>(dy) * static_cast<std::int64_t>(lowbits)) >> kShift));
    return y >> (6 - (x >> 24));
}

/// Sin::lookup (SIN_DELTA form): Q24 phase in, Q24 out.
[[nodiscard]] inline std::int32_t sinLookup(const ConstTables& t, std::int32_t phase) noexcept {
    constexpr int kShift = 24 - kSinLgSamples;
    const int lowbits = phase & ((1 << kShift) - 1);
    const int phaseInt = (phase >> (kShift - 1)) & ((kSinSamples - 1) << 1);
    const int dy = t.sin[static_cast<std::size_t>(phaseInt)];
    const int y0 = t.sin[static_cast<std::size_t>(phaseInt) + 1];
    return static_cast<std::int32_t>(
        y0 + ((static_cast<std::int64_t>(dy) * static_cast<std::int64_t>(lowbits)) >> kShift));
}

/// Freqlut::lookup: log frequency (Q24 per octave) to a phase increment.
[[nodiscard]] inline std::int32_t freqLookup(const RateTables& t, std::int32_t logfreq) noexcept {
    constexpr int kShift = 24 - kFreqLgSamples;
    constexpr int kMaxLogfreqInt = 20;
    const int ix = (logfreq & 0xffffff) >> kShift;
    const std::int32_t y0 = t.freqlut[static_cast<std::size_t>(ix)];
    const std::int32_t y1 = t.freqlut[static_cast<std::size_t>(ix) + 1];
    const int lowbits = logfreq & ((1 << kShift) - 1);
    const auto y = static_cast<std::int32_t>(
        y0 + ((static_cast<std::int64_t>(y1 - y0) * static_cast<std::int64_t>(lowbits)) >> kShift));
    const int hibits = logfreq >> 24;
    return y >> (kMaxLogfreqInt - hibits);
}

} // namespace adx::dx7::msfa

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
// adx-thread: main
// Ported from msfa's exp2.cc, sin.cc, freqlut.cc, lfo.cc (init) and pitchenv.cc (init).
// Changes (adX, 2026): see Tables.h. Each loop is upstream's, statement for statement.
#include "engine/instruments/dx7/msfa/Tables.h"

#include <cmath>

namespace adx::dx7::msfa {

namespace {

// Upstream computes these with libm: exp2(1/1024) (the same value as its
// pow(2, 1/1024)) and round(cos/sin(2 pi / 1024) * 2^30). They are written out as
// the exact results, so the tables are msfa's in every build configuration and no
// library transcendental runs (tools/lint.py dsp-math); dx7_engine_matches_msfa
// compares them with upstream's own.
constexpr double kStep1024 = 0x1.002c605e2e8cfp+0;
constexpr std::int32_t kCos1024 = 1073721611;
constexpr std::int32_t kSin1024 = 6588356;

[[nodiscard]] constexpr std::size_t at(int i) noexcept {
    return static_cast<std::size_t>(i);
}

ConstTables build() {
    ConstTables t;
    // Exp2::init
    {
        const double inc = kStep1024; // exp2(1.0 / kExp2Samples)
        double y = 1 << 30;
        for (int i = 0; i < kExp2Samples; i++) {
            t.exp2[(at(i) * 2) + 1] = static_cast<std::int32_t>(std::floor(y + 0.5));
            y *= inc;
        }
        for (int i = 0; i < kExp2Samples - 1; i++) {
            t.exp2[at(i) * 2] = t.exp2[(at(i) * 2) + 3] - t.exp2[(at(i) * 2) + 1];
        }
        t.exp2[(at(kExp2Samples) * 2) - 2] = static_cast<std::int32_t>(
            (1U << 31) - static_cast<std::uint32_t>(t.exp2[(at(kExp2Samples) * 2) - 1]));
    }
    // Sin::init (SIN_DELTA)
    {
        constexpr std::int64_t kR = 1 << 29;
        const std::int32_t c = kCos1024;
        const std::int32_t s = kSin1024;
        std::int32_t u = 1 << 30;
        std::int32_t v = 0;
        for (int i = 0; i < kSinSamples / 2; i++) {
            t.sin[(at(i) * 2) + 1] = (v + 32) >> 6;
            t.sin[((at(i) + at(kSinSamples / 2)) * 2) + 1] = -((v + 32) >> 6);
            const auto next = static_cast<std::int32_t>(
                ((static_cast<std::int64_t>(u) * s) + (static_cast<std::int64_t>(v) * c) + kR) >>
                30);
            u = static_cast<std::int32_t>(
                ((static_cast<std::int64_t>(u) * c) - (static_cast<std::int64_t>(v) * s) + kR) >>
                30);
            v = next;
        }
        for (int i = 0; i < kSinSamples - 1; i++) {
            t.sin[at(i) * 2] = t.sin[(at(i) * 2) + 3] - t.sin[(at(i) * 2) + 1];
        }
        t.sin[(at(kSinSamples) * 2) - 2] = -t.sin[(at(kSinSamples) * 2) - 1];
    }
    return t;
}

} // namespace

const ConstTables& constTables() {
    static const ConstTables kTables = build();
    return kTables;
}

void buildRateTables(RateTables& out, double sampleRate) {
    // Freqlut::init
    constexpr int kMaxLogfreqInt = 20;
    double y = static_cast<double>(1LL << (24 + kMaxLogfreqInt)) / sampleRate;
    const double inc = kStep1024; // pow(2, 1.0 / kFreqSamples)
    for (int i = 0; i < kFreqSamples + 1; i++) {
        out.freqlut[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(std::floor(y + 0.5));
        y *= inc;
    }
    // Lfo::init: 1 << 32 / 15.5 s / 11
    // Upstream's rounding (a truncating cast of x + 0.5), kept: the tables must be
    // msfa's bit for bit, and these values are positive.
    // NOLINTBEGIN(bugprone-incorrect-roundings)
    out.lfoUnit =
        static_cast<std::uint32_t>(static_cast<std::int32_t>((kN * 25190424 / sampleRate) + 0.5));
    // PitchEnv::init
    out.pitchEnvUnit = static_cast<std::int32_t>((kN * (1 << 24) / (21.3 * sampleRate)) + 0.5);
    // NOLINTEND(bugprone-incorrect-roundings)
    out.sampleRate = sampleRate;
}

} // namespace adx::dx7::msfa

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
// Ported from msfa's dx7note.cc. Changes: see Dx7Note.h; each is marked "adX:".
#include "engine/instruments/dx7/msfa/Dx7Note.h"

#include <algorithm>
#include <cmath>

namespace adx::dx7::msfa {

namespace {

constexpr int kLevelThresh = 1120;

std::int32_t midinoteToLogfreq(int midinote) noexcept {
    constexpr int kBase = 50857777; // (1 << 24) * (log(440) / log(2) - 69/12)
    constexpr int kStep = (1 << 24) / 12;
    return kBase + (kStep * midinote);
}

constexpr std::array<std::int32_t, 32> kCoarseMul = {
    -16777216, 0,        16777216, 26591258, 33554432, 38955489, 43368474, 47099600,
    50331648,  53182516, 55732705, 58039632, 60145690, 62083076, 63876816, 65546747,
    67108864,  68576247, 69959732, 71268397, 72509921, 73690858, 74816848, 75892776,
    76922906,  77910978, 78860292, 79773775, 80654032, 81503396, 82323963, 83117622};

/// floor((1 << 24) / ln 2 * ln(1 + 0.01 fine) + 0.5) for fine 0..99, as upstream's
/// osc_freq computes it with libm; written out so no library transcendental runs on the
/// audio thread (tools/lint.py dsp-math) and the pitch is the same in every build.
constexpr std::array<std::int32_t, 100> kFineLog = {
    0,        240842,   479311,   715453,   949314,   1180937,  1410364,  1637637,  1862796,
    2085880,  2306926,  2525972,  2743054,  2958205,  3171461,  3382855,  3592418,  3800183,
    4006179,  4210437,  4412985,  4613853,  4813067,  5010655,  5206643,  5401057,  5593922,
    5785262,  5975102,  6163464,  6350371,  6535847,  6719911,  6902587,  7083894,  7263853,
    7442485,  7619807,  7795840,  7970602,  8144111,  8316385,  8487441,  8657298,  8825970,
    8993475,  9159829,  9325048,  9489146,  9652139,  9814042,  9974869,  10134635, 10293353,
    10451037, 10607700, 10763356, 10918018, 11071697, 11224407, 11376159, 11526966, 11676839,
    11825789, 11973829, 12120969, 12267219, 12412591, 12557096, 12700743, 12843542, 12985504,
    13126637, 13266953, 13406460, 13545168, 13683085, 13820221, 13956584, 14092183, 14227027,
    14361124, 14494482, 14627109, 14759014, 14890203, 15020685, 15150468, 15279559, 15407964,
    15535692, 15662750, 15789144, 15914881, 16039969, 16164413, 16288221, 16411399, 16533954,
    16655890,
};

std::int32_t oscFreq(int midinote, int mode, int coarse, int fine, int detune) noexcept {
    std::int32_t logfreq = 0;
    if (mode == 0) {
        logfreq = midinoteToLogfreq(midinote);
        logfreq += kCoarseMul[static_cast<std::size_t>(coarse & 31)];
        if (fine != 0) {
            // floor(24204406.323123 * log(1 + 0.01 fine) + 0.5): upstream's libm result
            // per fine step, written out (see kFineLog).
            logfreq += kFineLog[static_cast<std::size_t>(std::clamp(fine, 0, 99))];
        }
        logfreq += 12606 * (detune - 7);
    } else {
        // ((1 << 24) * log(10) / log(2) * .01) << 3
        logfreq = (4458616 * (((coarse & 3) * 100) + fine)) >> 3;
        logfreq += detune > 7 ? 13457 * (detune - 7) : 0;
    }
    return logfreq;
}

constexpr std::array<std::uint8_t, 64> kVelocityData = {
    0,   70,  86,  97,  106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
    166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202, 205,
    206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232, 233, 235,
    237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251, 252, 253, 254};

int scaleVelocity(int velocity, int sensitivity) noexcept {
    const int clampedVel = std::max(0, std::min(127, velocity));
    const int velValue = kVelocityData[static_cast<std::size_t>(clampedVel >> 1)] - 239;
    return ((sensitivity * velValue + 7) >> 3) << 4;
}

int scaleRate(int midinote, int sensitivity) noexcept {
    const int x = std::min(31, std::max(0, (midinote / 3) - 7));
    return (sensitivity * x) >> 3;
}

constexpr std::array<std::uint8_t, 33> kExpScaleData = {
    0,  1,  2,  3,  4,  5,  6,   7,   8,   9,   11,  14,  16,  19,  23,  27, 33,
    39, 47, 56, 66, 80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250};

int scaleCurve(int group, int depth, int curve) noexcept {
    int scale = 0;
    if (curve == 0 || curve == 3) {
        scale = (group * depth * 329) >> 12; // linear
    } else {
        const int nScaleData = static_cast<int>(kExpScaleData.size());
        const int rawExp = kExpScaleData[static_cast<std::size_t>(std::min(group, nScaleData - 1))];
        scale = (rawExp * depth * 329) >> 15;
    }
    if (curve < 2) {
        scale = -scale;
    }
    return scale;
}

int scaleLevel(int midinote, int breakPt, int leftDepth, int rightDepth, int leftCurve,
               int rightCurve) noexcept {
    const int offset = midinote - breakPt - 17;
    if (offset >= 0) {
        return scaleCurve(offset / 3, rightDepth, rightCurve);
    }
    return scaleCurve((-offset) / 3, leftDepth, leftCurve);
}

constexpr std::array<std::uint8_t, 8> kPitchModSensTab = {0, 10, 20, 33, 55, 92, 153, 255};

// adX: how far each AMS setting moves an operator at full amplitude modulation, in
// Q24 doublings (AMS 0 none; 3 about 42 dB, close to the DX7's near-silence).
constexpr std::array<std::int32_t, 4> kAmpModSens = {0, 1 << 23, 3 << 23, 7 << 24};

} // namespace

void Dx7Note::setOperators(const std::uint8_t* patch, bool retrigger) noexcept {
    std::array<int, 4> rates{};
    std::array<int, 4> levels{};
    for (std::size_t op = 0; op < 6; op++) {
        const std::size_t off = op * 21;
        for (std::size_t i = 0; i < 4; i++) {
            rates[i] = patch[off + i];
            levels[i] = patch[off + 4 + i];
        }
        int outlevel = patch[off + 16];
        outlevel = Env::scaleoutlevel(outlevel);
        const int levelScaling = scaleLevel(m_midinote, patch[off + 8], patch[off + 9],
                                            patch[off + 10], patch[off + 11], patch[off + 12]);
        outlevel += levelScaling;
        outlevel = std::min(127, outlevel);
        outlevel = outlevel << 5;
        outlevel += scaleVelocity(m_velocity, patch[off + 15]);
        outlevel = std::max(0, outlevel);
        const int rateScaling = scaleRate(m_midinote, patch[off + 13]);
        if (retrigger) {
            m_env[op].init(rates, levels, outlevel, rateScaling);
        } else {
            m_env[op].update(rates, levels, outlevel, rateScaling); // adX
        }
        const int mode = patch[off + 17];
        const int coarse = patch[off + 18];
        const int fine = patch[off + 19];
        const int detune = patch[off + 20];
        m_basepitch[op] = oscFreq(m_midinote, mode, coarse, fine, detune);
        m_ampmodsens[op] = patch[off + 14] & 3; // adX
        if (retrigger) {
            m_params[op].phase = 0;
            m_params[op].gain[1] = 0;
        }
    }
    for (std::size_t i = 0; i < 4; i++) {
        rates[i] = patch[126 + i];
        levels[i] = patch[130 + i];
    }
    if (retrigger) {
        m_pitchenv.set(*m_rate, rates, levels);
    } else {
        m_pitchenv.update(rates, levels); // adX
    }
    m_algorithm = patch[134];
    const int feedback = patch[135];
    m_fbShift = feedback != 0 ? 8 - feedback : 16;
    m_pitchmoddepth = (patch[139] * 165) >> 6;
    m_pitchmodsens = kPitchModSensTab[patch[143] & 7];
    m_ampmoddepth = (patch[140] * 165) >> 6; // adX
    m_opEnable = patch[155];                 // adX
}

void Dx7Note::init(const RateTables& rate, const std::uint8_t* patch, int midinote,
                   int velocity) noexcept {
    m_rate = &rate;
    m_midinote = midinote;
    m_velocity = velocity;
    m_pitchOffset = 0;
    setOperators(patch, true);
}

void Dx7Note::update(const std::uint8_t* patch) noexcept {
    if (m_rate != nullptr) {
        setOperators(patch, false);
    }
}

void Dx7Note::compute(std::int32_t* buf, std::int32_t lfoVal, std::int32_t lfoDelay,
                      const Controllers& ctrls) noexcept {
    std::int32_t pitchmod = m_pitchenv.getsample();
    // Upstream: uint32_t pmd = pitchmoddepth_ * lfo_delay (Q32).
    // (The product fits in 32 bits, so widening it first changes nothing.)
    std::uint64_t pmd = static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_pitchmoddepth)) *
                        static_cast<std::uint32_t>(lfoDelay);
    // adX: amplitude modulation depth from the LFO, Q24, and the controllers.
    std::int64_t amd = (static_cast<std::int64_t>(m_ampmoddepth) * lfoDelay) / 255;
    std::int64_t egBias = 0;
    for (std::size_t c = 0; c < 4; ++c) {
        if (ctrls.value[c] == 0 || ctrls.range[c] == 0) {
            continue;
        }
        const std::int64_t amount =
            (static_cast<std::int64_t>(ctrls.range[c]) * ctrls.value[c] * (1 << 24)) / (99LL * 127);
        if ((ctrls.assign[c] & 1) != 0) {
            pmd += static_cast<std::uint64_t>(amount) * 255U;
        }
        if ((ctrls.assign[c] & 2) != 0) {
            amd += amount;
        }
        if ((ctrls.assign[c] & 4) != 0) {
            egBias += amount;
        }
    }
    pmd = std::min<std::uint64_t>(pmd, 0xFFFFFFFFU);
    const std::int32_t senslfo = m_pitchmodsens * (lfoVal - (1 << 23));
    pitchmod += static_cast<std::int32_t>(
        (static_cast<std::int64_t>(pmd) * static_cast<std::int64_t>(senslfo)) >> 39);

    // adX: the bend range and step (upstream: (pitchbend - 0x2000) << 9, 3 semitones).
    const std::int64_t bend = ctrls.pitchBend - 0x2000;
    std::int64_t pb = (bend * ctrls.bendRange * (1 << 24)) / (12LL * 8192);
    if (ctrls.bendStep > 0) {
        const std::int64_t semitone = (1 << 24) / 12;
        const std::int64_t step = semitone * ctrls.bendStep;
        pb = (pb >= 0 ? (pb + (step / 2)) : (pb - (step / 2))) / step * step;
    }
    pitchmod += static_cast<std::int32_t>(pb);
    pitchmod += m_pitchOffset; // adX

    const std::int64_t ampMod = std::min<std::int64_t>(amd, 1 << 24) * lfoVal >> 24;
    for (std::size_t op = 0; op < 6; op++) {
        m_params[op].gain[0] = m_params[op].gain[1];
        std::int32_t level = m_env[op].getsample();
        if (m_ampmodsens[op] != 0 && (ampMod != 0 || egBias != 0)) { // adX
            const std::int64_t depth = std::min<std::int64_t>(ampMod + egBias, 1 << 24);
            level -= static_cast<std::int32_t>(
                (depth * kAmpModSens[static_cast<std::size_t>(m_ampmodsens[op])]) >> 24);
        }
        std::int32_t gain = exp2Lookup(constTables(), level - (14 * (1 << 24)));
        // adX: VCED 155 has OP6 in bit 0 ... OP1 in bit 5; index 0 here is OP6.
        if ((m_opEnable & (1U << op)) == 0) {
            gain = 0;
        }
        m_params[op].freq = freqLookup(*m_rate, m_basepitch[op] + pitchmod);
        m_params[op].gain[1] = gain;
    }
    m_core.compute(buf, m_params, m_algorithm, m_fbBuf, m_fbShift);
}

void Dx7Note::keyup() noexcept {
    for (std::size_t op = 0; op < 6; op++) {
        m_env[op].keydown(false);
        m_pitchenv.keydown(false);
    }
}

bool Dx7Note::finished() const noexcept {
    for (std::size_t op = 0; op < 6; op++) {
        const bool carrier = (algorithmFlags(m_algorithm, static_cast<int>(op)) & 3) == 0;
        if (!carrier) {
            continue;
        }
        if (!m_env[op].finished() || m_params[op].gain[0] >= kLevelThresh ||
            m_params[op].gain[1] >= kLevelThresh) {
            return false;
        }
    }
    return true;
}

} // namespace adx::dx7::msfa

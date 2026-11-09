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
// Ported from msfa's pitchenv.cc (tables) and lfo.cc. Changes: see Env.h.
#include "engine/instruments/dx7/msfa/Env.h"

#include <algorithm>

namespace adx::dx7::msfa {

namespace {

constexpr std::array<std::uint8_t, 100> kRateTab = {
    1,   2,   3,   3,   4,   4,   5,   5,   6,   6,   7,   7,   8,   8,   9,   9,   10,
    10,  11,  11,  12,  12,  13,  13,  14,  14,  15,  16,  16,  17,  18,  18,  19,  20,
    21,  22,  23,  24,  25,  26,  27,  28,  30,  31,  33,  34,  36,  37,  38,  39,  41,
    42,  44,  46,  47,  49,  51,  53,  54,  56,  58,  60,  62,  64,  66,  68,  70,  72,
    74,  76,  79,  82,  85,  88,  91,  94,  98,  102, 106, 110, 115, 120, 125, 130, 135,
    141, 147, 153, 159, 165, 171, 178, 185, 193, 202, 211, 232, 243, 254, 255};

constexpr std::array<std::int8_t, 100> kPitchTab = {
    -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43, -41, -39, -37, -35,
    -33,  -32,  -31,  -30, -29, -28, -27, -26, -25, -24, -23, -22, -21, -20, -19, -18, -17,
    -16,  -15,  -14,  -13, -12, -11, -10, -9,  -8,  -7,  -6,  -5,  -4,  -3,  -2,  -1,  0,
    1,    2,    3,    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,
    18,   19,   20,   21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,
    35,   38,   40,   43,  46,  49,  53,  58,  65,  73,  82,  92,  103, 115, 127};

} // namespace

int PitchEnv::pitch(int level) noexcept {
    return kPitchTab[static_cast<std::size_t>(std::clamp(level, 0, 99))];
}

int PitchEnv::rateOf(int rate) noexcept {
    return kRateTab[static_cast<std::size_t>(std::clamp(rate, 0, 99))];
}

void Lfo::reset(const RateTables& rate, const std::uint8_t* params) noexcept {
    m_unit = rate.lfoUnit;
    const int speed = params[0]; // 0..99
    int sr = speed == 0 ? 1 : (165 * speed) >> 6;
    sr *= sr < 160 ? 11 : (11 + ((sr - 160) >> 4));
    m_delta = m_unit * static_cast<std::uint32_t>(sr);
    int a = 99 - params[1]; // LFO delay
    if (a == 99) {
        m_delayInc = ~0U;
        m_delayInc2 = ~0U;
    } else {
        a = (16 + (a & 15)) << (1 + (a >> 4));
        m_delayInc = m_unit * static_cast<std::uint32_t>(a);
        a &= 0xff80;
        a = a > 0x80 ? a : 0x80;
        m_delayInc2 = m_unit * static_cast<std::uint32_t>(a);
    }
    m_waveform = params[5];
    m_sync = params[4] != 0;
}

std::int32_t Lfo::getsample() noexcept {
    m_phase += m_delta;
    std::int32_t x = 0;
    switch (m_waveform) {
    case 0: // triangle
        x = static_cast<std::int32_t>(m_phase >> 7);
        x ^= -static_cast<std::int32_t>(m_phase >> 31);
        x &= (1 << 24) - 1;
        return x;
    case 1: // sawtooth down
        return static_cast<std::int32_t>((~m_phase ^ (1U << 31)) >> 8);
    case 2: // sawtooth up
        return static_cast<std::int32_t>((m_phase ^ (1U << 31)) >> 8);
    case 3: // square
        return static_cast<std::int32_t>(((~m_phase) >> 7) & (1U << 24));
    case 4: // sine
        return (1 << 23) + (sinLookup(constTables(), static_cast<std::int32_t>(m_phase >> 8)) >> 1);
    case 5: // s&h
        if (m_phase < m_delta) {
            m_randState = static_cast<std::uint8_t>((m_randState * 179 + 17) & 0xff);
        }
        x = m_randState ^ 0x80;
        return (x + 1) << 16;
    default:
        return 1 << 23;
    }
}

std::int32_t Lfo::getdelay() noexcept {
    const std::uint32_t delta = m_delayState < (1U << 31) ? m_delayInc : m_delayInc2;
    const std::uint32_t d = m_delayState + delta;
    if (d < m_delayInc) {
        return 1 << 24;
    }
    m_delayState = d;
    if (d < (1U << 31)) {
        return 0;
    }
    return static_cast<std::int32_t>((d >> 7) & ((1U << 24) - 1));
}

} // namespace adx::dx7::msfa

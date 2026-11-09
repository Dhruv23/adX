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
// Ported from msfa's fm_core.cc and fm_op_kernel.cc. Changes: see FmCore.h.
#include "engine/instruments/dx7/msfa/FmCore.h"

namespace adx::dx7::msfa {

namespace {

enum : std::uint8_t {
    kOutBusOne = 1 << 0,
    kOutBusTwo = 1 << 1,
    kOutBusAdd = 1 << 2,
    kInBusOne = 1 << 4,
    kInBusTwo = 1 << 5,
    kFbIn = 1 << 6,
    kFbOut = 1 << 7,
};

constexpr std::array<std::array<int, 6>, 32> kAlgorithms = {{
    {0xc1, 0x11, 0x11, 0x14, 0x01, 0x14}, // 1
    {0x01, 0x11, 0x11, 0x14, 0xc1, 0x14}, // 2
    {0xc1, 0x11, 0x14, 0x01, 0x11, 0x14}, // 3
    {0x41, 0x11, 0x94, 0x01, 0x11, 0x14}, // 4
    {0xc1, 0x14, 0x01, 0x14, 0x01, 0x14}, // 5
    {0x41, 0x94, 0x01, 0x14, 0x01, 0x14}, // 6
    {0xc1, 0x11, 0x05, 0x14, 0x01, 0x14}, // 7
    {0x01, 0x11, 0xc5, 0x14, 0x01, 0x14}, // 8
    {0x01, 0x11, 0x05, 0x14, 0xc1, 0x14}, // 9
    {0x01, 0x05, 0x14, 0xc1, 0x11, 0x14}, // 10
    {0xc1, 0x05, 0x14, 0x01, 0x11, 0x14}, // 11
    {0x01, 0x05, 0x05, 0x14, 0xc1, 0x14}, // 12
    {0xc1, 0x05, 0x05, 0x14, 0x01, 0x14}, // 13
    {0xc1, 0x05, 0x11, 0x14, 0x01, 0x14}, // 14
    {0x01, 0x05, 0x11, 0x14, 0xc1, 0x14}, // 15
    {0xc1, 0x11, 0x02, 0x25, 0x05, 0x14}, // 16
    {0x01, 0x11, 0x02, 0x25, 0xc5, 0x14}, // 17
    {0x01, 0x11, 0x11, 0xc5, 0x05, 0x14}, // 18
    {0xc1, 0x14, 0x14, 0x01, 0x11, 0x14}, // 19
    {0x01, 0x05, 0x14, 0xc1, 0x14, 0x14}, // 20
    {0x01, 0x14, 0x14, 0xc1, 0x14, 0x14}, // 21
    {0xc1, 0x14, 0x14, 0x14, 0x01, 0x14}, // 22
    {0xc1, 0x14, 0x14, 0x01, 0x14, 0x04}, // 23
    {0xc1, 0x14, 0x14, 0x14, 0x04, 0x04}, // 24
    {0xc1, 0x14, 0x14, 0x04, 0x04, 0x04}, // 25
    {0xc1, 0x05, 0x14, 0x01, 0x14, 0x04}, // 26
    {0x01, 0x05, 0x14, 0xc1, 0x14, 0x04}, // 27
    {0x04, 0xc1, 0x11, 0x14, 0x01, 0x14}, // 28
    {0xc1, 0x14, 0x01, 0x14, 0x04, 0x04}, // 29
    {0x04, 0xc1, 0x11, 0x14, 0x04, 0x04}, // 30
    {0xc1, 0x14, 0x04, 0x04, 0x04, 0x04}, // 31
    {0xc4, 0x04, 0x04, 0x04, 0x04, 0x04}, // 32
}};

[[nodiscard]] std::int32_t wrapAdd(std::int32_t a, std::int32_t b) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

[[nodiscard]] std::int32_t scaled(std::int32_t y, std::int32_t gain) noexcept {
    return static_cast<std::int32_t>((static_cast<std::int64_t>(y) * gain) >> 24);
}

// FmOpKernel::compute: an operator modulated by `input`.
void opCompute(const ConstTables& t, std::int32_t* output, const std::int32_t* input,
               std::int32_t phase0, std::int32_t freq, std::int32_t gain1, std::int32_t gain2,
               bool add) noexcept {
    const std::int32_t dgain = (gain2 - gain1 + (kN >> 1)) >> kLgN;
    std::int32_t gain = gain1;
    std::int32_t phase = phase0;
    for (int i = 0; i < kN; i++) {
        gain += dgain;
        const std::int32_t y = sinLookup(t, wrapAdd(phase, input[i]));
        if (add) {
            output[i] = wrapAdd(output[i], scaled(y, gain));
        } else {
            output[i] = scaled(y, gain);
        }
        phase = wrapAdd(phase, freq);
    }
}

// FmOpKernel::compute_pure: a sine, no modulation.
void opComputePure(const ConstTables& t, std::int32_t* output, std::int32_t phase0,
                   std::int32_t freq, std::int32_t gain1, std::int32_t gain2, bool add) noexcept {
    const std::int32_t dgain = (gain2 - gain1 + (kN >> 1)) >> kLgN;
    std::int32_t gain = gain1;
    std::int32_t phase = phase0;
    for (int i = 0; i < kN; i++) {
        gain += dgain;
        const std::int32_t y = sinLookup(t, phase);
        if (add) {
            output[i] = wrapAdd(output[i], scaled(y, gain));
        } else {
            output[i] = scaled(y, gain);
        }
        phase = wrapAdd(phase, freq);
    }
}

// FmOpKernel::compute_fb: one operator feeding back on itself.
void opComputeFb(const ConstTables& t, std::int32_t* output, std::int32_t phase0, std::int32_t freq,
                 std::int32_t gain1, std::int32_t gain2, std::array<std::int32_t, 2>& fbBuf,
                 int fbShift, bool add) noexcept {
    const std::int32_t dgain = (gain2 - gain1 + (kN >> 1)) >> kLgN;
    std::int32_t gain = gain1;
    std::int32_t phase = phase0;
    std::int32_t y0 = fbBuf[0];
    std::int32_t y = fbBuf[1];
    for (int i = 0; i < kN; i++) {
        gain += dgain;
        const std::int32_t scaledFb = (y0 + y) >> (fbShift + 1);
        y0 = y;
        y = sinLookup(t, wrapAdd(phase, scaledFb));
        y = scaled(y, gain);
        output[i] = add ? wrapAdd(output[i], y) : y;
        phase = wrapAdd(phase, freq);
    }
    fbBuf[0] = y0;
    fbBuf[1] = y;
}

} // namespace

int algorithmFlags(int algorithm, int op) noexcept {
    return kAlgorithms[static_cast<std::size_t>(algorithm & 31)][static_cast<std::size_t>(op)];
}

void FmCore::compute(std::int32_t* output, std::array<FmOpParams, 6>& params, int algorithm,
                     std::array<std::int32_t, 2>& fbBuf, int feedbackShift) noexcept {
    constexpr int kLevelThresh = 1120;
    const ConstTables& tables = constTables();
    const std::array<int, 6>& alg = kAlgorithms[static_cast<std::size_t>(algorithm & 31)];
    std::array<bool, 3> hasContents = {true, false, false};
    for (std::size_t op = 0; op < 6; op++) {
        const int flags = alg[op];
        bool add = (flags & kOutBusAdd) != 0;
        FmOpParams& param = params[op];
        const int inbus = (flags >> 4) & 3;
        const int outbus = flags & 3;
        std::int32_t* outptr =
            (outbus == 0) ? output : m_buf[static_cast<std::size_t>(outbus - 1)].data();
        const std::int32_t gain1 = param.gain[0];
        const std::int32_t gain2 = param.gain[1];
        if (gain1 >= kLevelThresh || gain2 >= kLevelThresh) {
            if (!hasContents[static_cast<std::size_t>(outbus)]) {
                add = false;
            }
            if (inbus == 0 || !hasContents[static_cast<std::size_t>(inbus)]) {
                if ((flags & 0xc0) == 0xc0 && feedbackShift < 16) {
                    opComputeFb(tables, outptr, param.phase, param.freq, gain1, gain2, fbBuf,
                                feedbackShift, add);
                } else {
                    opComputePure(tables, outptr, param.phase, param.freq, gain1, gain2, add);
                }
            } else {
                opCompute(tables, outptr, m_buf[static_cast<std::size_t>(inbus - 1)].data(),
                          param.phase, param.freq, gain1, gain2, add);
            }
            hasContents[static_cast<std::size_t>(outbus)] = true;
        } else if (!add) {
            hasContents[static_cast<std::size_t>(outbus)] = false;
        }
        param.phase = static_cast<std::int32_t>(static_cast<std::uint32_t>(param.phase) +
                                                (static_cast<std::uint32_t>(param.freq) << kLgN));
    }
}

} // namespace adx::dx7::msfa

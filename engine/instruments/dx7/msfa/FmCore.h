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
// Ported from msfa's fm_core.h/.cc and fm_op_kernel.h/.cc (the portable, non-NEON
// paths, which are the ones x86 runs). Changes (adX, 2026): namespaced; the kernels
// take the sine table explicitly; phase arithmetic is done in uint32 and cast back, so
// the wrap upstream relies on is defined behaviour (same bits on two's complement).
#pragma once

#include <array>
#include <cstdint>

#include "engine/instruments/dx7/msfa/Tables.h"

namespace adx::dx7::msfa {

struct FmOpParams {
    std::array<std::int32_t, 2> gain{};
    std::int32_t freq{0};
    std::int32_t phase{0};
};

/// The algorithm's operator flags, as upstream's table (index 0 is operator 6).
[[nodiscard]] int algorithmFlags(int algorithm, int op) noexcept;

class FmCore {
public:
    /// Adds this note's 64 samples to `output`.
    void compute(std::int32_t* output, std::array<FmOpParams, 6>& params, int algorithm,
                 std::array<std::int32_t, 2>& fbBuf, int feedbackShift) noexcept;

private:
    std::array<std::array<std::int32_t, kN>, 2> m_buf{};
};

} // namespace adx::dx7::msfa

#include "engine/dsp/TruePeak.h"

#include <algorithm>
#include <cmath>

namespace adx::dsp {
namespace {

// BS.1770-4 Annex 2, Table: the 4-phase, 12-taps-per-phase interpolation filter.
// NOLINTBEGIN(readability-magic-numbers) - the standard's coefficients.
constexpr std::array<std::array<float, 12>, 4> kPhases{{
    {0.0017089843750F, 0.0109863281250F, -0.0196533203125F, 0.0332031250000F, -0.0594482421875F,
     0.1373291015625F, 0.9721679687500F, -0.1022949218750F, 0.0476074218750F, -0.0266113281250F,
     0.0148925781250F, -0.0083007812500F},
    {-0.0291748046875F, 0.0292968750000F, -0.0517578125000F, 0.0891113281250F, -0.1665039062500F,
     0.4650878906250F, 0.7797851562500F, -0.2003173828125F, 0.1015625000000F, -0.0582275390625F,
     0.0330810546875F, -0.0189208984375F},
    {-0.0189208984375F, 0.0330810546875F, -0.0582275390625F, 0.1015625000000F, -0.2003173828125F,
     0.7797851562500F, 0.4650878906250F, -0.1665039062500F, 0.0891113281250F, -0.0517578125000F,
     0.0292968750000F, -0.0291748046875F},
    {-0.0083007812500F, 0.0148925781250F, -0.0266113281250F, 0.0476074218750F, -0.1022949218750F,
     0.9721679687500F, 0.1373291015625F, -0.0594482421875F, 0.0332031250000F, -0.0196533203125F,
     0.0109863281250F, 0.0017089843750F},
}};
// NOLINTEND(readability-magic-numbers)

} // namespace

float TruePeak::push(float sample) noexcept {
    m_history[m_ringIndex] = sample;
    m_ringIndex = (m_ringIndex + 1) % kTaps;
    float peak = 0.0F;
    for (const std::array<float, kTaps>& phase : kPhases) {
        float sum = 0.0F;
        // Newest sample meets the last tap: a plain convolution over the history.
        for (std::size_t t = 0; t < kTaps; ++t) {
            sum += phase[t] * m_history[(m_ringIndex + t) % kTaps];
        }
        peak = std::max(peak, std::abs(sum));
    }
    return peak;
}

} // namespace adx::dsp

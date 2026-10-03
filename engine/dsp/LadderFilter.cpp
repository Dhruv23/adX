#include "engine/dsp/LadderFilter.h"

#include <algorithm>

namespace adx::dsp {

LadderCoefficients ladderCoefficients(double cutoff, double resonance, double sampleRate) noexcept {
    const double fc = std::clamp(cutoff, 1.0, sampleRate * 0.4999);
    const double g = tanTurns(0.5 * fc / sampleRate);
    return LadderCoefficients{.stage = static_cast<float>(g / (1.0 + g)),
                              .k = static_cast<float>(4.0 * std::clamp(resonance, 0.0, 1.0))};
}

} // namespace adx::dsp

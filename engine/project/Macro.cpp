#include "engine/project/Macro.h"

#include <algorithm>

namespace adx::project {

float macroTargetValue(const MacroTarget& target, float value) noexcept {
    return core::interpolate(target.curve, target.minimum, target.maximum,
                             std::clamp(value, 0.0F, 1.0F));
}

} // namespace adx::project

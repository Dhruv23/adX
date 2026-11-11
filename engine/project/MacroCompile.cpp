// adx-thread: main
#include "engine/project/MacroCompile.h"

#include <algorithm>

#include "engine/dsp/Math.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"

namespace adx::project {

float normalisedIn(const ControllerRange& range, float value) noexcept {
    const auto low = static_cast<double>(range.minimum);
    const auto high = static_cast<double>(range.maximum);
    if (!(high > low)) {
        return 0.0F;
    }
    const auto v = static_cast<double>(value);
    if (range.logarithmic) {
        return static_cast<float>(dsp::log2(std::max(v, low * 1e-6) / low) / dsp::log2(high / low));
    }
    return static_cast<float>((v - low) / (high - low));
}

float MacroOffset::apply(float value) const noexcept {
    return range.map(normalisedIn(range, value) + offset);
}

MacroOffsets
foldMacros(const Project& project,
           const std::function<std::optional<std::uint32_t>(ParamRef)>& indexOf,
           std::vector<float>& params) {
    MacroOffsets offsets;
    for (const Macro& macro : project.macros) {
        for (const MacroTarget& target : macro.targets) {
            if (!target.target.valid() || target.target.kind == ParamKind::Macro) {
                continue;
            }
            const std::optional<std::uint32_t> index = indexOf(target.target);
            if (!index || *index >= params.size()) {
                continue;
            }
            const ParamDescriptor* descriptor =
                ParamRegistry::descriptorFor(target.target, project);
            float value = macroTargetValue(target, macro.value);
            if (descriptor != nullptr && descriptor->maximum > descriptor->minimum) {
                value = std::clamp(value, descriptor->minimum, descriptor->maximum);
            }
            ControllerRange range = ControllerRange::of(descriptor);
            if (descriptor == nullptr) {
                // No descriptor: the target's own two ends are the only range there is.
                range.minimum = std::min(target.minimum, target.maximum);
                range.maximum = std::max(target.minimum, target.maximum);
            }
            offsets[*index] =
                MacroOffset{.range = range,
                            .offset = normalisedIn(range, value) -
                                      normalisedIn(range, params[*index])};
            params[*index] = value;
        }
    }
    return offsets;
}

} // namespace adx::project

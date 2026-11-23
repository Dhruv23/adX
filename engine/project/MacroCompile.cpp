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

float macroValue(const Project& project, const Macro& macro) noexcept {
    float value = macro.value;
    for (const Controller& pad : project.controllers) {
        if (pad.kind != ControllerKind::Xy) {
            continue;
        }
        for (const std::uint16_t axis : {kAxisX, kAxisY}) {
            const ParamRef target = padTarget(pad, axis);
            if (target.kind == ParamKind::Macro && target.owner == macro.id.value) {
                value = padValue(pad, axis);
            }
        }
    }
    return value;
}

MacroOffsets foldMacros(const Project& project,
                        const std::function<std::optional<std::uint32_t>(ParamRef)>& indexOf,
                        std::vector<float>& params) {
    MacroOffsets offsets;
    const auto fold = [&](ParamRef target, float value, ControllerRange range) {
        const std::optional<std::uint32_t> index = indexOf(target);
        if (!index || *index >= params.size()) {
            return;
        }
        offsets[*index] =
            MacroOffset{.range = range,
                        .offset = normalisedIn(range, value) - normalisedIn(range, params[*index])};
        params[*index] = value;
    };
    for (const Macro& macro : project.macros) {
        const float position = macroValue(project, macro);
        for (const MacroTarget& target : macro.targets) {
            if (!target.target.valid() || target.target.kind == ParamKind::Macro ||
                target.target.kind == ParamKind::ControllerAxis) {
                continue;
            }
            const ParamDescriptor* descriptor =
                ParamRegistry::descriptorFor(target.target, project);
            float value = macroTargetValue(target, position);
            if (descriptor != nullptr && descriptor->maximum > descriptor->minimum) {
                value = std::clamp(value, descriptor->minimum, descriptor->maximum);
            }
            ControllerRange range = ControllerRange::of(descriptor);
            if (descriptor == nullptr) {
                // No descriptor: the target's own two ends are the only range there is.
                range.minimum = std::min(target.minimum, target.maximum);
                range.maximum = std::max(target.minimum, target.maximum);
            }
            fold(target.target, value, range);
        }
    }
    // X-Y pads straight onto a parameter (a pad on a macro moved the macro above).
    for (const Controller& pad : project.controllers) {
        if (pad.kind != ControllerKind::Xy) {
            continue;
        }
        for (const std::uint16_t axis : {kAxisX, kAxisY}) {
            const ParamRef target = padTarget(pad, axis);
            if (!target.valid() || target.kind == ParamKind::Macro ||
                target.kind == ParamKind::ControllerAxis) {
                continue;
            }
            const ControllerRange range =
                ControllerRange::of(ParamRegistry::descriptorFor(target, project));
            fold(target, range.map(padValue(pad, axis)), range);
        }
    }
    return offsets;
}

} // namespace adx::project

// One descriptor row for an effect's parameter table, with the common defaults - so a
// table of twenty rows reads as twenty rows.
#pragma once

#include <cstdint>
#include <string_view>

#include "engine/project/ParamDescriptor.h"

namespace adx::effects {

[[nodiscard]] constexpr project::ParamDescriptor
param(std::string_view name, float lo, float hi, float def, project::Unit unit,
      project::ScaleKind scale = project::ScaleKind::Linear,
      project::RateClass rate = project::RateClass::Block) {
    return project::ParamDescriptor{.name = name,
                                    .minimum = lo,
                                    .maximum = hi,
                                    .defaultValue = def,
                                    .unit = unit,
                                    .scale = scale,
                                    .rate = rate,
                                    .curve = project::CurvePart::None};
}

/// A choice: a stepped count.
[[nodiscard]] constexpr project::ParamDescriptor choice(std::string_view name, float hi,
                                                        float def) {
    return param(name, 0.0F, hi, def, project::Unit::Count, project::ScaleKind::Stepped);
}

/// A per-frame parameter: gain, mix, depth, a frequency read every sample.
[[nodiscard]] constexpr project::ParamDescriptor
perFrame(std::string_view name, float lo, float hi, float def, project::Unit unit,
         project::ScaleKind scale = project::ScaleKind::Linear) {
    return param(name, lo, hi, def, unit, scale, project::RateClass::Sample);
}

template<class E> [[nodiscard]] constexpr std::uint32_t idx(E e) noexcept {
    return static_cast<std::uint32_t>(e);
}

} // namespace adx::effects

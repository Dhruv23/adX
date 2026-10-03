// Every instrument and effect type this build knows, and the parameters each declares.
//
// One table, read by four parties that must agree: the graph builder, which lays out
// a node's parameter slice in the order its descriptors name; the parser, which
// warns about a parameter its type does not declare and reports one outside its
// range (P2-4, ADX2001); the automation compiler, which takes its tolerance from a
// parameter's range; and Phase 5's UI, which builds a generic editor from it
// (phase_4.md §9).
//
// The descriptor tables themselves live beside the nodes that read them
// (engine/instruments/*/...Params.h, engine/effects/...Params.h), so an instrument's
// parameter enum and its table are one edit apart. This file only lists them.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "engine/project/ParamDescriptor.h"

namespace adx::project {

enum class TypeKind : std::uint8_t { Instrument, Effect };

struct TypeInfo {
    std::string_view name;
    TypeKind kind{TypeKind::Instrument};
    std::span<const ParamDescriptor> params;
    /// For the browser and `adx info`.
    std::string_view summary;
};

/// No such parameter in the type.
inline constexpr std::uint32_t kNoParam = 0xFFFFFFFFU;

[[nodiscard]] std::span<const TypeInfo> instrumentTypes() noexcept;
[[nodiscard]] std::span<const TypeInfo> effectTypes() noexcept;

/// Exact, case-sensitive: instrument types are written lower case (`additive`),
/// effect types as v1 wrote them (`Reverb`).
[[nodiscard]] const TypeInfo* findInstrumentType(std::string_view name) noexcept;
[[nodiscard]] const TypeInfo* findEffectType(std::string_view name) noexcept;

/// The index of the value parameter `name` in `type` - not one of the curve
/// components that share its name - or kNoParam.
[[nodiscard]] std::uint32_t paramIndexOf(const TypeInfo& type, std::string_view name) noexcept;

/// The descriptor of value parameter `name` in `type`, or nullptr.
[[nodiscard]] const ParamDescriptor* findParam(const TypeInfo& type,
                                               std::string_view name) noexcept;

} // namespace adx::project

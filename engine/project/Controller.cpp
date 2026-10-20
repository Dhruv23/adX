#include "engine/project/Controller.h"

#include <array>

namespace adx::project {
namespace {

constexpr std::array<const char*, kControllerKindCount> kKindNames = {"lfo", "envelope", "peak"};
constexpr std::array<const char*, kLfoShapeCount> kShapeNames = {"sine", "triangle", "saw",
                                                                 "square"};

template<class Enum, std::size_t N>
bool fromName(const std::array<const char*, N>& names, std::string_view name, Enum& out) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (name == names.at(i)) {
            out = static_cast<Enum>(i);
            return true;
        }
    }
    return false;
}

} // namespace

const char* toString(ControllerKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kKindNames.size() ? kKindNames.at(index) : "lfo";
}

bool controllerKindFromString(std::string_view name, ControllerKind& out) noexcept {
    return fromName(kKindNames, name, out);
}

const char* toString(LfoShape shape) noexcept {
    const auto index = static_cast<std::size_t>(shape);
    return index < kShapeNames.size() ? kShapeNames.at(index) : "sine";
}

bool lfoShapeFromString(std::string_view name, LfoShape& out) noexcept {
    return fromName(kShapeNames, name, out);
}

} // namespace adx::project

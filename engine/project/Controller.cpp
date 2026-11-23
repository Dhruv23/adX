#include "engine/project/Controller.h"

#include <array>

namespace adx::project {
namespace {

constexpr std::array<const char*, kControllerKindCount> kKindNames = {"lfo",     "envelope", "peak",
                                                                      "formula", "keyboard", "xy"};
constexpr std::array<const char*, kLfoShapeCount> kShapeNames = {
    "sine", "triangle", "saw", "square", "saw-down", "random", "smooth-random"};
constexpr std::array<const char*, kKeyboardInputCount> kInputNames = {"pitch", "velocity"};

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

template<class Enum, std::size_t N>
const char* nameOf(const std::array<const char*, N>& names, Enum value) noexcept {
    const auto index = static_cast<std::size_t>(value);
    return index < N ? names.at(index) : names.front();
}

} // namespace

const char* toString(ControllerKind kind) noexcept {
    return nameOf(kKindNames, kind);
}

bool controllerKindFromString(std::string_view name, ControllerKind& out) noexcept {
    return fromName(kKindNames, name, out);
}

const char* toString(LfoShape shape) noexcept {
    return nameOf(kShapeNames, shape);
}

bool lfoShapeFromString(std::string_view name, LfoShape& out) noexcept {
    return fromName(kShapeNames, name, out);
}

const char* toString(KeyboardInput input) noexcept {
    return nameOf(kInputNames, input);
}

bool keyboardInputFromString(std::string_view name, KeyboardInput& out) noexcept {
    return fromName(kInputNames, name, out);
}

} // namespace adx::project

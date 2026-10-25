// A generator of random *valid* commands against a project as it stands - the
// engine of undo_to_empty_random (test_undo_random.cpp), shared since Phase 7 with
// patch_apply_equals_target, which needs random but realistic edits to diff.
#pragma once

#include <memory>
#include <random>
#include <string>

#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::tests {

/// Picks random *valid* targets out of the project as it currently stands.
///
/// "Valid" matters: a generator that mostly names entities that do not exist would
/// spend its ten thousand commands doing nothing, and the test would pass without
/// having exercised anything.
class Generator {
public:
    Generator(std::mt19937& rng, const adx::project::Project& project)
        : m_rng(rng), m_project(project) {}

    [[nodiscard]] std::unique_ptr<adx::project::Command> next();

    /// Metadata, tempo, markers and channels.
    [[nodiscard]] std::unique_ptr<adx::project::Command> nextSimple(int kind);
    /// Patterns, notes, the playlist and the mixer.
    [[nodiscard]] std::unique_ptr<adx::project::Command> nextStructural(int kind);

private:
    template<class Container> [[nodiscard]] const auto* pick(const Container& items) {
        if (items.empty()) {
            return static_cast<const typename Container::value_type*>(nullptr);
        }
        std::uniform_int_distribution<std::size_t> index(0, items.size() - 1);
        return &items[index(m_rng)];
    }

    [[nodiscard]] int roll(int high) {
        return std::uniform_int_distribution<int>(0, high)(m_rng);
    }
    [[nodiscard]] double unit() {
        return std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
    }
    [[nodiscard]] adx::core::Ticks someTick() {
        return adx::core::Ticks{
            std::uniform_int_distribution<std::int64_t>(0, adx::core::kPpq * 64)(m_rng)};
    }
    [[nodiscard]] std::string someName() {
        return "gen" + std::to_string(std::uniform_int_distribution<int>(0, 1000000)(m_rng));
    }

    std::mt19937& m_rng;
    const adx::project::Project& m_project;
};

} // namespace adx::tests

// adx-thread: main
//
// Writing automation (STATE.md P9-7): while a pass is on and the arrangement rolls, every
// parameter change a hand makes - a plugin editor's gesture, today - is kept as a
// breakpoint at the tick it happened. Stopping the pass commits it as one undo step: a
// pattern holding a lane per parameter touched, placed on a new playlist track at the
// bar the pass began in. The lanes are ordinary automation clips, edited and saved like
// any other.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/project/Automation.h"
#include "engine/project/ParamRef.h"

namespace adx::project {

class Project;
class CommandStack;

class AutomationWriter {
public:
    /// A pass begins at arrangement tick `tick`. Any pass not committed is discarded.
    void begin(std::int64_t tick);
    [[nodiscard]] bool writing() const noexcept {
        return m_writing;
    }
    /// One change. Ignored while not writing, or before the pass began.
    void add(ParamRef target, std::int64_t tick, float value);
    /// Ends the pass at `tick` and adds what it caught to `project`; returns the
    /// pattern's name, or empty when nothing was touched.
    std::string commit(Project& project, CommandStack& stack, std::int64_t tick);
    /// Ends the pass and keeps nothing.
    void discard() noexcept;
    /// Parameters touched so far in this pass.
    [[nodiscard]] std::size_t lanes() const noexcept {
        return m_lanes.size();
    }

private:
    struct Lane {
        ParamRef target;
        std::vector<Breakpoint> points;
    };
    bool m_writing{false};
    std::int64_t m_begin{0};
    std::vector<Lane> m_lanes;
};

} // namespace adx::project

// adx-thread: main
//
// Performance recording (phase_11.md §4.6): what the session played, written back as an
// ordinary arrangement. The audio thread logs every pass of every clip, every stop and
// every rate change at its grid tick (session/PerformanceLog.h); this side gathers the
// log and, on commit, writes it as one undo step:
//
// - each pass of a clip becomes a PlaylistItem of the cell's pattern on the session
//   track's linked playlist track (made, and linked, if the track had none), at the
//   pass's tick, as long as the pass played, from the clip tick it started at (a legato
//   launch's phase is the item's offset), at the clip's gain;
// - a cell whose pattern also holds other channels' notes plays only its own channel's
//   in the session, so it is written as a copy of the pattern holding only those
//   ("<pattern> <channel>"), made once per pattern and channel;
// - each rate change becomes tempo events: from its tick on, the map's tempo times the
//   rate, so the arrangement runs at the tempo the audience heard.
//
// Nothing session-specific is written: the result is patterns, items and tempo events,
// and renders like what was heard - sample for sample where no rate changed and the
// launches fell on whole samples (the grid's bars at most tempos; phase_11.md §10).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/session/PerformanceLog.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::record {

struct PerformanceSummary {
    std::size_t items{0};
    std::size_t tempoEvents{0};
    std::size_t patternsMade{0};
    std::size_t tracksMade{0};
};

class PerformanceRecorder {
public:
    void add(const session::PerformanceEvent& event) {
        m_events.push_back(event);
    }
    void clear() noexcept {
        m_events.clear();
    }
    [[nodiscard]] std::span<const session::PerformanceEvent> events() const noexcept {
        return m_events;
    }

    /// Writes the log into `project` as one undo step. A clip still playing at `end`
    /// (a grid tick; negative for "where its pass would end") is cut there. Nothing is
    /// written, and no undo step made, for an empty log.
    PerformanceSummary commit(project::Project& project, project::CommandStack& stack,
                              std::int64_t end) const;

private:
    std::vector<session::PerformanceEvent> m_events;
};

} // namespace adx::record

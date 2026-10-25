// adx-thread: main
//
// Diagnostics for the editor panel, kept up to date keystroke by keystroke
// (phase_7.md §4.3).
//
// A file reload does not need this - a full parse of a 551-line file is well under a
// millisecond. The editor does, because every keystroke re-runs the diagnostics, and
// the budget is 5 ms on a 10,000-line file, which a full load does not meet.
//
// The scope is deliberately small, because an incremental parser that is clever about
// cross-section references is a bug farm:
//
//   - Only the lines that changed are compared and replaced (a common prefix and
//     suffix of lines; the caller passes the whole new text).
//   - An edit that stays inside one [PATTERN]'s body - no section header touched,
//     before or after - re-parses that pattern against the rest of the file with every
//     *other* pattern's body left out (its lines mapped back afterwards). A pattern's
//     body can only produce diagnostics about itself (its notes, its MINI, its
//     automation, against channels and its own LENGTH), and nothing else in the file
//     depends on what is in it, so the result for its lines is the full parse's
//     result. Every other line keeps the diagnostics it had, moved by the number of
//     lines the edit added or removed.
//   - Anything else - a header, a channel, the mixer, an edit spanning sections - is a
//     full parse. Correct by construction; `incremental_matches_full` checks it.
//
// "Diagnostics" are the parser's and validate()'s together, each at its line (P2-2): a
// routing cycle is underlined at the ROUTE that closes it.
//
// In a file big enough to need this, the patterns are nearly all of it, so the common
// keystroke is the cheap case.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/format/adx/Diagnostics.h"

namespace adx::format {

class IncrementalParse {
public:
    /// A full parse of `text`.
    void reset(std::string_view text);

    /// Brings the diagnostics up to `text`, re-parsing as little as §4.3 allows.
    void update(std::string_view text);

    [[nodiscard]] const DiagnosticList& diagnostics() const noexcept {
        return m_diagnostics;
    }

    /// Whether the last update() was the cheap path, and how long it took. For the
    /// budget test and the editor's status line.
    [[nodiscard]] bool lastWasIncremental() const noexcept {
        return m_incremental;
    }
    [[nodiscard]] double lastMilliseconds() const noexcept {
        return m_milliseconds;
    }

private:
    struct SectionRange {
        std::size_t header{0}; // line index of the header; for the preamble, 0
        std::size_t end{0};    // one past the last line
        bool pattern{false};
        bool preamble{false};
    };

    void full(std::string_view text);
    void indexSections();
    [[nodiscard]] std::size_t sectionOf(std::size_t line) const noexcept;
    void fixOffsets();

    std::vector<std::string> m_lines; // each with its terminator
    std::vector<SectionRange> m_sections;
    DiagnosticList m_diagnostics;
    bool m_incremental{false};
    double m_milliseconds{0.0};
};

/// Splits text into lines, each keeping its terminator. Exposed for tests.
[[nodiscard]] std::vector<std::string> splitLines(std::string_view text);

} // namespace adx::format

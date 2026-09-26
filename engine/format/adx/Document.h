// The lossless concrete syntax tree - the central idea of this phase.
//
// FINAL_PLAN §6 asks for two things that naively conflict:
//
//   Rule 2 - round-trip is lossless and stable... byte-identical.
//   Rule 4 - unknown keys and sections are preserved verbatim.
//
// A parser that builds only a Project cannot satisfy Rule 4, because the Project
// has nowhere to put a key it does not understand. A parser that builds only a
// syntax tree cannot satisfy the rest of the phase. So build both: the Document
// holds every byte, the Parser walks it and marks what it understood, and whatever
// is left is *residue* - reported, carried in the Project, and re-emitted on save.
//
// The contract, and it is testable with a fuzzer:
//
//     Document::parse(text).write() == text
//
// byte for byte, for **any** input, including malformed ones. Everything else in
// engine/format stands on that.
//
// This is also what makes Phase 7's bidirectional sync tractable: the Document is
// the pivot between a text edit and a model edit, and its line spans give the
// editor panel somewhere to anchor a diagnostic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Lexer.h"

namespace adx::format {

/// Index meaning "there is no such line" - used by the preamble, which has no
/// header.
inline constexpr std::size_t kNoLine = static_cast<std::size_t>(-1);

struct Section {
    /// Empty for the preamble: everything before the first section header.
    std::string type;
    std::string name;
    Span span;

    std::size_t headerIndex{kNoLine};
    /// Indices into Document::lines(), in file order, excluding the header.
    std::vector<std::size_t> lineIndices;

    bool claimed{false};

    [[nodiscard]] bool isPreamble() const noexcept {
        return headerIndex == kNoLine;
    }
};

struct ResidueLine {
    /// The original bytes, without the terminator.
    std::string raw;
    std::uint32_t sourceLine{0};

    [[nodiscard]] friend bool operator==(const ResidueLine&, const ResidueLine&) noexcept = default;
};

/// One run of unclaimed text, tagged with where it belongs.
///
/// A block is either a whole unknown section, or the unclaimed lines of a section
/// the parser did understand. Either way the writer knows where to put it back:
/// unknown keys go at the end of their own section, unknown sections after all the
/// known ones, in their original relative order (docs/adx-format-v2.md §9).
struct ResidueBlock {
    std::string sectionType;
    std::string sectionName;
    bool wholeSection{false};
    /// The `[TYPE name]` line, verbatim. Only set when wholeSection is true.
    std::string headerRaw;
    std::vector<ResidueLine> lines;
    std::uint32_t sourceLine{0};

    [[nodiscard]] friend bool operator==(const ResidueBlock&,
                                         const ResidueBlock&) noexcept = default;
};

struct DocumentResidue {
    std::vector<ResidueBlock> blocks;

    [[nodiscard]] bool empty() const noexcept {
        return blocks.empty();
    }

    /// The unclaimed lines belonging to one known section, or nullptr.
    [[nodiscard]] const ResidueBlock* forSection(std::string_view type,
                                                 std::string_view name) const noexcept;

    [[nodiscard]] friend bool operator==(const DocumentResidue&,
                                         const DocumentResidue&) noexcept = default;
};

class Document {
public:
    /// Never fails. A line it cannot classify becomes Kind::Unknown with its bytes
    /// intact, because there is no input for which losing the user's text is the
    /// right answer.
    [[nodiscard]] static Document parse(std::string_view text, DiagnosticList& diagnostics);

    /// Reproduces the input byte for byte.
    [[nodiscard]] std::string write() const;

    [[nodiscard]] std::span<const Section> sections() const noexcept {
        return m_sections;
    }
    [[nodiscard]] std::span<Section> sections() noexcept {
        return m_sections;
    }
    [[nodiscard]] std::span<const Line> lines() const noexcept {
        return m_lines;
    }
    [[nodiscard]] std::span<Line> lines() noexcept {
        return m_lines;
    }

    [[nodiscard]] const std::string& bom() const noexcept {
        return m_bom;
    }

    /// The line terminator the file predominantly used, for a writer creating a new
    /// file from this one. LF when the file had no terminators at all.
    [[nodiscard]] std::string_view dominantTerminator() const noexcept;

    /// Everything the Parser did not claim.
    [[nodiscard]] DocumentResidue residue() const;

private:
    std::string m_bom;
    std::vector<Line> m_lines;
    std::vector<Section> m_sections;
};

} // namespace adx::format

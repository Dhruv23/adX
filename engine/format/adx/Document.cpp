#include "engine/format/adx/Document.h"

#include <algorithm>

namespace adx::format {
namespace {

/// True for a line that carries data rather than presentation. Blank lines and
/// comments are never residue on their own - residue is text the model could not
/// hold, and a comment was never going to be held by the model anyway.
[[nodiscard]] bool isDataLine(const Line& line) noexcept {
    return line.kind != Line::Kind::Blank && line.kind != Line::Kind::Comment;
}

} // namespace

const ResidueBlock* DocumentResidue::forSection(std::string_view type,
                                                std::string_view name) const noexcept {
    for (const ResidueBlock& block : blocks) {
        if (!block.wholeSection && block.sectionType == type && block.sectionName == name) {
            return &block;
        }
    }
    return nullptr;
}

Document Document::parse(std::string_view text, DiagnosticList& diagnostics) {
    Document document;
    document.m_lines = lexLines(text, document.m_bom, diagnostics);

    // The preamble always exists, even when it is empty: it gives every line an
    // owning section, which means residue collection never has a special case.
    Section preamble;
    document.m_sections.push_back(std::move(preamble));

    bool sawSection = false;
    for (std::size_t i = 0; i < document.m_lines.size(); ++i) {
        const Line& line = document.m_lines[i];
        if (line.kind == Line::Kind::SectionHeader) {
            sawSection = true;
            Section section;
            section.type = line.sectionType;
            section.name = line.sectionName;
            section.span = line.span();
            section.headerIndex = i;
            document.m_sections.push_back(std::move(section));
            continue;
        }
        if (!sawSection && isDataLine(line)) {
            diagnostics.add(code::kContentBeforeSection, line.contentSpan(),
                            "content before the first section header");
        }
        document.m_sections.back().lineIndices.push_back(i);
    }

    return document;
}

std::string Document::write() const {
    std::size_t total = m_bom.size();
    for (const Line& line : m_lines) {
        total += line.raw.size() + line.terminator.size();
    }

    std::string out;
    out.reserve(total);
    out += m_bom;
    for (const Line& line : m_lines) {
        out += line.raw;
        out += line.terminator;
    }
    return out;
}

std::string_view Document::dominantTerminator() const noexcept {
    std::size_t crlf = 0;
    std::size_t lf = 0;
    for (const Line& line : m_lines) {
        if (line.terminator == "\r\n") {
            ++crlf;
        } else if (line.terminator == "\n") {
            ++lf;
        }
    }
    return crlf > lf ? "\r\n" : "\n";
}

DocumentResidue Document::residue() const {
    DocumentResidue out;

    for (const Section& section : m_sections) {
        const bool unknownSection = !section.isPreamble() && !section.claimed;

        ResidueBlock block;
        block.sectionType = section.type;
        block.sectionName = section.name;
        block.wholeSection = unknownSection;
        if (unknownSection) {
            block.headerRaw = m_lines[section.headerIndex].raw;
            block.sourceLine = m_lines[section.headerIndex].number;
        }

        for (const std::size_t index : section.lineIndices) {
            const Line& line = m_lines[index];
            if (unknownSection) {
                // An unknown section is preserved whole - comments and blanks
                // included - because those are part of what the author wrote there
                // and nothing in the model will ever account for them.
                block.lines.push_back(ResidueLine{.raw = line.raw, .sourceLine = line.number});
                continue;
            }
            if (line.claimed || !isDataLine(line)) {
                continue;
            }
            if (block.lines.empty()) {
                block.sourceLine = line.number;
            }
            block.lines.push_back(ResidueLine{.raw = line.raw, .sourceLine = line.number});
        }

        if (unknownSection) {
            // Trailing blank lines belong to the gap between sections, not to the
            // section, and re-emitting them would double the blank line the writer
            // puts there itself.
            while (!block.lines.empty() &&
                   block.lines.back().raw.find_first_not_of(" \t") == std::string::npos) {
                block.lines.pop_back();
            }
        }

        if (!block.lines.empty() || unknownSection) {
            out.blocks.push_back(std::move(block));
        }
    }

    std::ranges::stable_sort(out.blocks, [](const ResidueBlock& lhs, const ResidueBlock& rhs) {
        return lhs.sourceLine < rhs.sourceLine;
    });
    return out;
}

} // namespace adx::format

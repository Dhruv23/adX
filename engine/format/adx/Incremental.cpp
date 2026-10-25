// adx-thread: main
#include "engine/format/adx/Incremental.h"

#include <algorithm>
#include <chrono>

#include "engine/format/adx/Parser.h"
#include "engine/project/Project.h"
#include "engine/project/SourceMap.h"
#include "engine/project/Validate.h"
#include "engine/project/commands/CommandStack.h"

namespace adx::format {

namespace {

[[nodiscard]] bool isHorizontalSpace(char c) noexcept {
    return c == ' ' || c == '\t';
}

/// The lexer's rule (Lexer.cpp): a header is a line that starts with '[' at the margin.
[[nodiscard]] bool isHeader(std::string_view line) noexcept {
    return !line.empty() && line.front() == '[';
}

[[nodiscard]] bool isPatternHeader(std::string_view line) noexcept {
    if (!isHeader(line)) {
        return false;
    }
    std::size_t i = 1;
    while (i < line.size() && isHorizontalSpace(line[i])) {
        ++i;
    }
    const std::string_view rest = line.substr(i);
    return rest.starts_with("PATTERN") &&
           (rest.size() == 7 || isHorizontalSpace(rest[7]) || rest[7] == ']');
}

/// The terminator alone: a blank line that keeps the numbering of every line after it.
[[nodiscard]] std::string_view terminatorOf(const std::string& line) noexcept {
    if (line.ends_with("\r\n")) {
        return "\r\n";
    }
    if (line.ends_with('\n') || line.ends_with('\r')) {
        return std::string_view(line).substr(line.size() - 1);
    }
    return {};
}

/// The parser's diagnostics and validate()'s, both at their lines (P2-2): what the
/// editor shows, and what a reload is refused on.
DiagnosticList parsed(const std::string& text) {
    project::Project project;
    project::CommandStack stack;
    project::SourceMap sources;
    DiagnosticList diagnostics;
    ParseOptions options;
    options.groupAsOneStep = true;
    options.sources = &sources;
    load(text, project, stack, diagnostics, options);
    project::ValidationOptions validation;
    validation.sources = &sources;
    static_cast<void>(project::validate(project, diagnostics, validation));
    return diagnostics;
}

} // namespace

std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        // A line ends at LF, or at a CR that no LF follows.
        const bool ends =
            text[i] == '\n' || (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n'));
        if (ends) {
            lines.emplace_back(text.substr(start, i + 1 - start));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        lines.emplace_back(text.substr(start));
    }
    return lines;
}

void IncrementalParse::reset(std::string_view text) {
    const auto started = std::chrono::steady_clock::now();
    full(text);
    m_milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
}

void IncrementalParse::full(std::string_view text) {
    m_lines = splitLines(text);
    indexSections();
    m_diagnostics = parsed(std::string(text));
    m_diagnostics.sortByPosition();
    fixOffsets();
    m_incremental = false;
}

void IncrementalParse::indexSections() {
    m_sections.clear();
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        if (isHeader(m_lines[i])) {
            if (!m_sections.empty()) {
                m_sections.back().end = i;
            } else if (i > 0) {
                m_sections.push_back(SectionRange{.header = 0, .end = i, .preamble = true});
            }
            m_sections.push_back(SectionRange{
                .header = i, .end = m_lines.size(), .pattern = isPatternHeader(m_lines[i])});
        }
    }
    if (m_sections.empty()) {
        m_sections.push_back(SectionRange{.header = 0, .end = m_lines.size(), .preamble = true});
    }
}

std::size_t IncrementalParse::sectionOf(std::size_t line) const noexcept {
    for (std::size_t s = 0; s < m_sections.size(); ++s) {
        if (line >= m_sections[s].header && line < m_sections[s].end) {
            return s;
        }
    }
    return m_sections.size();
}

void IncrementalParse::fixOffsets() {
    // Spans from a parse of the reduced text carry that text's byte offsets; the editor
    // wants this text's.
    std::vector<std::uint32_t> starts;
    starts.reserve(m_lines.size());
    std::uint32_t offset = 0;
    for (const std::string& line : m_lines) {
        starts.push_back(offset);
        offset += static_cast<std::uint32_t>(line.size());
    }
    DiagnosticList fixed;
    for (const Diagnostic& d : m_diagnostics.all()) {
        Span span = d.span;
        if (span.line >= 1 && span.line <= starts.size()) {
            span.byteOffset = starts[span.line - 1] + span.column - 1;
        }
        fixed.add(d.code, span, d.message, d.hint);
    }
    m_diagnostics = std::move(fixed);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - one decision, then one splice
void IncrementalParse::update(std::string_view text) {
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&] {
        m_milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
    };
    std::vector<std::string> lines = splitLines(text);
    if (m_lines.empty() || lines.empty()) {
        full(text);
        finish();
        return;
    }

    // The changed range: [prefix, oldEnd) of the old lines became [prefix, newEnd).
    const std::size_t common = std::min(lines.size(), m_lines.size());
    std::size_t prefix = 0;
    while (prefix < common && lines[prefix] == m_lines[prefix]) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < common - prefix &&
           lines[lines.size() - 1 - suffix] == m_lines[m_lines.size() - 1 - suffix]) {
        ++suffix;
    }
    const std::size_t oldEnd = m_lines.size() - suffix;
    const std::size_t newEnd = lines.size() - suffix;
    if (prefix == oldEnd && prefix == newEnd) {
        m_incremental = true;
        finish();
        return;
    }

    // Eligible only inside one pattern's body, with no header on either side of the
    // edit. A pure insertion at a section's end belongs to the section before.
    const std::size_t anchor = (oldEnd == prefix && prefix > 0) ? prefix - 1 : prefix;
    const std::size_t index = sectionOf(anchor);
    bool eligible = index < m_sections.size() && m_sections[index].pattern &&
                    prefix > m_sections[index].header && oldEnd <= m_sections[index].end;
    for (std::size_t i = prefix; eligible && i < oldEnd; ++i) {
        eligible = !isHeader(m_lines[i]);
    }
    for (std::size_t i = prefix; eligible && i < newEnd; ++i) {
        eligible = !isHeader(lines[i]);
    }
    if (!eligible) {
        full(text);
        finish();
        return;
    }

    const auto delta = static_cast<std::ptrdiff_t>(newEnd) - static_cast<std::ptrdiff_t>(oldEnd);
    const SectionRange before = m_sections[index];
    m_lines = std::move(lines);
    for (std::size_t s = index + 1; s < m_sections.size(); ++s) {
        m_sections[s].header =
            static_cast<std::size_t>(static_cast<std::ptrdiff_t>(m_sections[s].header) + delta);
        m_sections[s].end =
            static_cast<std::size_t>(static_cast<std::ptrdiff_t>(m_sections[s].end) + delta);
    }
    m_sections[index].end =
        static_cast<std::size_t>(static_cast<std::ptrdiff_t>(before.end) + delta);
    const SectionRange& edited = m_sections[index];

    // The rest of the file with every other pattern's body blanked: what the edited
    // pattern is checked against.
    std::string reduced;
    reduced.reserve(m_lines.size() * 2);
    for (std::size_t s = 0; s < m_sections.size(); ++s) {
        const SectionRange& section = m_sections[s];
        const bool keep = s == index || !section.pattern;
        for (std::size_t i = section.header; i < section.end; ++i) {
            if (keep || i == section.header) {
                reduced += m_lines[i];
            } else {
                reduced += terminatorOf(m_lines[i]);
            }
        }
    }
    const DiagnosticList fresh = parsed(reduced);

    DiagnosticList merged;
    for (const Diagnostic& d : m_diagnostics.all()) {
        const std::size_t line = d.span.line;
        if (line == 0) {
            merged.add(d.code, d.span, d.message, d.hint);
            continue;
        }
        const std::size_t at = line - 1;
        if (at < before.header) {
            merged.add(d.code, d.span, d.message, d.hint);
        } else if (at >= before.end) {
            Span span = d.span;
            span.line = static_cast<std::uint32_t>(static_cast<std::ptrdiff_t>(line) + delta);
            merged.add(d.code, span, d.message, d.hint);
        }
    }
    for (const Diagnostic& d : fresh.all()) {
        const std::size_t line = d.span.line;
        if (line >= 1 && line - 1 >= edited.header && line - 1 < edited.end) {
            merged.add(d.code, d.span, d.message, d.hint);
        }
    }
    merged.sortByPosition();
    m_diagnostics = std::move(merged);
    fixOffsets();
    m_incremental = true;
    finish();
}

} // namespace adx::format

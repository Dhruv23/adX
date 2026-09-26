#include "engine/format/adx/Diagnostics.h"

#include <algorithm>
#include <array>

#include "engine/core/Config.h"

namespace adx::format {
namespace {

/// Every code this implementation can emit, with the severity it is always emitted
/// at. Must stay in sync with docs/adx-format-v2.md §10 - which is not a matter of
/// discipline, because diagnostics_all_codes_documented reads both and fails when
/// they disagree.
// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table:
// one row per line, columns aligned by position. Designating every field would
// triple its width and bury the values the table exists to show.
constexpr auto kCodes = std::to_array<DiagnosticCode>({
    {code::kUnterminatedString, Severity::Error, "Unterminated quoted string."},
    {code::kBadEscape, Severity::Warning, "Unrecognised escape sequence."},
    {code::kUnclosedSectionHeader, Severity::Error, "Section header is missing its closing ']'."},
    {code::kEmptySectionType, Severity::Error, "Section header has an empty type."},
    {code::kContentBeforeSection, Severity::Warning, "Content before the first section header."},
    {code::kControlCharacter, Severity::Warning, "Control character in text."},
    {code::kMalformedNumber, Severity::Error, "Malformed number or boolean."},
    {code::kMalformedPosition, Severity::Error, "Malformed position or duration."},
    {code::kMalformedKeyValue, Severity::Error, "Malformed key=value pair."},
    {code::kEmptyBlock, Severity::Warning, "Block opener with no indented body."},
    {code::kWrongFieldCount, Severity::Error, "Wrong number of fields on a positional line."},

    {code::kUnknownKey, Severity::Warning, "Unknown key."},
    {code::kUnknownSection, Severity::Warning, "Unknown section."},
    {code::kDuplicateKey, Severity::Warning, "Duplicate key in one section."},
    {code::kUnknownParameter, Severity::Warning, "Unknown parameter name."},
    {code::kUnknownCurve, Severity::Warning, "Unknown curve name."},
    {code::kUnknownInlineKey, Severity::Warning, "Unknown inline key on a positional line."},

    {code::kValueOutOfRange, Severity::Warning, "Value out of range; clamped."},
    {code::kMissingRequiredKey, Severity::Error, "A required key is missing."},
    {code::kDuplicateName, Severity::Error, "Duplicate entity name."},
    {code::kNoteOutsidePattern, Severity::Warning, "Note starts outside its pattern's length."},
    {code::kNegativeDuration, Severity::Error, "Negative duration."},
    {code::kBadMeterDenominator, Severity::Warning, "Unsupported meter denominator."},
    {code::kBreakpointsUnsorted, Severity::Warning, "Breakpoints were out of order."},
    {code::kBadNoteName, Severity::Error, "Not a note name."},
    {code::kFutureVersion, Severity::Warning, "ADX_VERSION is newer than this build."},
    {code::kDuplicateId, Severity::Error, "Duplicate entity id."},
    {code::kTempoOutOfRange, Severity::Warning, "Tempo outside [1, 999]; clamped."},
    {code::kBadNameCharacter, Severity::Error, "Entity name contains a forbidden character."},
    {code::kDuplicateNoteClip, Severity::Error, "Two note clips for one channel in one pattern."},
    {code::kZeroId, Severity::Error, "Entity id is zero, which is the null id."},

    {code::kUnresolvedParamPath, Severity::Error, "Parameter path does not resolve."},
    {code::kUnknownChannelRef, Severity::Error, "Reference to an unknown channel."},
    {code::kUnknownInsertRef, Severity::Error, "Reference to an unknown insert."},
    {code::kUnknownPatternRef, Severity::Error, "Reference to an unknown pattern."},
    {code::kRoutingCycle, Severity::Error, "Routing cycle."},
    {code::kMissingSampleFile, Severity::Warning, "Referenced sample file does not exist."},
    {code::kMalformedReference, Severity::Error, "Malformed reference."},

    {code::kV1UnresolvedAutomation, Severity::Warning, "A v1 automation target did not resolve."},
    {code::kV1TrackExpanded, Severity::Info, "A v1 [TRACK] expanded into four entities."},
    {code::kV1CurveApproximated, Severity::Info, "A v1 exp curve was approximated."},
    {code::kV1SendCreatedBus, Severity::Info, "A v1 SEND= created an aux insert."},
    {code::kV1VelocityRescaled, Severity::Info, "Velocities were rescaled to 0..127."},
    {code::kV1SidechainMigrated, Severity::Info, "SIDECHAIN= became a Ducker slot and a route."},
    {code::kV1MasterFxMigrated, Severity::Info, "v1 master FX became master insert slots."},
    {code::kV1TupleNamed, Severity::Info, "A v1 positional tuple became named parameters."},
    {code::kV1NameSuffixed, Severity::Warning, "A v1 entity name was not unique and was suffixed."},
    {code::kV1ClipSecondsConverted, Severity::Info, "A v1 CLIP start in seconds became ticks."},
    {code::kV1LoopEnabled, Severity::Info, "v1 LOOP= was present, so the loop is enabled."},
    {code::kV1Unrecognised, Severity::Warning, "A v1 line was not recognised; kept as residue."},
});
// NOLINTEND(modernize-use-designated-initializers)

} // namespace

const char* toString(Severity severity) noexcept {
    switch (severity) {
    case Severity::Error:
        return "error";
    case Severity::Warning:
        return "warning";
    case Severity::Info:
        return "info";
    }
    return "error";
}

std::string Diagnostic::codeString() const {
    // Fixed width, zero padded, so codes line up in a column when printed and sort
    // lexicographically in the same order they sort numerically.
    std::string out = "ADX0000";
    std::uint16_t remaining = code;
    for (std::size_t i = 0; i < 4; ++i) {
        out[6 - i] = static_cast<char>('0' + (remaining % 10));
        remaining /= 10;
    }
    return out;
}

std::span<const DiagnosticCode> allDiagnosticCodes() noexcept {
    return kCodes;
}

const DiagnosticCode* findDiagnosticCode(std::uint16_t code) noexcept {
    const auto match = std::ranges::find(kCodes, code, &DiagnosticCode::code);
    return match == kCodes.end() ? nullptr : &*match;
}

void DiagnosticList::add(std::uint16_t code, Span span, std::string message, std::string hint) {
    const DiagnosticCode* row = findDiagnosticCode(code);
    // A code with no row is a programming error, not a malformed file: the table is
    // what the documentation check reads, so emitting a code that is not in it would
    // produce a diagnostic nothing documents.
    ADX_ASSERT(row != nullptr);
    m_items.push_back(Diagnostic{.severity = row != nullptr ? row->severity : Severity::Error,
                                 .code = code,
                                 .span = span,
                                 .message = std::move(message),
                                 .hint = std::move(hint)});
}

void DiagnosticList::append(const DiagnosticList& other) {
    m_items.insert(m_items.end(), other.m_items.begin(), other.m_items.end());
}

bool DiagnosticList::hasErrors() const noexcept {
    return std::ranges::any_of(
        m_items, [](const Diagnostic& item) { return item.severity == Severity::Error; });
}

std::size_t DiagnosticList::count(Severity severity) const noexcept {
    return static_cast<std::size_t>(std::ranges::count(m_items, severity, &Diagnostic::severity));
}

void DiagnosticList::sortByPosition() {
    std::ranges::stable_sort(m_items, [](const Diagnostic& lhs, const Diagnostic& rhs) {
        if (lhs.span.line != rhs.span.line) {
            return lhs.span.line < rhs.span.line;
        }
        if (lhs.span.column != rhs.span.column) {
            return lhs.span.column < rhs.span.column;
        }
        return lhs.code < rhs.code;
    });
}

} // namespace adx::format

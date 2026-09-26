// What the parser says when something is wrong, and where exactly it is.
//
// Iteration one called std::stof inside try/catch per token. std::exception::what()
// carries no position, so every diagnostic degraded to "Malformed X on line N" -
// no column, no length, nothing an editor could underline (FINAL_PLAN §3.3.11).
//
// A Diagnostic here carries line, column *and* length, which is what Phase 7's
// editor panel needs to draw a squiggle under the three characters that are wrong
// rather than highlighting the line.
//
// Every code this file can emit is documented in docs/adx-format-v2.md §10. A code
// that is not is a CI failure - see the table at the bottom and the test that reads
// it.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adx::format {

/// A range of characters in the source text.
///
/// Line and column are one-based, because that is what editors and compilers use
/// and a zero-based column in a diagnostic is a bug report waiting to happen.
/// `byteOffset` is zero-based and absolute, for callers that index the buffer.
struct Span {
    std::uint32_t line{1};
    std::uint32_t column{1};
    std::uint32_t length{0};
    std::uint32_t byteOffset{0};

    [[nodiscard]] friend bool operator==(const Span&, const Span&) noexcept = default;
};

enum class Severity : std::uint8_t { Error, Warning, Info };

[[nodiscard]] const char* toString(Severity severity) noexcept;

struct Diagnostic {
    Severity severity{Severity::Error};
    /// ADX0001..ADX4999, as a plain number. Stable across releases.
    std::uint16_t code{0};
    Span span;
    std::string message;
    /// Optional. "did you mean RESFILTER?" - the half that turns a diagnostic into
    /// a fix.
    std::string hint;

    /// "ADX1001", for printing and for the documentation check.
    [[nodiscard]] std::string codeString() const;
};

/// Every code, in one table.
///
/// The single source of truth for "what can this implementation emit", read by
/// diagnostics_all_codes_documented and by the CLI's `--explain`. Adding a code
/// without adding a row here means the code cannot be emitted, because the emit
/// helpers take a row.
struct DiagnosticCode {
    std::uint16_t code{0};
    Severity severity{Severity::Error};
    /// One line, matching the summary column in docs/adx-format-v2.md §10.
    std::string_view summary;
};

namespace code {
// 0xxx - lexical.
inline constexpr std::uint16_t kUnterminatedString = 1;
inline constexpr std::uint16_t kBadEscape = 2;
inline constexpr std::uint16_t kUnclosedSectionHeader = 3;
inline constexpr std::uint16_t kEmptySectionType = 4;
inline constexpr std::uint16_t kContentBeforeSection = 5;
inline constexpr std::uint16_t kControlCharacter = 6;
inline constexpr std::uint16_t kMalformedNumber = 7;
inline constexpr std::uint16_t kMalformedPosition = 8;
inline constexpr std::uint16_t kMalformedKeyValue = 9;
inline constexpr std::uint16_t kEmptyBlock = 10;
inline constexpr std::uint16_t kWrongFieldCount = 11;

// 1xxx - unknown but tolerated.
inline constexpr std::uint16_t kUnknownKey = 1001;
inline constexpr std::uint16_t kUnknownSection = 1002;
inline constexpr std::uint16_t kDuplicateKey = 1003;
inline constexpr std::uint16_t kUnknownParameter = 1004;
inline constexpr std::uint16_t kUnknownCurve = 1005;
inline constexpr std::uint16_t kUnknownInlineKey = 1006;

// 2xxx - semantic.
inline constexpr std::uint16_t kValueOutOfRange = 2001;
inline constexpr std::uint16_t kMissingRequiredKey = 2002;
inline constexpr std::uint16_t kDuplicateName = 2003;
inline constexpr std::uint16_t kNoteOutsidePattern = 2004;
inline constexpr std::uint16_t kNegativeDuration = 2005;
inline constexpr std::uint16_t kBadMeterDenominator = 2006;
inline constexpr std::uint16_t kBreakpointsUnsorted = 2007;
inline constexpr std::uint16_t kBadNoteName = 2008;
inline constexpr std::uint16_t kFutureVersion = 2009;
inline constexpr std::uint16_t kDuplicateId = 2010;
inline constexpr std::uint16_t kTempoOutOfRange = 2011;
inline constexpr std::uint16_t kBadNameCharacter = 2012;
inline constexpr std::uint16_t kDuplicateNoteClip = 2013;
inline constexpr std::uint16_t kZeroId = 2014;

// 3xxx - reference resolution.
inline constexpr std::uint16_t kUnresolvedParamPath = 3001;
inline constexpr std::uint16_t kUnknownChannelRef = 3002;
inline constexpr std::uint16_t kUnknownInsertRef = 3003;
inline constexpr std::uint16_t kUnknownPatternRef = 3004;
inline constexpr std::uint16_t kRoutingCycle = 3005;
inline constexpr std::uint16_t kMissingSampleFile = 3006;
inline constexpr std::uint16_t kMalformedReference = 3007;

// 4xxx - v1 migration.
inline constexpr std::uint16_t kV1UnresolvedAutomation = 4001;
inline constexpr std::uint16_t kV1TrackExpanded = 4002;
inline constexpr std::uint16_t kV1CurveApproximated = 4003;
inline constexpr std::uint16_t kV1SendCreatedBus = 4004;
inline constexpr std::uint16_t kV1VelocityRescaled = 4005;
inline constexpr std::uint16_t kV1SidechainMigrated = 4006;
inline constexpr std::uint16_t kV1MasterFxMigrated = 4007;
inline constexpr std::uint16_t kV1TupleNamed = 4008;
inline constexpr std::uint16_t kV1NameSuffixed = 4009;
inline constexpr std::uint16_t kV1ClipSecondsConverted = 4010;
inline constexpr std::uint16_t kV1LoopEnabled = 4011;
inline constexpr std::uint16_t kV1Unrecognised = 4012;
} // namespace code

/// The full table. Sorted by code.
[[nodiscard]] std::span<const DiagnosticCode> allDiagnosticCodes() noexcept;

/// The row for `code`, or nullptr. Emitting a diagnostic goes through this, so a
/// code with no row cannot be emitted.
[[nodiscard]] const DiagnosticCode* findDiagnosticCode(std::uint16_t code) noexcept;

class DiagnosticList {
public:
    /// Records `code` at `span`. The severity comes from the table, so one code
    /// never has two severities in two places.
    void add(std::uint16_t code, Span span, std::string message, std::string hint = {});

    void append(const DiagnosticList& other);

    [[nodiscard]] std::span<const Diagnostic> all() const noexcept {
        return m_items;
    }
    [[nodiscard]] bool empty() const noexcept {
        return m_items.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return m_items.size();
    }
    [[nodiscard]] bool hasErrors() const noexcept;
    [[nodiscard]] std::size_t count(Severity severity) const noexcept;

    /// Sorts by position, then code. The CLI prints them in file order, which is
    /// the order a person reads them in, not the order the parser happened to
    /// notice them.
    void sortByPosition();

    void clear() noexcept {
        m_items.clear();
    }

private:
    std::vector<Diagnostic> m_items;
};

} // namespace adx::format

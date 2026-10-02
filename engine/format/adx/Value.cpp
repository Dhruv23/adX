#include "engine/format/adx/Value.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace adx::format {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// Splits `text` on `delimiter` into at most `parts.size()` pieces, returning how
/// many there were. More than that is a failure, reported by the caller as a field
/// count problem rather than a number problem.
template<std::size_t N>
[[nodiscard]] std::size_t split(std::string_view text, char delimiter,
                                std::array<std::string_view, N>& parts) noexcept {
    std::size_t count = 0;
    std::size_t start = 0;
    while (count < N) {
        const std::size_t at = text.find(delimiter, start);
        if (at == std::string_view::npos) {
            parts.at(count) = text.substr(start);
            ++count;
            return count;
        }
        parts.at(count) = text.substr(start, at - start);
        ++count;
        start = at + 1;
    }
    // Ran out of room: signal by returning N+1, which no caller accepts.
    return N + 1;
}

} // namespace

Span subSpan(Span span, std::uint32_t offset, std::uint32_t length) noexcept {
    Span out = span;
    out.column += offset;
    out.byteOffset += offset;
    out.length = length;
    return out;
}

bool parseDouble(std::string_view text, Span span, DiagnosticList& diagnostics, double& out) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        diagnostics.add(code::kMalformedNumber, span, "expected a number");
        return false;
    }
    const char* begin = trimmed.data();
    const char* end = begin + trimmed.size();
    double value = 0.0;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        const auto offset = static_cast<std::uint32_t>(result.ptr - begin);
        diagnostics.add(code::kMalformedNumber,
                        subSpan(span, offset, static_cast<std::uint32_t>(trimmed.size()) - offset),
                        "not a number: '" + std::string(trimmed) + "'");
        return false;
    }
    if (!std::isfinite(value)) {
        diagnostics.add(code::kMalformedNumber, span, "number is not finite");
        return false;
    }
    out = value;
    return true;
}

bool parseInt64(std::string_view text, Span span, DiagnosticList& diagnostics, std::int64_t& out) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        diagnostics.add(code::kMalformedNumber, span, "expected an integer");
        return false;
    }
    const char* begin = trimmed.data();
    const char* end = begin + trimmed.size();
    std::int64_t value = 0;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        const auto offset = static_cast<std::uint32_t>(result.ptr - begin);
        diagnostics.add(code::kMalformedNumber,
                        subSpan(span, offset, static_cast<std::uint32_t>(trimmed.size()) - offset),
                        "not an integer: '" + std::string(trimmed) + "'");
        return false;
    }
    out = value;
    return true;
}

bool parseBool(std::string_view text, Span span, DiagnosticList& diagnostics, bool& out) {
    const std::string_view trimmed = trim(text);
    if (trimmed == "yes") {
        out = true;
        return true;
    }
    if (trimmed == "no") {
        out = false;
        return true;
    }
    diagnostics.add(code::kMalformedNumber, span,
                    "expected yes or no, got '" + std::string(trimmed) + "'");
    return false;
}

bool parsePosition(std::string_view text, const core::TempoMap& tempo, Span span,
                   DiagnosticList& diagnostics, core::Ticks& out) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        diagnostics.add(code::kMalformedPosition, span, "expected a position");
        return false;
    }

    if (trimmed.find(':') == std::string_view::npos) {
        // Decimal beats - quarter notes from zero. Kept for hand-authored files and
        // for v1, where every position was a float beat.
        double beats = 0.0;
        if (!parseDouble(trimmed, span, diagnostics, beats)) {
            return false;
        }
        out = core::Ticks{std::llround(beats * static_cast<double>(core::kPpq))};
        return true;
    }

    std::array<std::string_view, 3> parts{};
    const std::size_t count = split(trimmed, ':', parts);
    if (count < 2 || count > 3) {
        diagnostics.add(code::kMalformedPosition, span,
                        "expected bar:beat:tick, got '" + std::string(trimmed) + "'");
        return false;
    }

    DiagnosticList local;
    core::BarBeatTick position;
    std::uint32_t offset = 0;
    const auto field = [&](std::size_t index, std::int64_t& target) {
        const auto fieldSpan =
            subSpan(span, offset, static_cast<std::uint32_t>(parts.at(index).size()));
        offset += static_cast<std::uint32_t>(parts.at(index).size()) + 1;
        return parseInt64(parts.at(index), fieldSpan, local, target);
    };

    bool ok = field(0, position.bar);
    ok = field(1, position.beat) && ok;
    if (count == 3) {
        ok = field(2, position.tick) && ok;
    }
    if (!ok) {
        // One diagnostic for the position, not three for its fields: the user wrote
        // one token and needs to be told that token is wrong.
        diagnostics.add(code::kMalformedPosition, span,
                        "malformed position '" + std::string(trimmed) + "'");
        return false;
    }
    if (position.beat < 0 || position.tick < 0) {
        diagnostics.add(code::kMalformedPosition, span,
                        "beat and tick may not be negative in '" + std::string(trimmed) + "'");
        return false;
    }

    out = tempo.fromBarBeat(position);
    return true;
}

bool parseDuration(std::string_view text, const core::TempoMap& tempo, Span span,
                   DiagnosticList& diagnostics, core::Ticks& out) {
    core::Ticks value;
    if (!parsePosition(text, tempo, span, diagnostics, value)) {
        return false;
    }
    if (value.value < 0) {
        diagnostics.add(code::kNegativeDuration, span, "duration may not be negative");
        return false;
    }
    out = value;
    return true;
}

bool parseCurve(std::string_view text, Span span, DiagnosticList& diagnostics, core::Curve& out) {
    const std::string_view trimmed = trim(text);
    std::string_view head = trimmed;
    std::string_view args;
    if (const std::size_t open = trimmed.find('('); open != std::string_view::npos) {
        if (trimmed.back() != ')') {
            diagnostics.add(code::kUnknownCurve, span,
                            "curve '" + std::string(trimmed) + "' is missing its closing ')'");
            return false;
        }
        head = trimmed.substr(0, open);
        args = trimmed.substr(open + 1, trimmed.size() - open - 2);
    }

    core::CurveKind kind{};
    if (!core::curveKindFromString(head.data(), head.size(), kind)) {
        diagnostics.add(code::kUnknownCurve, span,
                        "unknown curve '" + std::string(head) + "'; using linear",
                        "one of linear, exponential, logarithmic, step, smooth, bezier, hold");
        return false;
    }

    core::Curve curve;
    curve.kind = kind;
    if (!args.empty()) {
        std::array<std::string_view, 4> parts{};
        const std::size_t count = split(args, ',', parts);
        if (kind == core::CurveKind::Bezier) {
            if (count != 4) {
                diagnostics.add(code::kWrongFieldCount, span,
                                "bezier takes four handle coordinates");
                return false;
            }
            std::array<double, 4> values{};
            for (std::size_t i = 0; i < 4; ++i) {
                if (!parseDouble(parts.at(i), span, diagnostics, values.at(i))) {
                    return false;
                }
            }
            curve.c1x = static_cast<float>(values[0]);
            curve.c1y = static_cast<float>(values[1]);
            curve.c2x = static_cast<float>(values[2]);
            curve.c2y = static_cast<float>(values[3]);
        } else {
            if (count != 1) {
                diagnostics.add(code::kWrongFieldCount, span, "this curve takes one tension");
                return false;
            }
            double tension = 0.0;
            if (!parseDouble(parts.at(0), span, diagnostics, tension)) {
                return false;
            }
            curve.tension = static_cast<float>(std::clamp(tension, -1.0, 1.0));
        }
    }

    out = curve;
    return true;
}

bool parseFraction(std::string_view text, Span span, DiagnosticList& diagnostics,
                   core::Rational& out) {
    const std::string_view trimmed = trim(text);
    std::array<std::string_view, 2> parts{};
    const std::size_t count = split(trimmed, '/', parts);
    if (count != 2) {
        diagnostics.add(code::kMalformedNumber, span,
                        "expected a fraction like 1/16, got '" + std::string(trimmed) + "'");
        return false;
    }
    std::int64_t numerator = 0;
    std::int64_t denominator = 0;
    if (!parseInt64(parts[0], span, diagnostics, numerator) ||
        !parseInt64(parts[1], span, diagnostics, denominator)) {
        return false;
    }
    if (denominator == 0) {
        diagnostics.add(code::kMalformedNumber, span, "a fraction may not have a zero denominator");
        return false;
    }
    out = core::Rational::make(numerator, denominator);
    return true;
}

bool parseColor(std::string_view text, Span span, DiagnosticList& diagnostics,
                project::Color& out) {
    std::string_view trimmed = trim(text);
    if (trimmed.starts_with('#')) {
        trimmed.remove_prefix(1);
    }
    if (trimmed.size() != 6) {
        diagnostics.add(code::kMalformedNumber, span,
                        "expected a colour like #rrggbb, got '" + std::string(trimmed) + "'");
        return false;
    }
    std::array<std::uint8_t, 3> channels{};
    for (std::size_t i = 0; i < 3; ++i) {
        const char* begin = trimmed.data() + (i * 2);
        unsigned int value = 0;
        const auto result = std::from_chars(begin, begin + 2, value, 16);
        if (result.ec != std::errc{} || result.ptr != begin + 2) {
            diagnostics.add(code::kMalformedNumber,
                            subSpan(span, static_cast<std::uint32_t>(i * 2), 2), "not a hex pair");
            return false;
        }
        channels.at(i) = static_cast<std::uint8_t>(value);
    }
    out = project::Color{.red = channels[0], .green = channels[1], .blue = channels[2]};
    return true;
}

std::string formatDouble(double value) {
    // to_chars' shortest round-trip form: the smallest text that reads back as these
    // exact bits. Anything else either loses precision or writes 0.10000000149011612
    // where the user typed 0.1, and both make a diff unreadable.
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        return "0";
    }
    return {buffer.data(), result.ptr};
}

std::string formatFloat(float value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        return "0";
    }
    return {buffer.data(), result.ptr};
}

std::string formatPosition(core::Ticks at, const core::TempoMap& tempo) {
    const core::BarBeatTick position = tempo.toBarBeat(at);
    std::string out = std::to_string(position.bar);
    out += ':';
    out += std::to_string(position.beat);
    out += ':';
    out += std::to_string(position.tick);
    return out;
}

std::string formatDuration(core::Ticks length, const core::TempoMap& tempo) {
    return formatPosition(length, tempo);
}

std::string formatCurve(const core::Curve& curve) {
    std::string out = core::toString(curve.kind);
    if (curve.kind == core::CurveKind::Bezier) {
        out += '(';
        out += formatFloat(curve.c1x);
        out += ',';
        out += formatFloat(curve.c1y);
        out += ',';
        out += formatFloat(curve.c2x);
        out += ',';
        out += formatFloat(curve.c2y);
        out += ')';
        return out;
    }
    const bool shaped =
        curve.kind == core::CurveKind::Exponential || curve.kind == core::CurveKind::Logarithmic;
    if (shaped && curve.tension != 0.0F) {
        out += '(';
        out += formatFloat(curve.tension);
        out += ')';
    }
    return out;
}

std::string formatFraction(core::Rational value) {
    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

std::string formatColor(project::Color color) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out = "#______";
    const std::array<std::uint8_t, 3> channels{color.red, color.green, color.blue};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        out[1 + (i * 2)] = kHex[channels.at(i) >> 4U];
        out[2 + (i * 2)] = kHex[channels.at(i) & 0x0FU];
    }
    return out;
}

} // namespace adx::format

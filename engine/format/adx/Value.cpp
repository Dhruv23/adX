#include "engine/format/adx/Value.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

#include "engine/format/adx/NoteName.h"

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

// --- note extensions -------------------------------------------------------

bool parsePitchAmount(std::string_view text, Span span, DiagnosticList& diagnostics,
                      std::int16_t& outCents) {
    double scale = 0.0;
    std::string_view number = text;
    if (number.ends_with("st")) {
        scale = 100.0;
        number.remove_suffix(2);
    } else if (number.ends_with('c')) {
        scale = 1.0;
        number.remove_suffix(1);
    } else {
        diagnostics.add(code::kMalformedKeyValue, span,
                        "a pitch amount needs a unit: '3st' for semitones or '-50c' for cents");
        return false;
    }
    double value = 0.0;
    if (!parseDouble(number, subSpan(span, 0, static_cast<std::uint32_t>(number.size())),
                     diagnostics, value)) {
        return false;
    }
    const double cents = value * scale;
    if (cents < -32768.0 || cents > 32767.0) {
        diagnostics.add(code::kValueOutOfRange, span, "pitch amount is outside +-327 semitones");
        return false;
    }
    outCents = static_cast<std::int16_t>(cents < 0.0 ? cents - 0.5 : cents + 0.5);
    return true;
}

namespace {

/// Splits `text` at the first `separator` into the part before and after it.
[[nodiscard]] bool splitAt(std::string_view text, char separator, std::string_view& before,
                           std::string_view& after) noexcept {
    const std::size_t at = text.find(separator);
    if (at == std::string_view::npos) {
        return false;
    }
    before = text.substr(0, at);
    after = text.substr(at + 1);
    return true;
}

/// Peels an optional `~curve` suffix off `text`.
[[nodiscard]] bool takeCurve(std::string_view& text, Span span, DiagnosticList& diagnostics,
                             core::Curve& out) {
    const std::size_t tilde = text.find('~');
    if (tilde == std::string_view::npos) {
        return true;
    }
    const std::string_view curve = text.substr(tilde + 1);
    text = text.substr(0, tilde);
    return parseCurve(curve, span, diagnostics, out);
}

[[nodiscard]] bool isDefaultCurve(const core::Curve& curve) noexcept {
    return curve.kind == core::CurveKind::Linear && curve.tension == 0.0F;
}

} // namespace

bool parseSlide(std::string_view text, const core::TempoMap& tempo, Span span,
                DiagnosticList& diagnostics, project::NoteSlide& out) {
    project::NoteSlide slide;
    std::string_view rest = text;
    if (!takeCurve(rest, span, diagnostics, slide.curve)) {
        return false;
    }
    std::string_view amount;
    std::string_view timing;
    std::string_view start;
    std::string_view length;
    if (!splitAt(rest, '@', amount, timing) || !splitAt(timing, '+', start, length)) {
        diagnostics.add(code::kMalformedKeyValue, span,
                        "a slide is '<amount>@<start>+<length>[~curve]', e.g. 3st@0:1:0+0:0:960");
        return false;
    }
    if (!parsePitchAmount(amount, span, diagnostics, slide.targetCents) ||
        !parseDuration(start, tempo, span, diagnostics, slide.start) ||
        !parseDuration(length, tempo, span, diagnostics, slide.length)) {
        return false;
    }
    out = slide;
    return true;
}

bool parseBend(std::string_view text, const core::TempoMap& tempo, Span span,
               DiagnosticList& diagnostics, std::vector<project::PitchPoint>& out) {
    std::vector<project::PitchPoint> points;
    std::string_view rest = text;
    while (!rest.empty()) {
        const std::size_t bar = rest.find('|');
        std::string_view item = rest.substr(0, bar);
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);

        project::PitchPoint point;
        if (!takeCurve(item, span, diagnostics, point.curve)) {
            return false;
        }
        std::string_view amount;
        std::string_view at;
        if (!splitAt(item, '@', amount, at)) {
            diagnostics.add(code::kMalformedKeyValue, span,
                            "a bend point is '<amount>@<at>[~curve]', joined with '|'");
            return false;
        }
        if (!parsePitchAmount(amount, span, diagnostics, point.cents) ||
            !parseDuration(at, tempo, span, diagnostics, point.at)) {
            return false;
        }
        points.push_back(point);
    }
    std::ranges::stable_sort(points, {}, [](const project::PitchPoint& p) { return p.at; });
    out = std::move(points);
    return true;
}

std::string formatPitchAmount(std::int16_t cents) {
    if (cents % 100 == 0) {
        return std::to_string(cents / 100) + "st";
    }
    return std::to_string(cents) + "c";
}

std::string formatSlide(const project::NoteSlide& slide, const core::TempoMap& tempo) {
    std::string out = formatPitchAmount(slide.targetCents);
    out += '@';
    out += formatDuration(slide.start, tempo);
    out += '+';
    out += formatDuration(slide.length, tempo);
    if (!isDefaultCurve(slide.curve)) {
        out += '~';
        out += formatCurve(slide.curve);
    }
    return out;
}

std::string formatBend(const std::vector<project::PitchPoint>& points,
                       const core::TempoMap& tempo) {
    std::string out;
    for (const project::PitchPoint& point : points) {
        if (!out.empty()) {
            out += '|';
        }
        out += formatPitchAmount(point.cents);
        out += '@';
        out += formatDuration(point.at, tempo);
        if (!isDefaultCurve(point.curve)) {
            out += '~';
            out += formatCurve(point.curve);
        }
    }
    return out;
}

// --- sampler zones (docs/adx-format-v2.md, ZONE) -----------------------------

namespace {

/// "60", "C4", or a range "36..48" / "C2..C3" into [low, high]; a single value is a
/// range of one.
bool parseKeyRange(const Token& token, DiagnosticList& diagnostics, std::uint8_t& low,
                   std::uint8_t& high) {
    const std::string_view text = token.value;
    const std::size_t dots = text.find("..");
    const std::string_view first = text.substr(0, dots);
    const std::string_view second = dots == std::string_view::npos ? first : text.substr(dots + 2);
    std::uint32_t bad = 0;
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    if (!noteNameToMidi(first, a, bad) || !noteNameToMidi(second, b, bad)) {
        diagnostics.add(code::kBadNoteName, token.valueSpan,
                        "a key or range is a MIDI number or note name, or two joined by '..'");
        return false;
    }
    low = std::min(a, b);
    high = std::max(a, b);
    return true;
}

bool parseFrames(const Token& token, DiagnosticList& diagnostics, std::uint32_t& out) {
    std::int64_t value = 0;
    if (!parseInt64(token.value, token.valueSpan, diagnostics, value)) {
        return false;
    }
    if (value < 0 || std::cmp_greater(value, std::numeric_limits<std::uint32_t>::max())) {
        diagnostics.add(code::kValueOutOfRange, token.valueSpan, "a frame position is 0 or more");
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

} // namespace

void parseZoneFields(std::span<const Token> fields, DiagnosticList& diagnostics,
                     project::SampleZone& zone) {
    for (const Token& token : fields) {
        if (!token.isKeyValue()) {
            diagnostics.add(code::kWrongFieldCount, token.span, "expected key=value");
            continue;
        }
        const std::string& key = token.text;
        double number = 0.0;
        if (key == "key") {
            (void)parseKeyRange(token, diagnostics, zone.keyLow, zone.keyHigh);
        } else if (key == "vel") {
            std::uint8_t low = 1;
            std::uint8_t high = 127;
            if (parseKeyRange(token, diagnostics, low, high)) {
                zone.velocityLow = low;
                zone.velocityHigh = high;
            }
        } else if (key == "root") {
            std::uint8_t root = 60;
            std::uint32_t bad = 0;
            if (noteNameToMidi(token.value, root, bad)) {
                zone.rootKey = root;
            } else {
                diagnostics.add(code::kBadNoteName, token.valueSpan,
                                "root is a MIDI number or note name");
            }
        } else if (key == "start") {
            (void)parseFrames(token, diagnostics, zone.start);
        } else if (key == "end") {
            (void)parseFrames(token, diagnostics, zone.end);
        } else if (key == "loopStart") {
            (void)parseFrames(token, diagnostics, zone.loopStart);
        } else if (key == "loopEnd") {
            (void)parseFrames(token, diagnostics, zone.loopEnd);
        } else if (key == "xfade") {
            (void)parseFrames(token, diagnostics, zone.crossfade);
        } else if (key == "loop") {
            if (!project::loopModeFromString(token.value, zone.loop)) {
                diagnostics.add(code::kUnknownInlineKey, token.valueSpan,
                                "unknown loop mode '" + token.value + "'");
            }
        } else if (key == "tune" || key == "gain" || key == "pan") {
            if (parseDouble(token.value, token.valueSpan, diagnostics, number)) {
                const auto value = static_cast<float>(number);
                if (key == "tune") {
                    zone.tuneCents = value;
                } else if (key == "gain") {
                    zone.gainDb = value;
                } else {
                    zone.pan = value;
                }
            }
        } else if (key == "rr") {
            // group:index
            const std::size_t colon = token.value.find(':');
            std::int64_t group = 0;
            std::int64_t index = 0;
            if (colon == std::string::npos ||
                !parseInt64(std::string_view(token.value).substr(0, colon), token.valueSpan,
                            diagnostics, group) ||
                !parseInt64(std::string_view(token.value).substr(colon + 1), token.valueSpan,
                            diagnostics, index) ||
                group < 0 || group > 255 || index < 0 || index > 255) {
                diagnostics.add(code::kValueOutOfRange, token.valueSpan,
                                "round robin is rr=<group>:<index>, each 0..255");
                continue;
            }
            zone.roundRobinGroup = static_cast<std::uint8_t>(group);
            zone.roundRobinIndex = static_cast<std::uint8_t>(index);
        } else {
            diagnostics.add(code::kUnknownInlineKey, token.span,
                            "unknown key '" + key + "' on a ZONE line");
        }
    }
}

/// "36" or "36..48": a range of one is written as its single value.
namespace {

std::string formatKeyRange(std::uint8_t low, std::uint8_t high) {
    return low == high ? std::to_string(low) : std::to_string(low) + ".." + std::to_string(high);
}

} // namespace

std::string formatZoneFields(const project::SampleZone& zone) {
    std::string text;
    const project::SampleZone defaults;
    if (zone.keyLow != defaults.keyLow || zone.keyHigh != defaults.keyHigh) {
        text += " key=" + formatKeyRange(zone.keyLow, zone.keyHigh);
    }
    text += " root=" + std::to_string(zone.rootKey);
    if (zone.velocityLow != defaults.velocityLow || zone.velocityHigh != defaults.velocityHigh) {
        text += " vel=" + formatKeyRange(zone.velocityLow, zone.velocityHigh);
    }
    if (zone.start != 0) {
        text += " start=" + std::to_string(zone.start);
    }
    if (zone.end != 0) {
        text += " end=" + std::to_string(zone.end);
    }
    if (zone.loop != project::LoopMode::Off) {
        text += " loop=";
        text += project::toString(zone.loop);
    }
    if (zone.loopStart != 0) {
        text += " loopStart=" + std::to_string(zone.loopStart);
    }
    if (zone.loopEnd != 0) {
        text += " loopEnd=" + std::to_string(zone.loopEnd);
    }
    if (zone.crossfade != 0) {
        text += " xfade=" + std::to_string(zone.crossfade);
    }
    if (zone.tuneCents != 0.0F) {
        text += " tune=" + formatFloat(zone.tuneCents);
    }
    if (zone.gainDb != 0.0F) {
        text += " gain=" + formatFloat(zone.gainDb);
    }
    if (zone.pan != 0.0F) {
        text += " pan=" + formatFloat(zone.pan);
    }
    if (zone.roundRobinGroup != 0) {
        text += " rr=" + std::to_string(zone.roundRobinGroup) + ":" +
                std::to_string(zone.roundRobinIndex);
    }
    return text;
}

} // namespace adx::format

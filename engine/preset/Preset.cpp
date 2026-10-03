// adx-thread: main
#include "engine/preset/Preset.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "engine/format/adx/Lexer.h"
#include "engine/format/adx/Value.h"

namespace adx::preset {
namespace {

using format::code::kUnknownKey;
using format::code::kUnknownParameter;
using format::code::kUnknownSection;
using format::code::kValueOutOfRange;

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return std::string(text.substr(first, last - first + 1));
}

/// A parameter line: `name=value [curve=...]`.
void parseParam(const format::Line& line, const project::TypeInfo* type,
                format::DiagnosticList& diagnostics, Preset& out) {
    const std::vector<format::Token> tokens = format::tokenize(line, diagnostics);
    if (tokens.empty() || !tokens.front().isKeyValue()) {
        diagnostics.add(format::code::kWrongFieldCount, line.contentSpan(),
                        "a parameter is 'name=value [curve=...]'");
        return;
    }
    const format::Token& head = tokens.front();
    double value = 0.0;
    if (!format::parseDouble(head.value, head.valueSpan, diagnostics, value)) {
        return;
    }
    project::ParamValue param{.name = head.text, .value = value, .hasCurve = false, .curve = {}};
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i].isKeyValue() && tokens[i].text == "curve") {
            param.hasCurve =
                format::parseCurve(tokens[i].value, tokens[i].valueSpan, diagnostics, param.curve);
        } else {
            diagnostics.add(format::code::kUnknownInlineKey, tokens[i].span,
                            "unknown key on a parameter line");
        }
    }
    if (type != nullptr) {
        if (const project::ParamDescriptor* descriptor = project::findParam(*type, param.name)) {
            if (value < descriptor->minimum || value > descriptor->maximum) {
                diagnostics.add(kValueOutOfRange, head.valueSpan,
                                param.name + " is outside " + std::to_string(descriptor->minimum) +
                                    ".." + std::to_string(descriptor->maximum));
            }
        } else {
            diagnostics.add(kUnknownParameter, head.span,
                            "'" + std::string(type->name) + "' has no parameter '" + param.name +
                                "'");
        }
    }
    out.params.push_back(std::move(param));
}

void parseZone(const format::Line& line, format::DiagnosticList& diagnostics, Preset& out) {
    const std::vector<format::Token> tokens = format::tokenize(line, diagnostics);
    if (tokens.size() < 2 || tokens.front().text != "ZONE" || tokens[1].isKeyValue()) {
        diagnostics.add(format::code::kWrongFieldCount, line.contentSpan(),
                        "a zone is 'ZONE \"sample.wav\" key=... root=...'");
        return;
    }
    PresetZone zone;
    zone.path = tokens[1].text;
    format::parseZoneFields(std::span<const format::Token>{tokens}.subspan(2), diagnostics,
                            zone.zone);
    out.zones.push_back(std::move(zone));
}

} // namespace

project::TypeKind Preset::kind() const noexcept {
    return project::findEffectType(type) != nullptr ? project::TypeKind::Effect
                                                    : project::TypeKind::Instrument;
}

bool Preset::hasTag(std::string_view tag) const noexcept {
    return std::ranges::find(tags, tag) != tags.end();
}

Preset parsePreset(std::string_view text, format::DiagnosticList& diagnostics) {
    std::string bom;
    std::vector<format::Line> const lines = format::lexLines(text, bom, diagnostics);
    Preset out;
    std::string section;
    const project::TypeInfo* type = nullptr;
    for (format::Line const& line : lines) {
        if (line.kind == format::Line::Kind::Blank || line.kind == format::Line::Kind::Comment) {
            continue;
        }
        if (line.kind == format::Line::Kind::SectionHeader) {
            section = line.sectionType;
            if (section != "PRESET" && section != "PARAMS" && section != "ZONES") {
                diagnostics.add(kUnknownSection, line.span(),
                                "unknown section [" + section + "] in a preset");
            }
            continue;
        }
        if (section == "PRESET" && line.kind == format::Line::Kind::KeyValue) {
            const std::string value = format::decodeValue(line.value);
            if (line.key == "NAME") {
                out.name = value;
            } else if (line.key == "TYPE") {
                out.type = value;
                type = project::findInstrumentType(value);
                if (type == nullptr) {
                    type = project::findEffectType(value);
                }
                if (type == nullptr) {
                    diagnostics.add(kUnknownKey, line.valueSpan,
                                    "unknown instrument or effect type '" + value + "'");
                }
            } else if (line.key == "PACK") {
                out.pack = value;
            } else if (line.key == "MIX") {
                double mix = 0.0;
                if (format::parseDouble(value, line.valueSpan, diagnostics, mix)) {
                    if (mix < 0.0 || mix > 1.0) {
                        diagnostics.add(kValueOutOfRange, line.valueSpan, "MIX is 0..1");
                    }
                    out.mix = mix;
                }
            } else if (line.key == "TAGS") {
                std::string_view rest = value;
                while (!rest.empty()) {
                    const std::size_t comma = rest.find(',');
                    std::string tag = trim(rest.substr(0, comma));
                    if (!tag.empty()) {
                        out.tags.push_back(std::move(tag));
                    }
                    rest = comma == std::string_view::npos ? std::string_view{}
                                                           : rest.substr(comma + 1);
                }
            } else {
                diagnostics.add(kUnknownKey, line.keySpan, "unknown preset key '" + line.key + "'");
            }
        } else if (section == "PARAMS") {
            parseParam(line, type, diagnostics, out);
        } else if (section == "ZONES") {
            parseZone(line, diagnostics, out);
        }
    }
    return out;
}

std::string writePreset(const Preset& preset) {
    std::ostringstream out;
    out << "[PRESET]\n";
    out << "NAME=" << format::quoteAlways(preset.name) << '\n';
    out << "TYPE=" << preset.type << '\n';
    if (!preset.pack.empty()) {
        out << "PACK=" << preset.pack << '\n';
    }
    if (preset.mix.has_value()) {
        out << "MIX=" << format::formatDouble(*preset.mix) << '\n';
    }
    if (!preset.tags.empty()) {
        out << "TAGS=";
        for (std::size_t i = 0; i < preset.tags.size(); ++i) {
            out << (i == 0 ? "" : ", ") << preset.tags[i];
        }
        out << '\n';
    }
    if (!preset.params.empty()) {
        out << "\n[PARAMS]\n";
        for (const project::ParamValue& param : preset.params) {
            out << param.name << '=' << format::formatDouble(param.value);
            if (param.hasCurve) {
                out << " curve=" << format::formatCurve(param.curve);
            }
            out << '\n';
        }
    }
    if (!preset.zones.empty()) {
        out << "\n[ZONES]\n";
        for (const PresetZone& zone : preset.zones) {
            out << "ZONE " << format::quoteAlways(zone.path) << format::formatZoneFields(zone.zone)
                << '\n';
        }
    }
    return out.str();
}

bool loadPreset(const std::filesystem::path& path, Preset& out,
                format::DiagnosticList& diagnostics) {
    std::ifstream const in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    out = parsePreset(text.str(), diagnostics);
    out.source = path;
    return true;
}

bool savePreset(const Preset& preset, const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary);
    out << writePreset(preset);
    return static_cast<bool>(out);
}

project::InstrumentSpec instrumentOf(const Preset& preset) {
    project::InstrumentSpec spec;
    spec.type = preset.type;
    spec.params = preset.params;
    return spec;
}

} // namespace adx::preset

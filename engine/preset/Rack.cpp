// adx-thread: main
#include "engine/preset/Rack.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

#include "engine/format/adx/Lexer.h"
#include "engine/format/adx/Value.h"
#include "engine/project/TypeCatalog.h"

namespace adx::preset {
namespace {

using format::code::kUnknownInlineKey;
using format::code::kUnknownKey;
using format::code::kUnknownParameter;
using format::code::kUnknownSection;
using format::code::kValueOutOfRange;
using format::code::kWrongFieldCount;
using project::Rack;
using project::RackLayer;
using project::RackSend;
using project::RackSlot;

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return std::string(text.substr(first, last - first + 1));
}

class RackParser {
public:
    explicit RackParser(format::DiagnosticList& diagnostics) : m_diag(diagnostics) {}

    Rack run(std::string_view text) {
        std::string bom;
        const std::vector<format::Line> lines = format::lexLines(text, bom, m_diag);
        for (const format::Line& line : lines) {
            if (line.kind == format::Line::Kind::Blank ||
                line.kind == format::Line::Kind::Comment) {
                continue;
            }
            if (line.kind == format::Line::Kind::SectionHeader) {
                open(line);
            } else if (line.kind == format::Line::Kind::KeyValue) {
                keyValue(line);
            } else if (m_section != Section::kNone) {
                positional(line);
            }
        }
        return std::move(m_rack);
    }

private:
    enum class Section : std::uint8_t { kNone, kRack, kLayer, kChain, kSend, kMacro };

    void open(const format::Line& line) {
        const std::string name = format::decodeValue(line.sectionName);
        if (line.sectionType == "RACK") {
            m_section = Section::kRack;
        } else if (line.sectionType == "CHAIN") {
            m_section = Section::kChain;
        } else if (line.sectionType == "LAYER") {
            m_section = usable(name, line, "layer") ? Section::kLayer : Section::kNone;
            if (m_section == Section::kLayer) {
                m_rack.layers.emplace_back().name = name;
            }
        } else if (line.sectionType == "SEND") {
            m_section = usable(name, line, "send") ? Section::kSend : Section::kNone;
            if (m_section == Section::kSend) {
                m_rack.sends.emplace_back().name = name;
            }
        } else if (line.sectionType == "MACRO") {
            m_section = usable(name, line, "macro") ? Section::kMacro : Section::kNone;
            if (m_section == Section::kMacro) {
                m_rack.macros.emplace_back().name = name;
            }
        } else {
            m_section = Section::kNone;
            m_diag.add(kUnknownSection, line.span(),
                       "unknown section [" + line.sectionType + "] in a rack");
        }
    }

    /// A layer, send or macro name is a path segment: no dots, quotes or brackets.
    bool usable(const std::string& name, const format::Line& line, std::string_view what) {
        if (!name.empty() && name.find_first_of(".\"[]") == std::string::npos) {
            return true;
        }
        m_diag.add(format::code::kBadNameCharacter, line.span(),
                   "'" + name + "' is not a usable " + std::string(what) + " name");
        return false;
    }

    bool number(const format::Line& line, const std::string& value, double& out) {
        return format::parseDouble(value, line.valueSpan, m_diag, out);
    }

    static bool byteRange(std::string_view text, std::uint8_t& low, std::uint8_t& high) {
        const auto whole = [](std::string_view part, int& out) {
            const auto result = std::from_chars(part.data(), part.data() + part.size(), out);
            return result.ec == std::errc{} && result.ptr == part.data() + part.size();
        };
        const std::size_t dots = text.find("..");
        int a = 0;
        if (!whole(text.substr(0, dots), a)) {
            return false;
        }
        int b = a;
        if (dots != std::string_view::npos && !whole(text.substr(dots + 2), b)) {
            return false;
        }
        low = static_cast<std::uint8_t>(std::clamp(std::min(a, b), 0, 127));
        high = static_cast<std::uint8_t>(std::clamp(std::max(a, b), 0, 127));
        return true;
    }

    void unknown(const format::Line& line, std::vector<project::RackExtra>& extra) {
        m_diag.add(kUnknownKey, line.keySpan,
                   "unknown rack key '" + line.key + "'; kept and written back");
        extra.emplace_back(line.key, line.value);
    }

    void keyValue(const format::Line& line) {
        const std::string value = format::decodeValue(line.value);
        double n = 0.0;
        switch (m_section) {
        case Section::kRack:
            rackKey(line, value);
            break;
        case Section::kLayer: {
            RackLayer& layer = m_rack.layers.back();
            if (line.key == "TYPE") {
                layer.type = value;
                if (project::findInstrumentType(value) == nullptr) {
                    m_diag.add(kUnknownKey, line.valueSpan,
                               "unknown instrument type '" + value + "'");
                }
            } else if (line.key == "PRESET") {
                layer.preset = value;
            } else if (line.key == "KEYS" || line.key == "VELOCITY") {
                const bool keys = line.key == "KEYS";
                if (!byteRange(value, keys ? layer.filter.keyLow : layer.filter.velocityLow,
                               keys ? layer.filter.keyHigh : layer.filter.velocityHigh)) {
                    m_diag.add(kWrongFieldCount, line.valueSpan, "a range is 'lo..hi'");
                }
            } else if (line.key == "GAIN" && number(line, value, n)) {
                layer.gainDb = static_cast<float>(n);
            } else if (line.key == "PAN" && number(line, value, n)) {
                layer.pan = static_cast<float>(n);
            } else if (line.key == "PITCH" && number(line, value, n)) {
                layer.pitchCents = static_cast<float>(n);
            } else if (line.key == "POLYPHONY" && number(line, value, n)) {
                layer.polyphony = static_cast<std::uint16_t>(std::clamp(n, 1.0, 256.0));
            } else if (line.key != "GAIN" && line.key != "PAN" && line.key != "PITCH" &&
                       line.key != "POLYPHONY") {
                unknown(line, layer.extra);
            }
            break;
        }
        case Section::kSend: {
            RackSend& send = m_rack.sends.back();
            if (line.key == "LEVEL") {
                if (number(line, value, n)) {
                    send.levelDb = static_cast<float>(n);
                }
            } else {
                unknown(line, send.extra);
            }
            break;
        }
        case Section::kMacro:
            if (line.key == "VALUE") {
                if (number(line, value, n)) {
                    m_rack.macros.back().value = static_cast<float>(std::clamp(n, 0.0, 1.0));
                }
            } else {
                m_diag.add(kUnknownKey, line.keySpan, "unknown macro key '" + line.key + "'");
            }
            break;
        case Section::kChain:
        case Section::kNone:
            m_diag.add(kUnknownKey, line.keySpan, "'" + line.key + "' has no section to set");
            break;
        }
    }

    void rackKey(const format::Line& line, const std::string& value) {
        double n = 0.0;
        if (line.key == "NAME") {
            m_rack.name = value;
        } else if (line.key == "PACK") {
            m_rack.pack = value;
        } else if (line.key == "CATEGORY") {
            m_rack.category = value;
        } else if (line.key == "ROOT_HINT") {
            m_rack.rootHint = value;
        } else if (line.key == "FALLBACK") {
            m_rack.fallback = value;
        } else if (line.key == "TEMPO_HINT") {
            if (number(line, value, n)) {
                m_rack.tempoHint = n;
            }
        } else if (line.key == "VERSION") {
            if (number(line, value, n)) {
                m_rack.version = static_cast<int>(n);
            }
        } else if (line.key == "TAGS") {
            std::string_view rest = value;
            while (!rest.empty()) {
                const std::size_t comma = rest.find(',');
                std::string tag = trim(rest.substr(0, comma));
                if (!tag.empty()) {
                    m_rack.tags.push_back(std::move(tag));
                }
                rest =
                    comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
        } else {
            unknown(line, m_rack.extra);
        }
    }

    void positional(const format::Line& line) {
        const std::vector<format::Token> tokens = format::tokenize(line, m_diag);
        if (tokens.empty()) {
            return;
        }
        const std::string& head = tokens.front().text;
        if (head == "SLOT" && m_section != Section::kRack && m_section != Section::kMacro) {
            if (m_section == Section::kChain) {
                slot(line, tokens, m_rack.chain);
            } else if (m_section == Section::kSend) {
                slot(line, tokens, m_rack.sends.back().slots);
            } else {
                slot(line, tokens, m_rack.layers.back().chain);
            }
        } else if (head == "PARAM" && m_section == Section::kLayer) {
            param(line, tokens);
        } else if (head == "ZONE" && m_section == Section::kLayer) {
            if (tokens.size() < 2 || tokens[1].isKeyValue()) {
                m_diag.add(kWrongFieldCount, line.contentSpan(),
                           "a zone is 'ZONE \"sample.wav\" key=... root=...'");
                return;
            }
            project::RackZone zone;
            zone.path = tokens[1].text;
            format::parseZoneFields(std::span<const format::Token>{tokens}.subspan(2), m_diag,
                                    zone.zone);
            m_rack.layers.back().zones.push_back(std::move(zone));
        } else if (head == "TARGET" && m_section == Section::kMacro) {
            target(line, tokens);
        } else {
            m_diag.add(kUnknownKey, line.contentSpan(), "unrecognised line in a rack");
        }
    }

    void slot(const format::Line& line, const std::vector<format::Token>& tokens,
              std::vector<RackSlot>& chain) {
        if (tokens.size() < 2 || tokens[1].isKeyValue()) {
            m_diag.add(kWrongFieldCount, line.contentSpan(),
                       "a rack slot is 'SLOT <Type> [key=value...]'");
            return;
        }
        RackSlot slot;
        slot.type = tokens[1].text;
        const project::TypeInfo* type = project::findEffectType(slot.type);
        if (type == nullptr) {
            m_diag.add(kUnknownKey, tokens[1].span, "unknown effect type '" + slot.type + "'");
        }
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const format::Token& token = tokens[i];
            if (!token.isKeyValue()) {
                m_diag.add(kWrongFieldCount, token.span, "expected key=value");
                continue;
            }
            if (token.text == "key") {
                slot.keyRack = token.value == "rack";
                if (!slot.keyRack) {
                    m_diag.add(kUnknownInlineKey, token.valueSpan,
                               "a rack slot is keyed by 'rack' or not at all");
                }
                continue;
            }
            if (token.text == "ir") {
                slot.impulse = token.value;
                continue;
            }
            if (token.text == "bypass") {
                bool flag = false;
                if (format::parseBool(token.value, token.valueSpan, m_diag, flag)) {
                    slot.bypass = flag;
                }
                continue;
            }
            double value = 0.0;
            if (!format::parseDouble(token.value, token.valueSpan, m_diag, value)) {
                continue;
            }
            if (token.text == "mix") {
                slot.mix = static_cast<float>(value);
                continue;
            }
            check(type, token, value);
            slot.params.push_back(project::SlotParam{.name = token.text, .value = value});
        }
        chain.push_back(std::move(slot));
    }

    void check(const project::TypeInfo* type, const format::Token& token, double value) {
        if (type == nullptr) {
            return;
        }
        const project::ParamDescriptor* descriptor = project::findParam(*type, token.text);
        if (descriptor == nullptr) {
            m_diag.add(kUnknownParameter, token.span,
                       "'" + std::string(type->name) + "' has no parameter '" + token.text + "'");
        } else if (value < descriptor->minimum || value > descriptor->maximum) {
            m_diag.add(kValueOutOfRange, token.valueSpan,
                       token.text + " is outside " + std::to_string(descriptor->minimum) + ".." +
                           std::to_string(descriptor->maximum));
        }
    }

    void param(const format::Line& line, const std::vector<format::Token>& tokens) {
        if (tokens.size() < 2 || !tokens[1].isKeyValue()) {
            m_diag.add(kWrongFieldCount, line.contentSpan(),
                       "a parameter is 'PARAM name=value [curve=...]'");
            return;
        }
        RackLayer& layer = m_rack.layers.back();
        double value = 0.0;
        if (!format::parseDouble(tokens[1].value, tokens[1].valueSpan, m_diag, value)) {
            return;
        }
        if (layer.preset.empty()) {
            check(project::findInstrumentType(layer.type), tokens[1], value);
        }
        project::ParamValue out{
            .name = tokens[1].text, .value = value, .hasCurve = false, .curve = {}};
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            if (tokens[i].isKeyValue() && tokens[i].text == "curve") {
                out.hasCurve =
                    format::parseCurve(tokens[i].value, tokens[i].valueSpan, m_diag, out.curve);
            } else {
                m_diag.add(kUnknownInlineKey, tokens[i].span, "unknown key on a PARAM line");
            }
        }
        layer.params.push_back(std::move(out));
    }

    void target(const format::Line& line, const std::vector<format::Token>& tokens) {
        if (tokens.size() < 2 || tokens[1].isKeyValue()) {
            m_diag.add(kWrongFieldCount, line.contentSpan(),
                       "a macro target is 'TARGET path range=lo..hi [curve=...]'");
            return;
        }
        project::RackMacroTarget out;
        out.path = tokens[1].text;
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const format::Token& token = tokens[i];
            if (token.isKeyValue() && token.text == "range") {
                const std::string_view text = token.value;
                const std::size_t dots = text.find("..");
                double low = 0.0;
                double high = 1.0;
                if (dots == std::string_view::npos) {
                    m_diag.add(kWrongFieldCount, token.valueSpan, "a range is 'lo..hi'");
                } else if (format::parseDouble(text.substr(0, dots), token.valueSpan, m_diag,
                                               low) &&
                           format::parseDouble(text.substr(dots + 2), token.valueSpan, m_diag,
                                               high)) {
                    out.minimum = static_cast<float>(low);
                    out.maximum = static_cast<float>(high);
                }
            } else if (token.isKeyValue() && token.text == "curve") {
                static_cast<void>(
                    format::parseCurve(token.value, token.valueSpan, m_diag, out.curve));
            } else {
                m_diag.add(kUnknownInlineKey, token.span, "unknown key on a TARGET line");
            }
        }
        m_rack.macros.back().targets.push_back(std::move(out));
    }

    format::DiagnosticList& m_diag;
    Rack m_rack;
    Section m_section{Section::kNone};
};

void writeSlots(std::ostringstream& out, const std::vector<RackSlot>& slots) {
    for (const RackSlot& slot : slots) {
        out << "SLOT " << slot.type;
        if (slot.mix != 1.0F) {
            out << " mix=" << format::formatFloat(slot.mix);
        }
        if (slot.bypass) {
            out << " bypass=" << format::formatBool(true);
        }
        for (const project::SlotParam& param : slot.params) {
            out << ' ' << param.name << '=' << format::formatDouble(param.value);
        }
        if (!slot.impulse.empty()) {
            out << " ir=" << format::quoteAlways(slot.impulse);
        }
        if (slot.keyRack) {
            out << " key=rack";
        }
        out << '\n';
    }
}

void writeExtra(std::ostringstream& out, const std::vector<project::RackExtra>& extra) {
    for (const auto& [key, value] : extra) {
        out << key << '=' << value << '\n';
    }
}

std::string range(std::uint8_t low, std::uint8_t high) {
    return std::to_string(low) + ".." + std::to_string(high);
}

} // namespace

project::Rack parseRack(std::string_view text, format::DiagnosticList& diagnostics) {
    return RackParser(diagnostics).run(text);
}

std::string writeRack(const project::Rack& rack) {
    std::ostringstream out;
    out << "[RACK]\n";
    out << "NAME=" << format::quoteAlways(rack.name) << '\n';
    if (!rack.pack.empty()) {
        out << "PACK=" << rack.pack << '\n';
    }
    if (!rack.category.empty()) {
        out << "CATEGORY=" << rack.category << '\n';
    }
    if (!rack.tags.empty()) {
        out << "TAGS=";
        for (std::size_t i = 0; i < rack.tags.size(); ++i) {
            out << (i == 0 ? "" : ", ") << rack.tags[i];
        }
        out << '\n';
    }
    if (rack.tempoHint != 0.0) {
        out << "TEMPO_HINT=" << format::formatDouble(rack.tempoHint) << '\n';
    }
    if (!rack.rootHint.empty()) {
        out << "ROOT_HINT=" << rack.rootHint << '\n';
    }
    if (!rack.fallback.empty()) {
        out << "FALLBACK=" << format::quoteAlways(rack.fallback) << '\n';
    }
    out << "VERSION=" << rack.version << '\n';
    writeExtra(out, rack.extra);

    const project::RackLayer defaults;
    for (const project::RackLayer& layer : rack.layers) {
        out << "\n[LAYER " << layer.name << "]\n";
        if (!layer.preset.empty()) {
            out << "PRESET=" << format::quoteAlways(layer.preset) << '\n';
        } else {
            out << "TYPE=" << layer.type << '\n';
        }
        if (layer.filter.keyLow != 0 || layer.filter.keyHigh < 127) {
            out << "KEYS=" << range(layer.filter.keyLow, layer.filter.keyHigh) << '\n';
        }
        if (layer.filter.velocityLow != 0 || layer.filter.velocityHigh < 127) {
            out << "VELOCITY=" << range(layer.filter.velocityLow, layer.filter.velocityHigh)
                << '\n';
        }
        if (layer.gainDb != defaults.gainDb) {
            out << "GAIN=" << format::formatFloat(layer.gainDb) << '\n';
        }
        if (layer.pan != defaults.pan) {
            out << "PAN=" << format::formatFloat(layer.pan) << '\n';
        }
        if (layer.pitchCents != defaults.pitchCents) {
            out << "PITCH=" << format::formatFloat(layer.pitchCents) << '\n';
        }
        if (layer.polyphony != defaults.polyphony) {
            out << "POLYPHONY=" << layer.polyphony << '\n';
        }
        writeExtra(out, layer.extra);
        for (const project::ParamValue& param : layer.params) {
            out << "PARAM " << param.name << '=' << format::formatDouble(param.value);
            if (param.hasCurve) {
                out << " curve=" << format::formatCurve(param.curve);
            }
            out << '\n';
        }
        for (const project::RackZone& zone : layer.zones) {
            out << "ZONE " << format::quoteAlways(zone.path) << format::formatZoneFields(zone.zone)
                << '\n';
        }
        writeSlots(out, layer.chain);
    }
    if (!rack.chain.empty()) {
        out << "\n[CHAIN]\n";
        writeSlots(out, rack.chain);
    }
    for (const project::RackSend& send : rack.sends) {
        out << "\n[SEND " << send.name << "]\n";
        out << "LEVEL=" << format::formatFloat(send.levelDb) << '\n';
        writeExtra(out, send.extra);
        writeSlots(out, send.slots);
    }
    for (const project::RackMacro& macro : rack.macros) {
        out << "\n[MACRO " << macro.name << "]\n";
        out << "VALUE=" << format::formatFloat(macro.value) << '\n';
        for (const project::RackMacroTarget& target : macro.targets) {
            out << "TARGET " << target.path << " range=" << format::formatFloat(target.minimum)
                << ".." << format::formatFloat(target.maximum);
            if (!(target.curve == core::Curve{})) {
                out << " curve=" << format::formatCurve(target.curve);
            }
            out << '\n';
        }
    }
    return out.str();
}

bool loadRack(const std::filesystem::path& path, project::Rack& out,
              format::DiagnosticList& diagnostics) {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    out = parseRack(text.str(), diagnostics);
    return true;
}

bool saveRack(const project::Rack& rack, const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary);
    out << writeRack(rack);
    return static_cast<bool>(out);
}

void resolveRackPresets(project::Rack& rack, const PresetLibrary& library,
                        format::DiagnosticList& diagnostics) {
    for (project::RackLayer& layer : rack.layers) {
        if (layer.preset.empty()) {
            continue;
        }
        const std::size_t slash = layer.preset.find('/');
        const std::string pack =
            slash == std::string::npos ? std::string{} : layer.preset.substr(0, slash);
        const std::string name =
            slash == std::string::npos ? layer.preset : layer.preset.substr(slash + 1);
        const Preset* preset = library.find(name, pack);
        if (preset == nullptr || preset->kind() != project::TypeKind::Instrument) {
            diagnostics.add(format::code::kUnknownKey, format::Span{},
                            "rack layer '" + layer.name + "' names preset '" + layer.preset +
                                "', which is not an instrument preset in the library");
            layer.preset.clear();
            continue;
        }
        std::vector<project::ParamValue> params = preset->params;
        for (const project::ParamValue& own : layer.params) {
            const auto match = std::ranges::find(params, own.name, &project::ParamValue::name);
            if (match != params.end()) {
                *match = own;
            } else {
                params.push_back(own);
            }
        }
        layer.type = preset->type;
        layer.params = std::move(params);
        if (layer.zones.empty()) {
            const std::filesystem::path directory = preset->source.parent_path();
            for (const PresetZone& zone : preset->zones) {
                std::string path = zone.path;
                if (!path.starts_with("lib://") && std::filesystem::path(path).is_relative() &&
                    !directory.empty()) {
                    path = (directory / path).lexically_normal().generic_string();
                }
                layer.zones.push_back(project::RackZone{.zone = zone.zone, .path = path});
            }
        }
        layer.preset.clear();
    }
}

void anchorRackPaths(project::Rack& rack, const std::filesystem::path& directory) {
    const auto anchor = [&directory](std::string& path) {
        if (path.empty() || path.starts_with("lib://") ||
            std::filesystem::path(path).is_absolute()) {
            return;
        }
        path = (directory / path).lexically_normal().generic_string();
    };
    const auto slots = [&anchor](std::vector<project::RackSlot>& chain) {
        for (project::RackSlot& slot : chain) {
            anchor(slot.impulse);
        }
    };
    for (project::RackLayer& layer : rack.layers) {
        for (project::RackZone& zone : layer.zones) {
            anchor(zone.path);
        }
        slots(layer.chain);
    }
    slots(rack.chain);
    for (project::RackSend& send : rack.sends) {
        slots(send.slots);
    }
}

} // namespace adx::preset

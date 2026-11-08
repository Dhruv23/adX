// adx-thread: main
#include "engine/midi/PadController.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace adx::midi {

namespace {

constexpr std::array<const char*, kPadLooks> kLookKeys{"EMPTY",   "STOPPED",     "QUEUED",
                                                       "PLAYING", "QUEUED_STOP", "RECORDING"};

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// KEY=VALUE tokens; a quoted value keeps its spaces, and a bare word after a key's
/// value joins it (SYSEX_PREFIX=F0 00 20 ...).
std::vector<std::pair<std::string, std::string>> tokens(std::string_view line) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        std::size_t const start = i;
        bool quoted = false;
        while (i < line.size() && (quoted || (line[i] != ' ' && line[i] != '\t'))) {
            if (line[i] == '"') {
                quoted = !quoted;
            }
            ++i;
        }
        const std::string_view word = line.substr(start, i - start);
        const std::size_t eq = word.find('=');
        if (eq == std::string_view::npos) {
            if (!out.empty()) {
                out.back().second += ' ';
                out.back().second += std::string{word};
            }
            continue;
        }
        std::string value{word.substr(eq + 1)};
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        out.emplace_back(std::string{word.substr(0, eq)}, std::move(value));
    }
    return out;
}

bool toInt(std::string_view text, int& out) {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool toBool(std::string_view text, bool& out) {
    if (text == "yes" || text == "true" || text == "1" || text == "on") {
        out = true;
        return true;
    }
    if (text == "no" || text == "false" || text == "0" || text == "off") {
        out = false;
        return true;
    }
    return false;
}

std::vector<std::uint8_t> numberList(std::string_view text, bool& ok) {
    std::vector<std::uint8_t> out;
    ok = true;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t comma = text.find(',', at);
        const std::string_view part =
            text.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
        int value = 0;
        if (!toInt(part, value) || value < 0 || value > 127) {
            ok = false;
            return {};
        }
        out.push_back(static_cast<std::uint8_t>(value));
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 1;
    }
    return out;
}

std::vector<std::uint8_t> hexBytes(std::string_view text, bool& ok) {
    std::vector<std::uint8_t> out;
    ok = true;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        std::size_t end = i;
        while (end < text.size() && text[end] != ' ') {
            ++end;
        }
        unsigned value = 0;
        const auto result = std::from_chars(text.data() + i, text.data() + end, value, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + end || value > 0xFF) {
            ok = false;
            return {};
        }
        out.push_back(static_cast<std::uint8_t>(value));
        i = end;
    }
    return out;
}

bool parseColor(std::string_view text, PadColor& out) {
    PadColor color;
    std::string_view head = text;
    const std::size_t comma = text.find(',');
    if (comma != std::string_view::npos) {
        head = text.substr(0, comma);
        std::string_view flags = text.substr(comma + 1);
        while (!flags.empty()) {
            const std::size_t next = flags.find(',');
            const std::string_view flag = flags.substr(0, next);
            if (flag == "blink") {
                color.blink = true;
            } else if (flag == "pulse") {
                color.pulse = true;
            } else {
                return false;
            }
            flags = next == std::string_view::npos ? std::string_view{} : flags.substr(next + 1);
        }
    }
    if (head.starts_with("vel:")) {
        int value = 0;
        if (!toInt(head.substr(4), value) || value < 0 || value > 127) {
            return false;
        }
        color.velocity = static_cast<std::uint8_t>(value);
    } else if (head.size() == 7 && head.front() == '#') {
        unsigned value = 0;
        const auto result = std::from_chars(head.data() + 1, head.data() + 7, value, 16);
        if (result.ec != std::errc{} || result.ptr != head.data() + 7) {
            return false;
        }
        color.rgb = value;
    } else {
        return false;
    }
    out = color;
    return true;
}

bool parseControl(std::string_view text, PadControl& out) {
    PadControl control;
    std::string_view list;
    if (text.starts_with("cc:")) {
        control.cc = true;
        list = text.substr(3);
    } else if (text.starts_with("notes:")) {
        list = text.substr(6);
    } else if (text.starts_with("note:")) {
        list = text.substr(5);
    } else {
        return false;
    }
    bool ok = false;
    control.numbers = numberList(list, ok);
    if (!ok) {
        return false;
    }
    out = std::move(control);
    return true;
}

std::string colorText(const PadColor& color, FeedbackMode mode) {
    std::string out;
    if (mode == FeedbackMode::Note || (color.rgb == 0 && color.velocity != 0)) {
        out = "vel:" + std::to_string(color.velocity);
    } else {
        std::array<char, 8> hex{};
        std::snprintf(hex.data(), hex.size(), "#%06X", color.rgb & 0xFFFFFFU);
        out = hex.data();
    }
    if (color.blink) {
        out += ",blink";
    }
    if (color.pulse) {
        out += ",pulse";
    }
    return out;
}

std::string listText(const std::vector<std::uint8_t>& numbers) {
    std::string out;
    for (const std::uint8_t n : numbers) {
        if (!out.empty()) {
            out += ',';
        }
        out += std::to_string(n);
    }
    return out;
}

std::string controlText(const PadControl& control) {
    if (control.numbers.empty()) {
        return {};
    }
    if (control.cc) {
        return "cc:" + listText(control.numbers);
    }
    return (control.numbers.size() == 1 ? "note:" : "notes:") + listText(control.numbers);
}

std::string hexText(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    for (const std::uint8_t b : bytes) {
        std::array<char, 4> hex{};
        std::snprintf(hex.data(), hex.size(), "%02X", b);
        if (!out.empty()) {
            out += ' ';
        }
        out += hex.data();
    }
    return out;
}

void profileKey(PadProfile& p, const std::string& key, const std::string& value,
                std::vector<std::string>& warnings, bool& known) {
    known = true;
    int number = 0;
    bool flag = false;
    bool ok = true;
    if (key == "NAME") {
        p.name = value;
    } else if (key == "MATCH_DEVICE") {
        p.matchDevice = value;
    } else if (key == "GRID") {
        const std::size_t x = value.find('x');
        int c = 0;
        int r = 0;
        ok = x != std::string::npos && toInt(std::string_view{value}.substr(0, x), c) &&
             toInt(std::string_view{value}.substr(x + 1), r) && c > 0 && r > 0 && c <= 64 &&
             r <= 64;
        if (ok) {
            p.columns = static_cast<std::uint32_t>(c);
            p.rows = static_cast<std::uint32_t>(r);
        }
    } else if (key == "NOTE_BASE") {
        ok = toInt(value, p.noteBase);
    } else if (key == "ROW_STRIDE") {
        ok = toInt(value, p.rowStride);
    } else if (key == "COLUMN_STRIDE") {
        ok = toInt(value, p.columnStride);
    } else if (key == "NOTES") {
        p.notes = numberList(value, ok);
    } else if (key == "CHANNEL") {
        if (value == "any") {
            p.channel = 0;
        } else {
            ok = toInt(value, number) && number >= 1 && number <= 16;
            p.channel = ok ? static_cast<std::uint8_t>(number) : p.channel;
        }
    } else if (key == "VELOCITY_SENSITIVE") {
        ok = toBool(value, flag);
        p.velocitySensitive = ok ? flag : p.velocitySensitive;
    } else if (key == "VELOCITY_CURVE") {
        ok = value == "linear" || value == "soft" || value == "hard";
        p.velocityCurve = ok ? value : p.velocityCurve;
    } else if (key == "CONSUME_ALL") {
        ok = toBool(value, flag);
        p.consumeAll = ok ? flag : p.consumeAll;
    } else {
        known = false;
    }
    if (known && !ok) {
        warnings.push_back("[PAD_PROFILE] " + key + "=" + value + ": not understood");
    }
}

void feedbackKey(PadProfile& p, const std::string& key, const std::string& value,
                 std::vector<std::string>& warnings, bool& known) {
    known = true;
    bool ok = true;
    int number = 0;
    if (key == "MODE") {
        if (value == "note") {
            p.feedback = FeedbackMode::Note;
        } else if (value == "sysex_rgb") {
            p.feedback = FeedbackMode::SysexRgb;
        } else if (value == "none") {
            p.feedback = FeedbackMode::None;
        } else {
            ok = false;
        }
    } else if (key == "CHANNEL") {
        ok = toInt(value, number) && number >= 1 && number <= 16;
        p.feedbackChannel = ok ? static_cast<std::uint8_t>(number) : p.feedbackChannel;
    } else if (key == "SYSEX_PREFIX") {
        p.sysexPrefix = hexBytes(value, ok);
    } else if (key == "SYSEX_SUFFIX") {
        p.sysexSuffix = hexBytes(value, ok);
    } else {
        const auto look = std::ranges::find_if(kLookKeys, [&](const char* k) { return key == k; });
        if (look == kLookKeys.end()) {
            known = false;
            return;
        }
        ok = parseColor(value, p.colors.at(static_cast<std::size_t>(look - kLookKeys.begin())));
    }
    if (!ok) {
        warnings.push_back("[FEEDBACK] " + key + "=" + value + ": not understood");
    }
}

void controlKey(PadProfile& p, const std::string& key, const std::string& value,
                std::vector<std::string>& warnings, bool& known) {
    known = true;
    PadControl* control = nullptr;
    if (key == "SCENE_LAUNCH") {
        control = &p.sceneLaunch;
    } else if (key == "STOP_TRACK") {
        control = &p.stopTrack;
    } else if (key == "STOP_ALL") {
        control = &p.stopAll;
    } else if (key == "TAP_TEMPO") {
        control = &p.tapTempo;
    } else {
        known = false;
        return;
    }
    if (!parseControl(value, *control)) {
        warnings.push_back("[CONTROLS] " + key + "=" + value + ": not understood");
    }
}

} // namespace

const char* toString(FeedbackMode mode) noexcept {
    switch (mode) {
    case FeedbackMode::Note:
        return "note";
    case FeedbackMode::SysexRgb:
        return "sysex_rgb";
    case FeedbackMode::None:
        break;
    }
    return "none";
}

int PadProfile::noteAt(std::uint32_t column, std::uint32_t row) const noexcept {
    if (column >= columns || row >= rows) {
        return -1;
    }
    if (!notes.empty()) {
        const std::size_t index = (static_cast<std::size_t>(row) * columns) + column;
        return index < notes.size() ? notes[index] : -1;
    }
    const int note =
        noteBase + (static_cast<int>(row) * rowStride) + (static_cast<int>(column) * columnStride);
    return note >= 0 && note <= 127 ? note : -1;
}

PadParse parsePadProfile(std::string_view text) {
    PadParse parse;
    PadProfile& p = parse.profile;
    std::string section;
    bool sawProfile = false;
    std::size_t at = 0;
    std::size_t lineNo = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find('\n', at);
        std::string_view const line = trim(
            text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++lineNo;
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = std::string{line.substr(1, line.size() - 2)};
            sawProfile = sawProfile || section == "PAD_PROFILE";
            if (section != "PAD_PROFILE" && section != "FEEDBACK" && section != "CONTROLS") {
                parse.warnings.push_back("line " + std::to_string(lineNo) + ": unknown section [" +
                                         section + "]; kept");
                p.unknown.push_back(section + "|");
            }
            continue;
        }
        for (const auto& [key, value] : tokens(line)) {
            bool known = false;
            if (section == "PAD_PROFILE") {
                profileKey(p, key, value, parse.warnings, known);
            } else if (section == "FEEDBACK") {
                feedbackKey(p, key, value, parse.warnings, known);
            } else if (section == "CONTROLS") {
                controlKey(p, key, value, parse.warnings, known);
            }
            if (!known) {
                std::string warning = "line " + std::to_string(lineNo);
                warning += ": unknown key ";
                warning += key;
                warning += "; kept";
                parse.warnings.push_back(std::move(warning));
                std::string kept = section;
                kept += '|';
                kept += key;
                kept += '=';
                kept += value;
                p.unknown.push_back(std::move(kept));
            }
        }
    }
    parse.ok = sawProfile;
    if (!sawProfile) {
        parse.warnings.emplace_back("no [PAD_PROFILE] section");
    }
    if (!p.notes.empty() && p.notes.size() != static_cast<std::size_t>(p.columns) * p.rows) {
        parse.warnings.push_back("NOTES has " + std::to_string(p.notes.size()) + " pads for a " +
                                 std::to_string(p.columns) + "x" + std::to_string(p.rows) +
                                 " grid");
    }
    return parse;
}

std::string writePadProfile(const PadProfile& p) {
    std::string out = "[PAD_PROFILE]\n";
    const auto line = [&out](const std::string& key, const std::string& value) {
        if (!value.empty()) {
            out += key + "=" + value + "\n";
        }
    };
    const auto unknownIn = [&](const std::string& section) {
        for (const std::string& entry : p.unknown) {
            const std::size_t bar = entry.find('|');
            if (entry.substr(0, bar) == section && bar + 1 < entry.size()) {
                out += entry.substr(bar + 1) + "\n";
            }
        }
    };
    line("NAME", "\"" + p.name + "\"");
    line("MATCH_DEVICE", "\"" + p.matchDevice + "\"");
    line("GRID", std::to_string(p.columns) + "x" + std::to_string(p.rows));
    if (p.notes.empty()) {
        line("NOTE_BASE", std::to_string(p.noteBase));
        line("ROW_STRIDE", std::to_string(p.rowStride));
        if (p.columnStride != 1) {
            line("COLUMN_STRIDE", std::to_string(p.columnStride));
        }
    } else {
        line("NOTES", listText(p.notes));
    }
    line("CHANNEL", p.channel == 0 ? "any" : std::to_string(p.channel));
    line("VELOCITY_SENSITIVE", p.velocitySensitive ? "yes" : "no");
    if (p.velocityCurve != "linear") {
        line("VELOCITY_CURVE", p.velocityCurve);
    }
    if (!p.consumeAll) {
        line("CONSUME_ALL", "no");
    }
    unknownIn("PAD_PROFILE");
    out += "[FEEDBACK]\n";
    line("MODE", toString(p.feedback));
    if (p.feedback != FeedbackMode::None) {
        line("CHANNEL", std::to_string(p.feedbackChannel));
        line("SYSEX_PREFIX", hexText(p.sysexPrefix));
        line("SYSEX_SUFFIX", hexText(p.sysexSuffix));
        for (std::size_t i = 0; i < kPadLooks; ++i) {
            line(kLookKeys.at(i), colorText(p.colors.at(i), p.feedback));
        }
    }
    unknownIn("FEEDBACK");
    out += "[CONTROLS]\n";
    line("SCENE_LAUNCH", controlText(p.sceneLaunch));
    line("STOP_TRACK", controlText(p.stopTrack));
    line("STOP_ALL", controlText(p.stopAll));
    line("TAP_TEMPO", controlText(p.tapTempo));
    unknownIn("CONTROLS");
    for (const std::string& entry : p.unknown) {
        const std::size_t bar = entry.find('|');
        const std::string section = entry.substr(0, bar);
        if (section != "PAD_PROFILE" && section != "FEEDBACK" && section != "CONTROLS" &&
            bar + 1 >= entry.size()) {
            out += "[" + section + "]\n";
            for (const std::string& inner : p.unknown) {
                if (inner.starts_with(section + "|") && inner.size() > section.size() + 1) {
                    out += inner.substr(section.size() + 1) + "\n";
                }
            }
        }
    }
    return out;
}

float padGain(std::uint8_t velocity, std::string_view curve) noexcept {
    const float v = std::clamp(static_cast<float>(velocity) / 127.0F, 1.0F / 127.0F, 1.0F);
    if (curve == "soft") {
        return std::sqrt(v);
    }
    if (curve == "hard") {
        return v * v;
    }
    return v;
}

PadMap compilePadMap(const PadProfile& p) {
    PadMap map;
    map.channel = p.channel == 0 ? PadMap::kAnyChannel : static_cast<std::uint8_t>(p.channel - 1);
    map.consumeAll = p.consumeAll;
    map.velocitySensitive = p.velocitySensitive;
    for (std::size_t v = 0; v < map.gain.size(); ++v) {
        map.gain.at(v) = padGain(static_cast<std::uint8_t>(v), p.velocityCurve);
    }
    for (std::uint32_t r = 0; r < p.rows && r < 256; ++r) {
        for (std::uint32_t c = 0; c < p.columns && c < 256; ++c) {
            const int note = p.noteAt(c, r);
            if (note >= 0) {
                map.notes.at(static_cast<std::size_t>(note)) =
                    PadAction{.kind = PadAction::Kind::Cell,
                              .track = static_cast<std::uint8_t>(c),
                              .row = static_cast<std::uint8_t>(r)};
            }
        }
    }
    const auto place = [&map](const PadControl& control, PadAction::Kind kind, bool perRow) {
        for (std::size_t i = 0; i < control.numbers.size(); ++i) {
            auto& slots = control.cc ? map.controls : map.notes;
            slots.at(control.numbers[i] & 0x7FU) =
                PadAction{.kind = kind,
                          .track = static_cast<std::uint8_t>(perRow ? 0 : i),
                          .row = static_cast<std::uint8_t>(perRow ? i : 0)};
        }
    };
    place(p.sceneLaunch, PadAction::Kind::Scene, true);
    place(p.stopTrack, PadAction::Kind::StopTrack, false);
    place(p.stopAll, PadAction::Kind::StopAll, false);
    place(p.tapTempo, PadAction::Kind::TapTempo, false);
    return map;
}

bool profileMatches(const PadProfile& profile, std::string_view port) {
    return !profile.matchDevice.empty() && port.find(profile.matchDevice) != std::string_view::npos;
}

// --- learn mode ---

PadLearn::PadLearn(std::string name, std::uint32_t columns, std::uint32_t rows) {
    m_profile.name = std::move(name);
    m_profile.columns = std::clamp<std::uint32_t>(columns, 1, 64);
    m_profile.rows = std::clamp<std::uint32_t>(rows, 1, 64);
}

std::string PadLearn::prompt() const {
    switch (m_stage) {
    case Stage::Grid:
        return "pad " + std::to_string((m_index % m_profile.columns) + 1) + ", " +
               std::to_string((m_index / m_profile.columns) + 1);
    case Stage::Scenes:
        return "scene " + std::to_string(m_index + 1);
    case Stage::StopAll:
        return "stop all";
    case Stage::Tap:
        return "tap tempo";
    case Stage::Done:
        break;
    }
    return {};
}

bool PadLearn::done() const noexcept {
    return m_stage == Stage::Done;
}

void PadLearn::advance() {
    ++m_index;
    if (m_stage == Stage::Grid && m_index >= m_profile.columns * m_profile.rows) {
        m_stage = Stage::Scenes;
        m_index = 0;
    } else if (m_stage == Stage::Scenes && m_index >= m_profile.rows) {
        m_stage = Stage::StopAll;
        m_index = 0;
    } else if (m_stage == Stage::StopAll) {
        m_stage = Stage::Tap;
        m_index = 0;
    } else if (m_stage == Stage::Tap) {
        m_stage = Stage::Done;
    }
}

void PadLearn::skip() {
    if (m_stage == Stage::Scenes) {
        // The device has no scene buttons: all of them are skipped at once.
        m_scenes.clear();
        m_sceneSkipped = true;
        m_stage = Stage::StopAll;
        m_index = 0;
        return;
    }
    if (m_stage == Stage::Grid) {
        return; // every pad of the grid is needed
    }
    advance();
}

bool PadLearn::press(const ControlMessage& message) {
    if (m_stage == Stage::Done) {
        return false;
    }
    const bool note = message.kind == project::MidiControlKind::Note;
    const bool cc = message.kind == project::MidiControlKind::ControlChange;
    if ((note && !message.pressed) || (cc && message.value <= 0.0F) || (!note && !cc)) {
        return false;
    }
    const auto used = [&](std::uint8_t number, bool isCc) {
        const auto in = [&](const std::vector<std::uint8_t>& list) {
            return std::ranges::find(list, number) != list.end();
        };
        if (!isCc && in(m_grid)) {
            return true;
        }
        if (isCc == m_scenesCc && in(m_scenes)) {
            return true;
        }
        const auto taken = [&](const PadControl& c) { return c.cc == isCc && in(c.numbers); };
        return taken(m_profile.stopAll) || taken(m_profile.tapTempo);
    };
    if (used(message.number, cc)) {
        return false;
    }
    if (m_stage == Stage::Grid) {
        if (!note) {
            return false;
        }
        if (m_grid.empty()) {
            m_profile.channel = static_cast<std::uint8_t>(message.channel + 1);
        }
        m_grid.push_back(message.number);
    } else if (m_stage == Stage::Scenes) {
        if (m_scenes.empty()) {
            m_scenesCc = cc;
        } else if (m_scenesCc != cc) {
            return false;
        }
        m_scenes.push_back(message.number);
    } else if (m_stage == Stage::StopAll) {
        m_profile.stopAll = PadControl{.cc = cc, .numbers = {message.number}};
    } else if (m_stage == Stage::Tap) {
        m_profile.tapTempo = PadControl{.cc = cc, .numbers = {message.number}};
    }
    advance();
    return true;
}

PadProfile PadLearn::result() const {
    PadProfile p = m_profile;
    const std::uint32_t columns = p.columns;
    // A regular grid: one base and two strides account for every pad.
    bool regular = m_grid.size() == static_cast<std::size_t>(columns) * p.rows;
    if (regular) {
        const int base = m_grid[0];
        const int across = columns > 1 ? m_grid[1] - base : 1;
        const int down = p.rows > 1 ? m_grid[columns] - base : 8;
        for (std::uint32_t r = 0; r < p.rows && regular; ++r) {
            for (std::uint32_t c = 0; c < columns && regular; ++c) {
                regular = m_grid[(r * columns) + c] ==
                          base + (static_cast<int>(r) * down) + (static_cast<int>(c) * across);
            }
        }
        if (regular) {
            p.noteBase = base;
            p.columnStride = across;
            p.rowStride = down;
            p.notes.clear();
        }
    }
    if (!regular) {
        p.notes = m_grid;
    }
    if (!m_sceneSkipped && !m_scenes.empty()) {
        p.sceneLaunch = PadControl{.cc = m_scenesCc, .numbers = m_scenes};
    }
    return p;
}

} // namespace adx::midi

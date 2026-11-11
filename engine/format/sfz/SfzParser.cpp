// adx-thread: main
#include "engine/format/sfz/SfzParser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>

namespace adx::format::sfz {
namespace {

using project::LoopMode;
using project::ZoneTrigger;

constexpr int kMaxIncludeDepth = 8;

struct Item {
    bool header{false};
    std::string name;
    std::string value;
    std::uint32_t line{0};
    std::uint32_t column{0};
};

std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

bool isWord(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool number(std::string_view text, double& out) noexcept {
    text = trimmed(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

/// A key: a MIDI number, or a note name with c4 = 60.
bool keyOf(std::string_view text, int& out) noexcept {
    text = trimmed(text);
    double value = 0.0;
    if (number(text, value)) {
        out = static_cast<int>(value);
        return true;
    }
    if (text.empty()) {
        return false;
    }
    static constexpr std::array<int, 7> kSemitone{9, 11, 0, 2, 4, 5, 7}; // a b c d e f g
    const char letter = static_cast<char>(text.front() | 0x20);
    if (letter < 'a' || letter > 'g') {
        return false;
    }
    int pitch = kSemitone.at(static_cast<std::size_t>(letter - 'a'));
    text.remove_prefix(1);
    if (!text.empty() && text.front() == '#') {
        ++pitch;
        text.remove_prefix(1);
    } else if (!text.empty() && (text.front() == 'b' || text.front() == 'B')) {
        --pitch;
        text.remove_prefix(1);
    }
    double octave = 0.0;
    if (!number(text, octave)) {
        return false;
    }
    out = ((static_cast<int>(octave) + 1) * 12) + pitch;
    return true;
}

std::uint8_t midi(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 127));
}

/// `name` with its digits folded to N: amplitude_oncc11 and eg06_pitch are matched as
/// amplitude_onccN and egN_pitch.
std::string folded(std::string_view name) {
    std::string out;
    bool digits = false;
    for (const char c : name) {
        if (c >= '0' && c <= '9') {
            if (!digits) {
                out.push_back('N');
            }
            digits = true;
        } else {
            out.push_back(c);
            digits = false;
        }
    }
    return out;
}

enum class Handling : std::uint8_t { Applied, Ignored, Approximated, Unknown };

struct Known {
    std::string_view name;
    Handling handling;
    std::string_view detail;
};

// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
constexpr auto kKnown = std::to_array<Known>({
    {"sample", Handling::Applied, ""},
    {"default_path", Handling::Applied, ""},
    {"note_offset", Handling::Applied, ""},
    {"octave_offset", Handling::Applied, ""},
    {"lokey", Handling::Applied, ""},
    {"hikey", Handling::Applied, ""},
    {"key", Handling::Applied, ""},
    {"pitch_keycenter", Handling::Applied, ""},
    {"lovel", Handling::Applied, ""},
    {"hivel", Handling::Applied, ""},
    {"tune", Handling::Applied, ""},
    {"transpose", Handling::Applied, ""},
    {"volume", Handling::Applied, ""},
    {"pan", Handling::Applied, ""},
    {"offset", Handling::Applied, ""},
    {"end", Handling::Applied, ""},
    {"loop_mode", Handling::Applied, ""},
    {"loopmode", Handling::Applied, ""},
    {"loop_start", Handling::Applied, ""},
    {"loopstart", Handling::Applied, ""},
    {"loop_end", Handling::Applied, ""},
    {"loopend", Handling::Applied, ""},
    {"seq_length", Handling::Applied, ""},
    {"seq_position", Handling::Applied, ""},
    {"trigger", Handling::Applied, ""},
    {"rt_decay", Handling::Applied, ""},
    {"group", Handling::Applied, ""},
    {"off_by", Handling::Applied, ""},
    {"offby", Handling::Applied, ""},
    {"sw_lokey", Handling::Applied, ""},
    {"sw_hikey", Handling::Applied, ""},
    {"sw_last", Handling::Applied, ""},
    {"xfin_lovel", Handling::Applied, ""},
    {"xfin_hivel", Handling::Applied, ""},
    {"xfout_lovel", Handling::Applied, ""},
    {"xfout_hivel", Handling::Applied, ""},
    {"ampeg_attack", Handling::Applied, ""},
    {"ampeg_decay", Handling::Applied, ""},
    {"ampeg_sustain", Handling::Applied, ""},
    {"ampeg_release", Handling::Applied, ""},
    {"amp_veltrack", Handling::Applied, ""},
    {"lorand", Handling::Approximated, "random selection plays as a round robin"},
    {"hirand", Handling::Approximated, "random selection plays as a round robin"},
    {"sw_default", Handling::Ignored, "the lowest switch key is the default"},
    {"sw_label", Handling::Ignored, "a label"},
    {"group_label", Handling::Ignored, "a label"},
    {"region_label", Handling::Ignored, "a label"},
    {"master_label", Handling::Ignored, "a label"},
    {"global_label", Handling::Ignored, "a label"},
    {"label_ccN", Handling::Ignored, "a label"},
    {"set_ccN", Handling::Ignored, "a controller's power-on value"},
    {"set_hdccN", Handling::Ignored, "a controller's power-on value"},
    {"curve_index", Handling::Ignored, "a <curve> table"},
    {"vN", Handling::Ignored, "a <curve> table"},
    {"ampeg_dynamic", Handling::Ignored, "the envelope is read at the note-on"},
    {"note_polyphony", Handling::Ignored, "polyphony is the channel's"},
    {"polyphony", Handling::Ignored, "polyphony is the channel's"},
    {"off_mode", Handling::Ignored, "a choke fades in 2 ms"},
    {"off_time", Handling::Ignored, "a choke fades in 2 ms"},
    {"pitch_keytrack", Handling::Approximated, "every zone tracks the keyboard"},
    {"amplitude_onccN", Handling::Approximated, "controller-driven level is not applied"},
    {"amplitude_curveccN", Handling::Approximated, "controller-driven level is not applied"},
    {"pan_onccN", Handling::Approximated, "controller-driven pan is not applied"},
    {"pan_curveccN", Handling::Approximated, "controller-driven pan is not applied"},
    {"loccN", Handling::Approximated, "the zone plays whatever the controller's value"},
    {"hiccN", Handling::Approximated, "the zone plays whatever the controller's value"},
    {"on_loccN", Handling::Approximated, "a controller-triggered region is not imported"},
    {"on_hiccN", Handling::Approximated, "a controller-triggered region is not imported"},
});
// NOLINTEND(modernize-use-designated-initializers)

const Known* knownOpcode(std::string_view name) {
    const auto exact = std::ranges::find(kKnown, name, &Known::name);
    if (exact != kKnown.end()) {
        return &*exact;
    }
    const std::string shape = folded(name);
    const auto match = std::ranges::find(kKnown, std::string_view(shape), &Known::name);
    return match == kKnown.end() ? nullptr : &*match;
}

/// ARIA's flexible envelopes and LFOs: they need a per-zone articulation the sampler
/// does not have.
bool isAriaModulation(std::string_view name) {
    const std::string shape = folded(name);
    return shape.starts_with("egN_") || shape.starts_with("lfoN_");
}

class Lexer {
public:
    Lexer(const IncludeReader& include, std::vector<Note>& notes)
        : m_include(include), m_notes(notes) {}

    // NOLINTNEXTLINE(misc-no-recursion) - an #include is lexed in place, to a fixed depth.
    void run(std::string_view text, int depth) {
        std::uint32_t lineNumber = 0;
        while (!text.empty()) {
            const std::size_t end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            ++lineNumber;
            if (const std::size_t comment = line.find("//"); comment != std::string_view::npos) {
                line = line.substr(0, comment);
            }
            const std::string_view content = trimmed(line);
            if (content.starts_with("#define")) {
                define(content.substr(7));
            } else if (!content.empty()) {
                tokens(substituted(line), lineNumber, depth);
            }
        }
    }

    [[nodiscard]] std::vector<Item>& items() noexcept {
        return m_items;
    }

private:
    void define(std::string_view rest) {
        rest = trimmed(rest);
        const std::size_t space = rest.find_first_of(" \t");
        if (rest.empty() || rest.front() != '$' || space == std::string_view::npos) {
            return;
        }
        m_defines[std::string(rest.substr(0, space))] = std::string(trimmed(rest.substr(space)));
    }

    /// An `#include "file"` at `at` in `line`, which may sit between opcodes. Lexes the
    /// file in place and returns where the line continues.
    // NOLINTNEXTLINE(misc-no-recursion)
    std::size_t include(std::string_view line, std::size_t at, std::uint32_t lineNumber,
                        int depth) {
        const std::size_t open = line.find('"', at);
        const std::size_t close = open == std::string_view::npos ? open : line.find('"', open + 1);
        std::string text;
        if (close == std::string_view::npos || depth >= kMaxIncludeDepth || !m_include ||
            !m_include(line.substr(open + 1, close - open - 1), text)) {
            m_notes.push_back(Note{.kind = NoteKind::Unknown,
                                   .opcode = "#include",
                                   .detail = "the file could not be read",
                                   .line = lineNumber,
                                   .column = static_cast<std::uint32_t>(at + 1)});
            return close == std::string_view::npos ? line.size() : close + 1;
        }
        // The included file's items keep its own line numbers; the caller's vector is
        // appended to in place.
        run(text, depth + 1);
        return close + 1;
    }

    /// `line` with every $NAME replaced, longest name first.
    [[nodiscard]] std::string substituted(std::string_view line) const {
        std::string out(line);
        if (m_defines.empty() || out.find('$') == std::string::npos) {
            return out;
        }
        std::vector<std::pair<std::string, std::string>> ordered(m_defines.begin(),
                                                                 m_defines.end());
        std::ranges::sort(
            ordered, [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        for (const auto& [name, value] : ordered) {
            for (std::size_t at = out.find(name); at != std::string::npos;
                 at = out.find(name, at + value.size())) {
                out.replace(at, name.size(), value);
            }
        }
        return out;
    }

    /// Where the value that starts at `from` ends: before the next `word=`, the next
    /// header, or the end of the line. A sample's path may hold spaces.
    [[nodiscard]] static std::size_t valueEnd(std::string_view line, std::size_t from) {
        for (std::size_t i = from; i < line.size(); ++i) {
            if (line[i] == '<' || line.substr(i).starts_with("#include")) {
                return i;
            }
            if (line[i] != ' ' && line[i] != '\t') {
                continue;
            }
            std::size_t word = i;
            while (word < line.size() && (line[word] == ' ' || line[word] == '\t')) {
                ++word;
            }
            std::size_t after = word;
            while (after < line.size() && isWord(line[after])) {
                ++after;
            }
            if (after > word && after < line.size() && line[after] == '=') {
                return i;
            }
            if (word < line.size() && line[word] == '<') {
                return i;
            }
        }
        return line.size();
    }

    // NOLINTNEXTLINE(misc-no-recursion)
    void tokens(const std::string& line, std::uint32_t lineNumber, int depth) {
        const std::string_view view = line;
        std::size_t at = 0;
        while (at < view.size()) {
            if (view[at] == ' ' || view[at] == '\t' || view[at] == '\r') {
                ++at;
                continue;
            }
            if (view.substr(at).starts_with("#include")) {
                at = include(view, at, lineNumber, depth);
                continue;
            }
            const auto column = static_cast<std::uint32_t>(at + 1);
            if (view[at] == '<') {
                const std::size_t close = view.find('>', at);
                if (close == std::string_view::npos) {
                    return;
                }
                m_items.push_back(Item{.header = true,
                                       .name = std::string(view.substr(at + 1, close - at - 1)),
                                       .value = {},
                                       .line = lineNumber,
                                       .column = column});
                at = close + 1;
                continue;
            }
            const std::size_t equals = view.find('=', at);
            if (equals == std::string_view::npos) {
                return; // stray text: nothing SFZ can mean by it
            }
            const std::size_t end = valueEnd(view, equals + 1);
            m_items.push_back(
                Item{.header = false,
                     .name = std::string(trimmed(view.substr(at, equals - at))),
                     .value = std::string(trimmed(view.substr(equals + 1, end - equals - 1))),
                     .line = lineNumber,
                     .column = column});
            at = end;
        }
    }

    const IncludeReader& m_include;
    std::vector<Note>& m_notes;
    std::map<std::string, std::string> m_defines;
    std::vector<Item> m_items;
};

using Opcodes = std::vector<std::pair<std::string, std::string>>;

/// What a region carries beyond its zone until every region is in.
struct Draft {
    Region region;
    int sequenceLength{0};
    int sequencePosition{0};
    double random{-1.0};
    /// `tune` in cents and `transpose` in semitones: each replaces what it inherited,
    /// and the zone's tuning is their sum.
    double tune{0.0};
    double transpose{0.0};
    bool hasKeycenter{false};
    bool skip{false};
};

class Builder {
public:
    explicit Builder(Import& out) : m_out(out) {}

    void feed(const Item& item) {
        if (item.header) {
            header(item);
            return;
        }
        note(item);
        switch (m_scope) {
        case Scope::kControl:
            control(item);
            break;
        case Scope::kGlobal:
            m_global.emplace_back(item.name, item.value);
            break;
        case Scope::kMaster:
            m_master.emplace_back(item.name, item.value);
            break;
        case Scope::kGroup:
            m_group.emplace_back(item.name, item.value);
            break;
        case Scope::kRegion:
            m_region.emplace_back(item.name, item.value);
            break;
        case Scope::kOther:
            break;
        }
    }

    void finish() {
        closeRegion();
        assignRoundRobins();
        for (Draft& draft : m_drafts) {
            if (!draft.skip) {
                m_out.regions.push_back(std::move(draft.region));
            }
        }
    }

private:
    enum class Scope : std::uint8_t { kOther, kControl, kGlobal, kMaster, kGroup, kRegion };

    void header(const Item& item) {
        closeRegion();
        if (item.name == "control") {
            m_scope = Scope::kControl;
        } else if (item.name == "global") {
            m_scope = Scope::kGlobal;
            m_global.clear();
            m_master.clear();
            m_group.clear();
        } else if (item.name == "master") {
            m_scope = Scope::kMaster;
            m_master.clear();
            m_group.clear();
        } else if (item.name == "group") {
            m_scope = Scope::kGroup;
            m_group.clear();
        } else if (item.name == "region") {
            m_scope = Scope::kRegion;
            m_regionLine = item.line;
            m_inRegion = true;
        } else {
            // <curve>, <effect>, <midi>, <sample>: nothing a zone can hold.
            m_scope = Scope::kOther;
            if (item.name != "curve") {
                m_out.notes.push_back(Note{.kind = NoteKind::Unknown,
                                           .opcode = "<" + item.name + ">",
                                           .detail = "a header this build does not read",
                                           .line = item.line,
                                           .column = item.column});
            }
        }
    }

    /// One note per opcode the sampler does not apply, where the file wrote it.
    void note(const Item& item) {
        NoteKind kind = NoteKind::Unknown;
        std::string detail = "not an opcode this build reads";
        if (const Known* known = knownOpcode(item.name)) {
            if (known->handling == Handling::Applied) {
                return;
            }
            kind =
                known->handling == Handling::Ignored ? NoteKind::Ignored : NoteKind::Approximated;
            detail = std::string(known->detail);
        } else if (isAriaModulation(item.name)) {
            kind = NoteKind::Approximated;
            detail = "a per-region envelope or LFO; the sampler's are the instrument's";
        } else if (const std::string shape = folded(item.name); shape.ends_with("_onccN") ||
                                                                shape.ends_with("_curveccN") ||
                                                                shape.ends_with("_ccN")) {
            kind = NoteKind::Approximated;
            detail = "controller modulation of an opcode is not applied";
        }
        m_out.notes.push_back(Note{.kind = kind,
                                   .opcode = item.name,
                                   .detail = std::move(detail),
                                   .line = item.line,
                                   .column = item.column});
    }

    void control(const Item& item) {
        double value = 0.0;
        if (item.name == "default_path") {
            m_defaultPath = item.value;
            std::ranges::replace(m_defaultPath, '\\', '/');
        } else if (item.name == "note_offset" && number(item.value, value)) {
            m_noteOffset = static_cast<int>(value);
        } else if (item.name == "octave_offset" && number(item.value, value)) {
            m_noteOffset = 12 * static_cast<int>(value);
        }
    }

    void closeRegion() {
        if (!m_inRegion) {
            return;
        }
        m_inRegion = false;
        Draft draft;
        draft.region.line = m_regionLine;
        draft.region.zone.velocityLow = 0;
        for (const Opcodes* scope : {&m_global, &m_master, &m_group, &m_region}) {
            for (const auto& [name, value] : *scope) {
                apply(draft, name, value);
            }
        }
        m_region.clear();
        project::SampleZone& zone = draft.region.zone;
        zone.tuneCents = static_cast<float>(draft.tune + (100.0 * draft.transpose));
        if (!draft.hasKeycenter && zone.keyLow == zone.keyHigh) {
            zone.rootKey = zone.keyLow;
        }
        if (zone.keyLow > zone.keyHigh || zone.velocityLow > zone.velocityHigh) {
            draft.skip = true;
        }
        if (draft.region.sample.empty() || draft.region.sample.front() == '*') {
            if (!draft.region.sample.empty()) {
                m_out.notes.push_back(Note{.kind = NoteKind::Approximated,
                                           .opcode = "sample",
                                           .detail = "a generated sample (" + draft.region.sample +
                                                     ") is not imported",
                                           .line = m_regionLine,
                                           .column = 1});
            }
            draft.skip = true;
        } else {
            draft.region.sample = m_defaultPath + draft.region.sample;
        }
        m_drafts.push_back(std::move(draft));
    }

    // NOLINTNEXTLINE(readability-function-size) - one branch per opcode.
    void apply(Draft& draft, const std::string& name, const std::string& value) const {
        Region& region = draft.region;
        project::SampleZone& zone = region.zone;
        double n = 0.0;
        int key = 0;
        const bool numeric = number(value, n);
        if (name == "sample") {
            region.sample = value;
            std::ranges::replace(region.sample, '\\', '/');
        } else if (name == "lokey" && keyOf(value, key)) {
            draft.skip = draft.skip || key < 0;
            zone.keyLow = midi(key + m_noteOffset);
        } else if (name == "hikey" && keyOf(value, key)) {
            draft.skip = draft.skip || key < 0;
            zone.keyHigh = midi(key + m_noteOffset);
        } else if (name == "key" && keyOf(value, key)) {
            zone.keyLow = zone.keyHigh = midi(key + m_noteOffset);
            if (!draft.hasKeycenter) {
                zone.rootKey = zone.keyLow;
            }
        } else if (name == "pitch_keycenter" && keyOf(value, key)) {
            zone.rootKey = midi(key + m_noteOffset);
            draft.hasKeycenter = true;
        } else if (name == "lovel" && numeric) {
            zone.velocityLow = midi(static_cast<int>(n));
        } else if (name == "hivel" && numeric) {
            zone.velocityHigh = midi(static_cast<int>(n));
        } else if (name == "tune" && numeric) {
            draft.tune = n;
        } else if (name == "transpose" && numeric) {
            draft.transpose = n;
        } else if (name == "volume" && numeric) {
            zone.gainDb = static_cast<float>(n);
        } else if (name == "pan" && numeric) {
            zone.pan = std::clamp(static_cast<float>(n) / 100.0F, -1.0F, 1.0F);
        } else if (name == "offset" && numeric) {
            zone.start = static_cast<std::uint32_t>(std::max(0.0, n));
        } else if (name == "end" && numeric) {
            draft.skip = draft.skip || n < 0.0;
            zone.end = n > 0.0 ? static_cast<std::uint32_t>(n) + 1 : 0;
        } else if (name == "loop_mode" || name == "loopmode") {
            if (value == "loop_continuous") {
                zone.loop = LoopMode::Forward;
            } else if (value == "loop_sustain") {
                zone.loop = LoopMode::Sustain;
            } else {
                zone.loop = LoopMode::Off;
                region.oneShot = value == "one_shot";
            }
        } else if ((name == "loop_start" || name == "loopstart") && numeric) {
            zone.loopStart = static_cast<std::uint32_t>(std::max(0.0, n));
        } else if ((name == "loop_end" || name == "loopend") && numeric) {
            zone.loopEnd = static_cast<std::uint32_t>(std::max(0.0, n)) + 1;
        } else if (name == "seq_length" && numeric) {
            draft.sequenceLength = static_cast<int>(n);
        } else if (name == "seq_position" && numeric) {
            draft.sequencePosition = static_cast<int>(n);
        } else if (name == "lorand" && numeric) {
            draft.random = n;
        } else if (name == "trigger") {
            zone.trigger = (value == "release" || value == "release_key") ? ZoneTrigger::Release
                                                                          : ZoneTrigger::Attack;
        } else if (name == "rt_decay" && numeric) {
            zone.rtDecay = static_cast<float>(n);
        } else if (name == "group" && numeric) {
            zone.group = static_cast<std::uint16_t>(std::clamp(n, 0.0, 65535.0));
        } else if ((name == "off_by" || name == "offby") && numeric) {
            zone.offBy = static_cast<std::uint16_t>(std::clamp(n, 0.0, 65535.0));
        } else if (name == "sw_lokey" && keyOf(value, key)) {
            zone.switchLow = midi(key + m_noteOffset);
        } else if (name == "sw_hikey" && keyOf(value, key)) {
            zone.switchHigh = midi(key + m_noteOffset);
        } else if (name == "sw_last" && keyOf(value, key)) {
            zone.switchKey = midi(key + m_noteOffset);
        } else if (name == "xfin_lovel" && numeric) {
            zone.xfadeInLow = midi(static_cast<int>(n));
        } else if (name == "xfin_hivel" && numeric) {
            zone.xfadeInHigh = midi(static_cast<int>(n));
        } else if (name == "xfout_lovel" && numeric) {
            zone.xfadeOutLow = midi(static_cast<int>(n));
        } else if (name == "xfout_hivel" && numeric) {
            zone.xfadeOutHigh = midi(static_cast<int>(n));
        } else if (name == "ampeg_attack" && numeric) {
            region.attack = static_cast<float>(n);
        } else if (name == "ampeg_decay" && numeric) {
            region.decay = static_cast<float>(n);
        } else if (name == "ampeg_sustain" && numeric) {
            region.sustain = static_cast<float>(n) / 100.0F;
        } else if (name == "ampeg_release" && numeric) {
            region.release = static_cast<float>(n);
        } else if (name == "amp_veltrack" && numeric) {
            region.velocityTrack = std::clamp(static_cast<float>(n) / 100.0F, 0.0F, 1.0F);
        } else if (folded(name) == "on_loccN" || folded(name) == "on_hiccN") {
            draft.skip = true;
        }
    }

    /// seq_* and lorand regions that share keys, velocities and articulation take
    /// turns: one round-robin group per such set, up to the 255 a zone can name.
    void assignRoundRobins() {
        using Key = std::tuple<int, int, int, int, int, int>;
        std::map<Key, std::uint8_t> groups;
        std::map<Key, std::uint8_t> nextIndex;
        bool overflowed = false;
        for (Draft& draft : m_drafts) {
            const bool sequenced = draft.sequenceLength > 1;
            const bool random = draft.random >= 0.0;
            if (draft.skip || (!sequenced && !random)) {
                continue;
            }
            const project::SampleZone& zone = draft.region.zone;
            const Key key{zone.keyLow,       zone.keyHigh,   zone.velocityLow,
                          zone.velocityHigh, zone.switchKey, static_cast<int>(zone.trigger)};
            auto found = groups.find(key);
            if (found == groups.end()) {
                if (groups.size() >= 255) {
                    overflowed = true;
                    continue;
                }
                found = groups.emplace(key, static_cast<std::uint8_t>(groups.size() + 1)).first;
            }
            draft.region.zone.roundRobinGroup = found->second;
            draft.region.zone.roundRobinIndex =
                sequenced ? midi(draft.sequencePosition - 1) : nextIndex[key]++;
        }
        if (overflowed) {
            m_out.notes.push_back(
                Note{.kind = NoteKind::Approximated,
                     .opcode = "seq_length",
                     .detail = "more than 255 round-robin sets; the rest play their first sample",
                     .line = 0,
                     .column = 0});
        }
    }

    Import& m_out;
    Scope m_scope{Scope::kOther};
    Opcodes m_global;
    Opcodes m_master;
    Opcodes m_group;
    Opcodes m_region;
    std::string m_defaultPath;
    int m_noteOffset{0};
    bool m_inRegion{false};
    std::uint32_t m_regionLine{0};
    std::vector<Draft> m_drafts;
};

} // namespace

std::size_t Import::count(NoteKind kind) const noexcept {
    return static_cast<std::size_t>(std::ranges::count(notes, kind, &Note::kind));
}

Import parseSfz(std::string_view text, const IncludeReader& include) {
    Import out;
    Lexer lexer(include, out.notes);
    lexer.run(text, 0);
    Builder builder(out);
    for (const Item& item : lexer.items()) {
        builder.feed(item);
    }
    builder.finish();
    return out;
}

bool loadSfz(const std::filesystem::path& path, Import& out) {
    const auto read = [](const std::filesystem::path& file, std::string& text) {
        const std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        text = buffer.str();
        return true;
    };
    std::string text;
    if (!read(path, text)) {
        return false;
    }
    const std::filesystem::path directory = path.parent_path();
    out = parseSfz(text, [&](std::string_view name, std::string& included) {
        std::string relative(name);
        std::ranges::replace(relative, '\\', '/');
        return read(directory / relative, included);
    });
    return true;
}

std::string reportOf(const Import& import) {
    std::string out;
    for (const Note& note : import.notes) {
        const char* kind = "unknown";
        if (note.kind == NoteKind::Ignored) {
            kind = "ignored";
        } else if (note.kind == NoteKind::Approximated) {
            kind = "approximated";
        }
        out += std::to_string(note.line) + ":" + std::to_string(note.column) + " " + kind + " " +
               note.opcode + " - " + note.detail + "\n";
    }
    return out;
}

} // namespace adx::format::sfz

#include "engine/format/adx/V1Shim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine/format/adx/NoteName.h"
#include "engine/format/adx/Value.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"
#include "engine/project/Validate.h"
#include "engine/project/commands/AutomationCommands.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"

namespace adx::format {
namespace {

using project::Project;

/// One v1 positional tuple and the v2 parameter names its fields become.
struct TupleMapping {
    std::string_view key;
    std::array<std::string_view, 5> names;
    std::size_t count{0};
};

/// docs/adx-format-v2.md §12.1, exhaustive. Anything not here stays residue.
// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table:
// one row per line, columns aligned by position. Designating every field would
// triple its width and bury the values the table exists to show.
constexpr auto kPatchTuples = std::to_array<TupleMapping>({
    {"ENVELOPE", {"env.attack", "env.decay", "env.sustain", "env.release", ""}, 4},
    {"DRIVE", {"drive", "", "", "", ""}, 1},
    {"FILTER", {"filter.cutoff", "filter.lfoRate", "filter.lfoDepth", "", ""}, 3},
    {"SUB", {"sub.level", "sub.wave", "sub.dropSemitones", "sub.dropMs", ""}, 4},
    {"NOISE", {"noise.level", "noise.type", "", "", ""}, 2},
    {"RESFILTER",
     {"resfilter.type", "resfilter.cutoff", "resfilter.resonance", "resfilter.envAmount",
      "resfilter.keyTrack"},
     5},
    {"FILTERENV",
     {"filterEnv.attack", "filterEnv.decay", "filterEnv.sustain", "filterEnv.release", ""},
     4},
    {"FORMANT", {"formant.vowelA", "formant.vowelB", "formant.morph", "formant.amount", ""}, 4},
    {"VIBRATO", {"vibrato.rate", "vibrato.depthCents", "vibrato.delayMs", "", ""}, 3},
    {"GLIDE", {"glide.ms", "", "", "", ""}, 1},
    {"OSC", {"osc.wave", "osc.unison", "osc.detuneCents", "osc.pulseWidth", ""}, 4},
});

/// v1's per-track effects and the named parameters their positional arguments
/// become. v1's own comments and its SaveProject give the order.
struct EffectMapping {
    std::string_view type;
    std::array<std::string_view, 3> names;
    std::size_t count{0};
};

constexpr auto kEffectMappings = std::to_array<EffectMapping>({
    {"Reverb", {"mix", "room", "damp"}, 3},
    {"Distortion", {"drive", "mix", ""}, 2},
    {"Bitcrush", {"bits", "rate", "mix"}, 3},
    {"Chorus", {"rate", "depth", "mix"}, 3},
    {"EQ", {"low", "mid", "high"}, 3},
});
// NOLINTEND(modernize-use-designated-initializers)

/// v1 automation field names to v2 parameter names, for `patch.<field>` targets.
constexpr auto kPatchFieldAliases = std::to_array<std::pair<std::string_view, std::string_view>>({
    {"attackMs", "env.attack"},
    {"decayMs", "env.decay"},
    {"sustainLevel", "env.sustain"},
    {"releaseMs", "env.release"},
    {"drive", "drive"},
    {"filterCutoffHz", "filter.cutoff"},
    {"filterLfoRateHz", "filter.lfoRate"},
    {"filterLfoDepth", "filter.lfoDepth"},
    {"subOscLevel", "sub.level"},
    {"pitchDropSemitones", "sub.dropSemitones"},
    {"pitchDropMs", "sub.dropMs"},
    {"noiseLevel", "noise.level"},
    {"resFilterCutoff", "resfilter.cutoff"},
    {"resFilterResonance", "resfilter.resonance"},
    {"filterEnvAmount", "resfilter.envAmount"},
    {"keyTrack", "resfilter.keyTrack"},
    {"formantMorph", "formant.morph"},
    {"formantAmount", "formant.amount"},
    {"vibratoRateHz", "vibrato.rate"},
    {"vibratoDepthCents", "vibrato.depthCents"},
    {"glideMs", "glide.ms"},
    {"oscDetuneCents", "osc.detuneCents"},
    {"oscPulseWidth", "osc.pulseWidth"},
});

/// v1 wrote formant vowels as letters. The v2 model holds a number, so the letters
/// become indices in the order v1's own vowel table used.
/// v1's FORMANT vowel names onto the formant bank's a e i o u (0..4).
///
/// By whole name, not first letter: v1 had six names (AudioEngine.cpp,
/// GetVowelFormants), and keying on the first letter sent `Oo` to `Oh` and `Ee` to
/// `Eh` - two different vowels each - which is how Phase 2 shipped it. `Uh`, v1's
/// schwa, is nearest `Ah` and goes there; anything unrecognised was `Ah` in v1 too.
[[nodiscard]] double vowelIndex(std::string_view text) noexcept {
    // ASCII case-insensitive, without building a lowered copy.
    const auto is = [text](std::string_view name) noexcept {
        return std::ranges::equal(text, name, [](char a, char b) {
            const char lower = a >= 'A' && a <= 'Z' ? static_cast<char>(a + ('a' - 'A')) : a;
            return lower == b;
        });
    };
    if (is("eh") || is("e")) {
        return 1.0;
    }
    if (is("ee") || is("i")) {
        return 2.0;
    }
    if (is("oh") || is("o")) {
        return 3.0;
    }
    if (is("oo") || is("u")) {
        return 4.0;
    }
    return 0.0; // ah, uh, and anything else
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::vector<std::string_view> splitOn(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(delimiter, start);
        if (at == std::string_view::npos) {
            parts.push_back(trim(text.substr(start)));
            return parts;
        }
        parts.push_back(trim(text.substr(start, at - start)));
        start = at + 1;
    }
}

// --- the intermediate form ---------------------------------------------------
//
// Collected first, built second. v1 compiles its `PATTERN=` lines only after
// `LOOP=` has definitely been read, regardless of where in the file each appears,
// and a project whose tracks reference patches declared later has to work too. One
// gathering pass makes both fall out instead of needing special cases.

struct RawPoint {
    double beat{0.0};
    double value{0.0};
    std::string curve;
};

struct RawLane {
    std::string track;
    std::string param;
    std::vector<RawPoint> points;
    Span span;
    /// Claimed only if the lane resolves. An unresolvable target is preserved as
    /// residue, which is the honest answer when v2 has no equivalent parameter.
    std::size_t section{0};
    std::vector<std::size_t> lineIndices;
};

struct RawEffect {
    std::string type;
    std::vector<double> args;
    Span span;
};

struct RawNote {
    std::uint8_t pitch{60};
    double startBeat{0.0};
    double lengthBeats{1.0};
    double velocity{1.0};
};

struct RawClip {
    std::string path;
    double startSeconds{0.0};
    double pitchSemitones{0.0};
    double stretch{1.0};
    bool reverse{false};
};

struct RawTrack {
    std::string patchName;
    double volume{1.0};
    double pan{0.0};
    double sendDelay{0.0};
    double sendReverb{0.0};
    bool hasArp{false};
    int arpMode{0};
    double arpRateBeats{0.25};
    int arpOctaves{1};
    double arpGate{0.8};
    std::vector<RawEffect> effects;
    std::vector<RawNote> notes;
    std::vector<RawClip> clips;
    std::vector<std::string> mini;
    Span span;
};

struct RawGlobal {
    double bpm{120.0};
    double masterVolume{1.0};
    double tuning{440.0};
    double masterDrive{0.0};
    bool hasDelay{false};
    std::array<double, 3> delay{375.0, 0.4, 0.0};
    bool hasReverb{false};
    std::array<double, 3> reverb{0.5, 0.5, 0.0};
    bool hasSidechain{false};
    std::array<double, 3> sidechain{0.0, 0.5, 120.0};
    bool hasLoop{false};
    double loopStart{0.0};
    double loopEnd{0.0};
    std::vector<std::pair<double, std::string>> markers;
};

struct RawPatch {
    std::string name;
    std::vector<project::ParamValue> params;
};

} // namespace

// The collector and the builder are both long enough to deserve their own type, and
// both are implementation detail, so they stay in the anonymous namespace below.
namespace {

class V1Migration {
public:
    V1Migration(Document& document, Project& project, project::CommandStack& stack,
                DiagnosticList& diagnostics, const ParseOptions& options)
        : m_doc(document), m_project(project), m_stack(stack), m_diag(diagnostics),
          m_options(options) {}

    void run();

private:
    [[nodiscard]] std::span<Line> lines() noexcept {
        return m_doc.lines();
    }
    void execute(std::unique_ptr<project::Command> command) {
        m_stack.execute(std::move(command), m_project);
    }
    [[nodiscard]] static core::Ticks ticksOfBeats(double beats) noexcept {
        return core::Ticks{std::llround(beats * static_cast<double>(core::kPpq))};
    }

    void collect();
    void collectGlobal(Section& section);
    void collectPatch(Section& section);
    void collectTrack(Section& section);
    void collectAutomation(Section& section, std::size_t index);

    void build();
    void buildMaster();
    void buildTrack(const RawTrack& track);
    [[nodiscard]] core::InsertId ensureBus(std::string_view name);
    void buildAutomation();
    [[nodiscard]] std::string v2PathFor(const RawLane& lane);
    [[nodiscard]] std::string uniqueName(std::string_view wanted, Span span);

    Document& m_doc;
    Project& m_project;
    project::CommandStack& m_stack;
    DiagnosticList& m_diag;
    const ParseOptions& m_options;

    RawGlobal m_global;
    std::vector<RawPatch> m_patches;
    std::vector<RawTrack> m_tracks;
    std::vector<RawLane> m_lanes;

    core::InsertId m_master;
    /// Where tracks route: the "Master FX" insert carrying v1's master delay and
    /// reverb when the file had either, else the master itself.
    core::InsertId m_mixTarget;
    /// Which insert each master slot type landed on, for automation paths.
    std::unordered_map<std::string, core::InsertId> m_masterSlotOwners;
    std::unordered_map<std::string, core::InsertId> m_buses;
    /// Per v1 track name: the insert, channel and pattern it expanded into.
    // MSVC's unordered_map allocates a sentinel node in its default constructor,
    // which the implicit constructor below inherits. Nothing can be done about
    // that short of a different container, and this one is main-thread only.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    struct Expanded {
        core::InsertId insert;
        core::ChannelId channel;
        core::PatternId pattern;
        std::unordered_map<std::string, core::SlotId> slotsByType;
    };
    void buildTrackMixer(const RawTrack& track, const std::string& name, Expanded& expanded);
    void buildTrackPlaylist(const RawTrack& track, const std::string& name,
                            const Expanded& expanded);
    std::unordered_map<std::string, Expanded> m_expanded;
    std::unordered_map<std::string, core::SlotId> m_masterSlots;
    /// The first [TRACK]'s insert: v1's sidechain key, always (AudioEngine.cpp,
    /// "Track 1's bus ducks the master").
    core::InsertId m_firstTrackInsert;
    std::unordered_set<std::string> m_usedNames;
    bool m_velocityRescaled{false};
};

// --- collection --------------------------------------------------------------

void V1Migration::collectGlobal(Section& section) {
    section.claimed = true;
    for (const std::size_t index : section.lineIndices) {
        Line& line = lines()[index];
        if (line.kind != Line::Kind::KeyValue) {
            continue;
        }
        const std::string_view value = line.value;
        const auto number = [&](double& target) {
            double parsed = 0.0;
            if (parseDouble(value, line.valueSpan, m_diag, parsed)) {
                target = parsed;
            }
        };
        const auto tuple = [&](std::array<double, 3>& target, bool& present) {
            const std::vector<std::string_view> parts = splitOn(value, ',');
            if (parts.size() < 3) {
                m_diag.add(code::kWrongFieldCount, line.valueSpan, "expected three values");
                return;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                double parsed = 0.0;
                if (parseDouble(parts[i], line.valueSpan, m_diag, parsed)) {
                    target.at(i) = parsed;
                }
            }
            present = true;
        };

        if (line.key == "BPM") {
            number(m_global.bpm);
        } else if (line.key == "MASTER_VOL") {
            number(m_global.masterVolume);
        } else if (line.key == "TUNING") {
            number(m_global.tuning);
        } else if (line.key == "MASTER_DRIVE") {
            number(m_global.masterDrive);
        } else if (line.key == "DELAY") {
            tuple(m_global.delay, m_global.hasDelay);
        } else if (line.key == "REVERB") {
            tuple(m_global.reverb, m_global.hasReverb);
        } else if (line.key == "SIDECHAIN") {
            tuple(m_global.sidechain, m_global.hasSidechain);
        } else if (line.key == "LOOP") {
            const std::vector<std::string_view> parts = splitOn(value, ',');
            if (parts.size() >= 2 &&
                parseDouble(parts[0], line.valueSpan, m_diag, m_global.loopStart) &&
                parseDouble(parts[1], line.valueSpan, m_diag, m_global.loopEnd)) {
                m_global.hasLoop = true;
                m_diag.add(code::kV1LoopEnabled, line.span(),
                           "LOOP= is present, so the loop is enabled - v1's rule, preserved");
            }
        } else if (line.key == "MARKER") {
            // v1 split on the FIRST comma only, so a marker name may contain commas.
            // Preserved exactly: `MARKER=16,Drop, part 2` is beat 16, "Drop, part 2".
            const std::size_t comma = value.find(',');
            if (comma == std::string_view::npos) {
                m_diag.add(code::kWrongFieldCount, line.valueSpan, "expected beat,name");
                continue;
            }
            double beat = 0.0;
            if (parseDouble(value.substr(0, comma), line.valueSpan, m_diag, beat)) {
                m_global.markers.emplace_back(beat, std::string(trim(value.substr(comma + 1))));
            }
        } else {
            continue;
        }
        line.claimed = true;
    }
}

void V1Migration::collectPatch(Section& section) {
    section.claimed = true;
    RawPatch patch;
    patch.name = section.name;

    for (const std::size_t index : section.lineIndices) {
        Line& line = lines()[index];
        if (line.kind != Line::Kind::KeyValue) {
            continue;
        }
        const std::vector<std::string_view> parts = splitOn(line.value, ',');

        if (line.key == "HARMONICS") {
            for (std::size_t i = 0; i < parts.size() && i < 16; ++i) {
                double value = 0.0;
                if (parseDouble(parts[i], line.valueSpan, m_diag, value)) {
                    patch.params.push_back(
                        project::ParamValue{.name = "harmonic." + std::to_string(i + 1),
                                            .value = value,
                                            .hasCurve = false,
                                            .curve = {}});
                }
            }
            line.claimed = true;
            continue;
        }

        const auto mapping = std::ranges::find(kPatchTuples, line.key, &TupleMapping::key);
        if (mapping == kPatchTuples.end()) {
            continue;
        }
        if (parts.size() < mapping->count) {
            m_diag.add(code::kWrongFieldCount, line.valueSpan,
                       std::string(mapping->key) + " needs " + std::to_string(mapping->count) +
                           " values");
            continue;
        }
        for (std::size_t i = 0; i < mapping->count; ++i) {
            const std::string_view name = mapping->names.at(i);
            double value = 0.0;
            if (name == "formant.vowelA" || name == "formant.vowelB") {
                value = vowelIndex(parts[i]);
            } else if (!parseDouble(parts[i], line.valueSpan, m_diag, value)) {
                continue;
            }
            patch.params.push_back(project::ParamValue{
                .name = std::string(name), .value = value, .hasCurve = false, .curve = {}});
        }
        m_diag.add(code::kV1TupleNamed, line.keySpan,
                   std::string(line.key) + "= became " + std::to_string(mapping->count) +
                       " named parameters");
        line.claimed = true;
    }

    // v1 ran a release from the patch's sustain level, never from where the envelope
    // was (AudioEngine.cpp: releaseTable is a ramp from sustainLevel to 0), so with zero
    // sustain the release is silence and a note-off is a cut - the percussive patches
    // were tuned by their note lengths against that. v2 releases from the current level,
    // so the same release time would add a tail v1 never played (+5 dB at 39 Hz on
    // suffocation.adx's kick). Zero sustain therefore migrates with zero release.
    constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kReleases{
        {{"env.sustain", "env.release"}, {"filterEnv.sustain", "filterEnv.release"}}};
    for (const auto& [sustain, release] : kReleases) {
        const auto level = std::ranges::find(patch.params, sustain, &project::ParamValue::name);
        const auto time = std::ranges::find(patch.params, release, &project::ParamValue::name);
        if (level != patch.params.end() && time != patch.params.end() && level->value == 0.0 &&
            time->value != 0.0) {
            time->value = 0.0;
            m_diag.add(code::kV1ReleaseSilent, section.span,
                       "[PATCH " + patch.name + "] " + std::string(release) +
                           " became 0: v1 released from the sustain level, which is 0");
        }
    }

    m_patches.push_back(std::move(patch));
}

void V1Migration::collectTrack(Section& section) {
    section.claimed = true;
    RawTrack track;
    track.patchName = section.name;
    track.span = section.span;

    for (const std::size_t index : section.lineIndices) {
        Line& line = lines()[index];
        if (line.kind == Line::Kind::Blank || line.kind == Line::Kind::Comment) {
            continue;
        }

        if (line.kind == Line::Kind::KeyValue) {
            if (line.key == "MIX") {
                const std::vector<std::string_view> parts = splitOn(line.value, ',');
                if (parts.size() >= 2) {
                    (void)parseDouble(parts[0], line.valueSpan, m_diag, track.volume);
                    (void)parseDouble(parts[1], line.valueSpan, m_diag, track.pan);
                    line.claimed = true;
                }
                continue;
            }
            if (line.key == "SEND") {
                const std::vector<std::string_view> parts = splitOn(line.value, ',');
                if (parts.size() >= 2) {
                    double amount = 0.0;
                    if (parseDouble(parts[1], line.valueSpan, m_diag, amount)) {
                        if (parts[0] == "Delay") {
                            track.sendDelay = amount;
                            line.claimed = true;
                        } else if (parts[0] == "Reverb") {
                            track.sendReverb = amount;
                            line.claimed = true;
                        } else {
                            m_diag.add(code::kV1Unrecognised, line.valueSpan,
                                       "unknown v1 send bus '" + std::string(parts[0]) + "'");
                        }
                    }
                }
                continue;
            }
            if (line.key == "PATTERN") {
                track.mini.push_back(line.value);
                line.claimed = true;
                continue;
            }
            continue;
        }

        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.empty()) {
            continue;
        }

        if (tokens.front().text == "ARP" && tokens.size() == 5) {
            std::int64_t mode = 0;
            std::int64_t octaves = 1;
            if (parseInt64(tokens[1].text, tokens[1].span, m_diag, mode) &&
                parseDouble(tokens[2].text, tokens[2].span, m_diag, track.arpRateBeats) &&
                parseInt64(tokens[3].text, tokens[3].span, m_diag, octaves) &&
                parseDouble(tokens[4].text, tokens[4].span, m_diag, track.arpGate)) {
                track.hasArp = true;
                track.arpMode = static_cast<int>(mode);
                track.arpOctaves = static_cast<int>(octaves);
                line.claimed = true;
            }
            continue;
        }

        if (tokens.front().text == "EFFECT" && tokens.size() >= 3) {
            RawEffect effect;
            effect.type = tokens[1].text;
            effect.span = line.contentSpan();
            for (std::size_t i = 2; i < tokens.size(); ++i) {
                double value = 0.0;
                if (parseDouble(tokens[i].text, tokens[i].span, m_diag, value)) {
                    effect.args.push_back(value);
                }
            }
            track.effects.push_back(std::move(effect));
            line.claimed = true;
            continue;
        }

        if (tokens.front().text == "CLIP" && tokens.size() >= 3) {
            RawClip clip;
            clip.path = tokens[1].text;
            if (!parseDouble(tokens[2].text, tokens[2].span, m_diag, clip.startSeconds)) {
                continue;
            }
            if (tokens.size() >= 5) {
                (void)parseDouble(tokens[3].text, tokens[3].span, m_diag, clip.pitchSemitones);
                (void)parseDouble(tokens[4].text, tokens[4].span, m_diag, clip.stretch);
                clip.reverse = tokens.size() >= 6 && tokens[5].text == "R";
            }
            track.clips.push_back(std::move(clip));
            line.claimed = true;
            continue;
        }

        if (tokens.size() == 4) {
            RawNote note;
            std::uint32_t badOffset = 0;
            if (!noteNameToMidi(tokens[0].text, note.pitch, badOffset)) {
                m_diag.add(code::kBadNoteName, subSpan(tokens[0].span, badOffset, 1),
                           "'" + tokens[0].text + "' is not a note name");
                continue;
            }
            if (parseDouble(tokens[1].text, tokens[1].span, m_diag, note.startBeat) &&
                parseDouble(tokens[2].text, tokens[2].span, m_diag, note.lengthBeats) &&
                parseDouble(tokens[3].text, tokens[3].span, m_diag, note.velocity)) {
                track.notes.push_back(note);
                line.claimed = true;
            }
            continue;
        }

        m_diag.add(code::kV1Unrecognised, line.contentSpan(),
                   "not a v1 track line; preserved on save");
    }

    m_tracks.push_back(std::move(track));
}

void V1Migration::collectAutomation(Section& section, std::size_t index) {
    RawLane lane;
    lane.section = index;
    lane.span = section.span;
    const std::size_t space = section.name.find(' ');
    if (space == std::string::npos) {
        lane.track = section.name;
    } else {
        lane.track = section.name.substr(0, space);
        lane.param = std::string(trim(std::string_view(section.name).substr(space + 1)));
    }

    for (const std::size_t lineIndex : section.lineIndices) {
        const Line& line = lines()[lineIndex];
        if (line.kind == Line::Kind::Blank || line.kind == Line::Kind::Comment) {
            continue;
        }
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.size() != 3) {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                       "a v1 breakpoint is 'beat value curve'");
            continue;
        }
        RawPoint point;
        if (!parseDouble(tokens[0].text, tokens[0].span, m_diag, point.beat) ||
            !parseDouble(tokens[1].text, tokens[1].span, m_diag, point.value)) {
            continue;
        }
        point.curve = tokens[2].text;
        lane.points.push_back(std::move(point));
        lane.lineIndices.push_back(lineIndex);
    }

    m_lanes.push_back(std::move(lane));
}

void V1Migration::collect() {
    for (std::size_t i = 0; i < m_doc.sections().size(); ++i) {
        Section& section = m_doc.sections()[i];
        if (section.type == "GLOBAL") {
            collectGlobal(section);
        } else if (section.type == "PATCH") {
            collectPatch(section);
        } else if (section.type == "TRACK") {
            collectTrack(section);
        } else if (section.type == "AUTOMATION") {
            collectAutomation(section, i);
        } else if (!section.isPreamble()) {
            m_diag.add(code::kUnknownSection, section.span,
                       "'" + section.type + "' is not a v1 section; preserved on save");
        }
    }
}

// --- building ----------------------------------------------------------------

std::string V1Migration::uniqueName(std::string_view wanted, Span span) {
    const std::string base = project::sanitizeEntityName(wanted, "Track");
    std::string candidate = base;
    int suffix = 2;
    while (!m_usedNames.insert(candidate).second) {
        candidate = base + " " + std::to_string(suffix);
        ++suffix;
    }
    if (candidate != base) {
        m_diag.add(code::kV1NameSuffixed, span,
                   "'" + base + "' was already taken; this one is '" + candidate + "'");
    } else if (candidate != std::string(wanted)) {
        m_diag.add(code::kV1NameSuffixed, span,
                   "'" + std::string(wanted) +
                       "' contains characters a name may not have; using '" + candidate + "'");
    }
    return candidate;
}

void V1Migration::buildMaster() {
    project::Insert master;
    master.name = "Master";
    master.gain = static_cast<float>(m_global.masterVolume);
    auto command = std::make_unique<project::AddInsert>(std::move(master));
    const project::AddInsert* raw = command.get();
    execute(std::move(command));
    m_master = raw->created();

    // Order is normative, and it is v1's: AudioEngine.cpp's master chain ran delay,
    // reverb, the sidechain duck, the drive, then a 4:1 peak compressor at -3 dB, the
    // master volume and a hard clamp at +-1. Phase 2's shim put the drive first,
    // which changed the sound of every migrated project with MASTER_DRIVE; fixed in
    // Phase 4 (phase_4.md §11).
    // `mix` is the slot's own wet/dry field, not one of its named parameters. Putting
    // it in params as well would give the file two spellings of one value, and the
    // parser reads `mix=` back into the field - so a save/load cycle would move it
    // and `fmt` would stop being idempotent.
    // v1's master delay and reverb ran on the sum of the tracks, and its send buses fed
    // their *inputs*: the reverb send's tail was added after the reverb's own dry/wet,
    // never through it. A separate insert reproduces that topology - tracks into
    // "Master FX" (delay, reverb), that and the reverb bus into the master (duck,
    // drive, compressor, clamp). Phase 2 put everything on one insert, so a send bus's
    // tail went back through the master reverb's dry/wet and was reverberated twice.
    m_mixTarget = m_master;
    if (m_global.hasDelay || m_global.hasReverb) {
        project::Insert fx;
        fx.name = "Master FX";
        auto fxCommand = std::make_unique<project::AddInsert>(std::move(fx));
        const project::AddInsert* fxRaw = fxCommand.get();
        execute(std::move(fxCommand));
        m_mixTarget = fxRaw->created();
        execute(std::make_unique<project::AddRoute>(m_mixTarget, m_master));
    }

    const auto addSlotTo = [&](core::InsertId owner, std::string_view type,
                               std::vector<project::SlotParam> params) {
        project::Slot slot;
        slot.type = std::string(type);
        for (project::SlotParam& param : params) {
            if (param.name == "mix") {
                slot.mix = static_cast<float>(param.value);
                continue;
            }
            slot.params.push_back(std::move(param));
        }
        auto slotCommand = std::make_unique<project::AddSlot>(owner, std::move(slot));
        const project::AddSlot* slotRaw = slotCommand.get();
        execute(std::move(slotCommand));
        m_masterSlots[std::string(type)] = slotRaw->created();
        m_masterSlotOwners[std::string(type)] = owner;
    };
    const auto addSlot = [&](std::string_view type, std::vector<project::SlotParam> params) {
        addSlotTo(m_master, type, std::move(params));
    };

    const auto param = [](std::string_view name, double value) {
        return project::SlotParam{.name = std::string(name), .value = value};
    };

    if (m_global.hasDelay) {
        // v1 added the echoes on top of the untouched mix, at `mix` loud: the slot
        // fully wet, the effect passing its dry through and the echoes at `level`.
        addSlotTo(m_mixTarget, "Delay",
                  {param("timeMs", m_global.delay[0]), param("feedback", m_global.delay[1]),
                   param("mix", 1.0), param("dry", 1.0), param("level", m_global.delay[2])});
    }
    if (m_global.hasReverb) {
        addSlotTo(m_mixTarget, "Reverb",
                  {param("room", m_global.reverb[0]), param("damp", m_global.reverb[1]),
                   param("mix", m_global.reverb[2])});
    }
    if (m_global.hasSidechain) {
        addSlot("Ducker",
                {param("enabled", m_global.sidechain[0]), param("amount", m_global.sidechain[1]),
                 param("releaseMs", m_global.sidechain[2])});
        m_diag.add(code::kV1SidechainMigrated, Span{},
                   "SIDECHAIN= became a Ducker slot on the master insert, keyed from the first "
                   "track's insert as v1 always keyed it");
    }
    if (m_global.masterDrive != 0.0) {
        // v1's master drive was tanh(x * (1 + drive)): one more than the number in the
        // file, unlike a track's DISTORTION effect, which used the number as given.
        addSlot("Distortion", {param("drive", 1.0 + m_global.masterDrive), param("mix", 1.0)});
    }
    // v1's peak compressor and clamp, which every v1 render went through. The clamp
    // becomes a limiter at 0 dBFS: v1 clipped what got past its compressor; this
    // catches it without the distortion (P3-1).
    addSlot("Compressor", {param("threshold", -3.0), param("ratio", 4.0), param("knee", 0.0),
                           param("attack", 5.0), param("release", 50.0), param("smoothing", 1.0)});
    addSlot("Limiter", {param("ceiling", 0.0)});
    m_diag.add(code::kV1MasterFxMigrated, Span{},
               "v1's master FX are now slots on the master insert, in the order v1 processed "
               "them; its fixed peak compressor and output clamp are a Compressor and a Limiter");
}

core::InsertId V1Migration::ensureBus(std::string_view name) {
    const auto existing = m_buses.find(std::string(name));
    if (existing != m_buses.end()) {
        return existing->second;
    }
    // v1's sends fed only the *input* of its master delay or reverb, never the dry
    // mix. So the bus carries that effect fully wet, at the master effect's own level
    // - the send's signal reaches the master as echo or tail, not a second time dry.
    // Phase 2's empty buses passed every sent track through twice (phase_4.md §11).
    const bool delay = name == "Delay Bus";
    project::Insert bus;
    bus.name = std::string(name);
    double level = 0.0;
    if (delay && m_global.hasDelay) {
        level = m_global.delay[2];
    } else if (!delay && m_global.hasReverb) {
        level = m_global.reverb[2];
    }
    bus.gain = static_cast<float>(level);
    auto command = std::make_unique<project::AddInsert>(std::move(bus));
    const project::AddInsert* raw = command.get();
    execute(std::move(command));
    const core::InsertId id = raw->created();
    project::Slot effect;
    effect.mix = 1.0F;
    if (delay) {
        effect.type = "Delay";
        effect.params = {project::SlotParam{.name = "timeMs", .value = m_global.delay[0]},
                         project::SlotParam{.name = "feedback", .value = m_global.delay[1]}};
    } else {
        effect.type = "Reverb";
        effect.params = {project::SlotParam{.name = "room", .value = m_global.reverb[0]},
                         project::SlotParam{.name = "damp", .value = m_global.reverb[1]}};
    }
    execute(std::make_unique<project::AddSlot>(id, std::move(effect)));
    execute(std::make_unique<project::AddRoute>(id, delay ? m_mixTarget : m_master));
    m_buses.emplace(std::string(name), id);
    m_diag.add(code::kV1SendCreatedBus, Span{},
               "created the aux insert '" + std::string(name) +
                   "' and routed it to the master; v1's two hardcoded buses are ordinary "
                   "inserts now");
    return id;
}

/// The track's insert: its effects, its route to the master, and its sends.
void V1Migration::buildTrackMixer(const RawTrack& track, const std::string& name,
                                  Expanded& expanded) {

    project::Insert insert;
    insert.name = name;
    insert.gain = static_cast<float>(track.volume);
    insert.pan = static_cast<float>(track.pan);
    {
        auto command = std::make_unique<project::AddInsert>(std::move(insert));
        const project::AddInsert* raw = command.get();
        execute(std::move(command));
        expanded.insert = raw->created();
    }
    for (const RawEffect& effect : track.effects) {
        const auto mapping = std::ranges::find(kEffectMappings, effect.type, &EffectMapping::type);
        if (mapping == kEffectMappings.end()) {
            m_diag.add(code::kV1Unrecognised, effect.span,
                       "unknown v1 effect '" + effect.type + "'; preserved on save");
            continue;
        }
        project::Slot slot;
        slot.type = effect.type;
        for (std::size_t i = 0; i < mapping->count && i < effect.args.size(); ++i) {
            const std::string_view field = mapping->names.at(i);
            if (field == "mix") {
                slot.mix = static_cast<float>(effect.args[i]);
                continue;
            }
            slot.params.push_back(
                project::SlotParam{.name = std::string(field), .value = effect.args[i]});
        }
        auto command = std::make_unique<project::AddSlot>(expanded.insert, std::move(slot));
        const project::AddSlot* raw = command.get();
        execute(std::move(command));
        expanded.slotsByType.emplace(effect.type, raw->created());
    }
    execute(std::make_unique<project::AddRoute>(expanded.insert, m_mixTarget));

    if (track.sendDelay != 0.0) {
        execute(std::make_unique<project::AddSend>(expanded.insert, ensureBus("Delay Bus"),
                                                   static_cast<float>(track.sendDelay), false));
    }
    if (track.sendReverb != 0.0) {
        execute(std::make_unique<project::AddSend>(expanded.insert, ensureBus("Reverb Bus"),
                                                   static_cast<float>(track.sendReverb), false));
    }
}

/// The track's playlist lane: the pattern placed once at the start, and its clips.
void V1Migration::buildTrackPlaylist(const RawTrack& track, const std::string& name,
                                     const Expanded& expanded) {
    project::PlaylistTrack lane;
    lane.name = name;
    core::PlaylistTrackId laneId;
    {
        auto command = std::make_unique<project::AddPlaylistTrack>(std::move(lane));
        const project::AddPlaylistTrack* raw = command.get();
        execute(std::move(command));
        laneId = raw->created();
    }
    if (!track.notes.empty() || !track.mini.empty()) {
        project::PlaylistItem item;
        item.start = core::Ticks{0};
        item.content = project::PatternRef{.pattern = expanded.pattern};
        execute(std::make_unique<project::AddPlaylistItem>(laneId, item));
    }
    for (const RawClip& clip : track.clips) {
        auto sample = std::make_unique<project::AddSample>(clip.path);
        const project::AddSample* sampleRaw = sample.get();
        execute(std::move(sample));
        project::PlaylistItem item;
        item.start = m_project.tempo.ticksAtSeconds(clip.startSeconds);
        item.content =
            project::AudioClipRef{.sample = sampleRaw->created(),
                                  .stretch = static_cast<float>(clip.stretch),
                                  .pitchSemitones = static_cast<float>(clip.pitchSemitones),
                                  .reverse = clip.reverse};
        execute(std::make_unique<project::AddPlaylistItem>(laneId, item));
        m_diag.add(code::kV1ClipSecondsConverted, track.span,
                   "'" + clip.path + "' started at " + formatDouble(clip.startSeconds) +
                       "s in v1; converted to ticks through the tempo map");
    }
}

void V1Migration::buildTrack(const RawTrack& track) {
    const std::string name = uniqueName(track.patchName, track.span);
    Expanded expanded;
    buildTrackMixer(track, name, expanded);

    project::Channel channel;
    channel.name = name;
    channel.instrument.type = "additive";
    if (const auto patch = std::ranges::find(m_patches, track.patchName, &RawPatch::name);
        patch != m_patches.end()) {
        channel.instrument.params = patch->params;
    }
    if (track.hasArp) {
        // v1's integer modes: 0 off, 1 up, 2 down, 3 updown, 4 random.
        constexpr std::array<project::ArpMode, 5> kModes{
            project::ArpMode::Off, project::ArpMode::Up, project::ArpMode::Down,
            project::ArpMode::UpDown, project::ArpMode::Random};
        const auto index = static_cast<std::size_t>(std::clamp(track.arpMode, 0, 4));
        channel.arp.mode = kModes.at(index);
        // v1's rate was in beats; a beat is a quarter note, so 0.25 beats is 1/16.
        channel.arp.rate = core::Rational::make(std::llround(track.arpRateBeats * 16.0), 64);
        channel.arp.octaves = static_cast<std::uint16_t>(std::clamp(track.arpOctaves, 1, 8));
        channel.arp.gate = static_cast<float>(track.arpGate);
    }
    {
        auto command = std::make_unique<project::AddChannel>(std::move(channel));
        const project::AddChannel* raw = command.get();
        execute(std::move(command));
        expanded.channel = raw->created();
    }
    execute(std::make_unique<project::SetChannelOutput>(expanded.channel, expanded.insert));
    if (!m_firstTrackInsert.valid()) {
        m_firstTrackInsert = expanded.insert;
    }

    // One pattern holding the whole track, placed once at the start. v1 had no
    // pattern concept at all - its notes *were* the arrangement - so a faithful
    // migration cannot invent a reusable decomposition, only the structure that
    // makes one possible later.
    core::Ticks longest{0};
    for (const RawNote& note : track.notes) {
        longest = std::max(longest, ticksOfBeats(note.startBeat + note.lengthBeats));
    }
    const std::int64_t perBar =
        static_cast<std::int64_t>(m_project.tempo.meterAt(core::Ticks{0}).numerator) *
        core::ticksPerBeat(m_project.tempo.meterAt(core::Ticks{0}).denominator);
    project::Pattern pattern;
    pattern.name = name;
    pattern.length = longest.value > 0 && perBar > 0
                         ? core::Ticks{((longest.value + perBar - 1) / perBar) * perBar}
                         : project::kDefaultPatternLength;
    {
        auto command = std::make_unique<project::AddPattern>(std::move(pattern));
        const project::AddPattern* raw = command.get();
        execute(std::move(command));
        expanded.pattern = raw->created();
    }

    if (!track.notes.empty()) {
        std::vector<project::Note> notes;
        notes.reserve(track.notes.size());
        for (const RawNote& raw : track.notes) {
            project::Note note;
            note.pitch = raw.pitch;
            note.start = ticksOfBeats(raw.startBeat);
            note.length = ticksOfBeats(raw.lengthBeats);
            note.velocity = static_cast<std::uint8_t>(
                std::clamp(std::llround(raw.velocity * 127.0), 0LL, 127LL));
            notes.push_back(note);
        }
        m_velocityRescaled = true;
        execute(std::make_unique<project::AddNotes>(expanded.pattern, expanded.channel,
                                                    std::move(notes)));
    }
    for (const std::string& mini : track.mini) {
        execute(std::make_unique<project::SetMiniNotation>(expanded.pattern, expanded.channel,
                                                           std::vector<std::string>{mini}));
    }

    buildTrackPlaylist(track, name, expanded);

    m_diag.add(code::kV1TrackExpanded, track.span,
               "[TRACK " + track.patchName +
                   "] became the channel, pattern, playlist track and "
                   "insert all named '" +
                   name + "'");
    m_expanded.emplace(track.patchName, std::move(expanded));
}

std::string V1Migration::v2PathFor(const RawLane& lane) {
    const auto quote = [](std::string_view name) {
        return project::ParamRegistry::quoteSegment(name);
    };

    if (lane.track == "MASTER") {
        const auto slot = [&](std::string_view type, std::string_view leaf) -> std::string {
            const auto match = m_masterSlots.find(std::string(type));
            if (match == m_masterSlots.end()) {
                return {};
            }
            const auto owner = m_masterSlotOwners.find(std::string(type));
            return "insert." + std::to_string(owner->second.value) + ".slot." +
                   std::to_string(match->second.value) + "." + std::string(leaf);
        };
        // v1 spelled these `master.<field>`: the section header is
        // `[AUTOMATION MASTER master.reverbMix]`, so the track is MASTER *and* the
        // parameter keeps the prefix.
        constexpr std::string_view kPrefix = "master.";
        if (!lane.param.starts_with(kPrefix)) {
            return {};
        }
        const std::string_view field = std::string_view(lane.param).substr(kPrefix.size());
        if (field == "sidechainAmount") {
            return slot("Ducker", "amount");
        }
        if (field == "reverbMix") {
            return slot("Reverb", "mix");
        }
        if (field == "delayMix") {
            return slot("Delay", "level");
        }
        if (field == "masterDrive") {
            return slot("Distortion", "drive");
        }
        if (field == "masterVolume") {
            return "insert." + std::to_string(m_master.value) + ".gain";
        }
        return {};
    }

    const auto expanded = m_expanded.find(lane.track);
    if (expanded == m_expanded.end()) {
        return {};
    }
    const project::Channel* channel = m_project.find(expanded->second.channel);
    if (channel == nullptr) {
        return {};
    }

    // v1's mix lanes write the track's live volume and pan, which replace MIX= and are
    // applied after the track's effects (AudioEngine.cpp, applyTrackAutomationTarget).
    // MIX= became the insert's gain and pan, so that is what a lane drives - not the
    // channel's, which would multiply MIX= by the lane and sit before the effects.
    const std::string insert = "insert." + std::to_string(expanded->second.insert.value);
    if (lane.param == "mix.volume") {
        return insert + ".gain";
    }
    if (lane.param == "mix.pan") {
        return insert + ".pan";
    }
    constexpr std::string_view kPatchPrefix = "patch.";
    if (lane.param.starts_with(kPatchPrefix)) {
        const std::string_view field = std::string_view(lane.param).substr(kPatchPrefix.size());
        const auto alias = std::ranges::find(kPatchFieldAliases, field,
                                             &std::pair<std::string_view, std::string_view>::first);
        if (alias == kPatchFieldAliases.end()) {
            return {};
        }
        return "channel." + quote(channel->name) + "." + std::string(alias->second);
    }
    constexpr std::string_view kEffectPrefix = "effect.";
    if (lane.param.starts_with(kEffectPrefix)) {
        const std::string_view rest = std::string_view(lane.param).substr(kEffectPrefix.size());
        const std::size_t dot = rest.find('.');
        if (dot == std::string_view::npos) {
            return {};
        }
        const auto slot = expanded->second.slotsByType.find(std::string(rest.substr(0, dot)));
        if (slot == expanded->second.slotsByType.end()) {
            return {};
        }
        return "insert." + std::to_string(expanded->second.insert.value) + ".slot." +
               std::to_string(slot->second.value) + "." + std::string(rest.substr(dot + 1));
    }
    return {};
}

void V1Migration::buildAutomation() {
    if (m_lanes.empty()) {
        return;
    }
    project::PlaylistTrack lane;
    lane.name = uniqueName("Automation", Span{});
    core::PlaylistTrackId laneId;
    {
        auto command = std::make_unique<project::AddPlaylistTrack>(std::move(lane));
        const project::AddPlaylistTrack* raw = command.get();
        execute(std::move(command));
        laneId = raw->created();
    }

    bool approximated = false;
    for (const RawLane& raw : m_lanes) {
        const std::string path = v2PathFor(raw);
        project::ParamResolution resolved;
        if (!path.empty()) {
            resolved = project::ParamRegistry::resolve(path, m_project);
        }
        if (path.empty() || !resolved.ok()) {
            // Left unclaimed on purpose: an unresolvable lane is preserved verbatim
            // as residue rather than silently dropped, which is what v1 would have
            // done with an unknown target.
            m_diag.add(code::kV1UnresolvedAutomation, raw.span,
                       "[AUTOMATION " + raw.track + " " + raw.param +
                           "] has no v2 equivalent; the lane is preserved as residue");
            continue;
        }

        project::AutomationClip clip;
        clip.targetPath = path;
        clip.target = resolved.ref;
        for (const RawPoint& point : raw.points) {
            project::Breakpoint breakpoint;
            breakpoint.at = ticksOfBeats(point.beat);
            breakpoint.value = static_cast<float>(point.value);
            if (point.curve == "exp") {
                breakpoint.curve.kind = core::CurveKind::Exponential;
                approximated = true;
            } else if (point.curve == "step") {
                breakpoint.curve.kind = core::CurveKind::Step;
            } else if (point.curve == "smooth") {
                breakpoint.curve.kind = core::CurveKind::Smooth;
            } else {
                breakpoint.curve.kind = core::CurveKind::Linear;
            }
            clip.points.push_back(breakpoint);
        }

        auto command =
            std::make_unique<project::AddAutomationClip>(core::PatternId{}, std::move(clip));
        const project::AddAutomationClip* rawCommand = command.get();
        execute(std::move(command));

        project::PlaylistItem item;
        item.start = core::Ticks{0};
        item.content = project::AutomationRef{.clip = rawCommand->created()};
        execute(std::make_unique<project::AddPlaylistItem>(laneId, item));

        for (const std::size_t index : raw.lineIndices) {
            lines()[index].claimed = true;
        }
        m_doc.sections()[raw.section].claimed = true;
    }

    if (approximated) {
        m_diag.add(code::kV1CurveApproximated, Span{},
                   "v1's 'exp' was defined on the ratio between the two values, and v2's is a "
                   "normalised shape; the curves are close but not the same function");
    }
}

void V1Migration::build() {
    project::ProjectMeta meta;
    meta.tuning = m_global.tuning;
    // The file said it was v1, and saving it must not quietly claim otherwise. The
    // upgrade to 2 happens only through `adx fmt --upgrade`.
    meta.version = 1;
    if (m_global.hasLoop) {
        meta.loopEnabled = true;
        meta.loopStart = ticksOfBeats(m_global.loopStart);
        meta.loopEnd = ticksOfBeats(m_global.loopEnd);
    }
    execute(std::make_unique<project::SetMeta>(std::move(meta)));
    execute(std::make_unique<project::SetTempoEvent>(core::Ticks{0}, m_global.bpm, false));

    buildMaster();
    for (const RawTrack& track : m_tracks) {
        buildTrack(track);
    }
    const auto ducker = m_masterSlots.find("Ducker");
    if (ducker != m_masterSlots.end() && m_firstTrackInsert.valid()) {
        execute(std::make_unique<project::SetSlotSidechain>(ducker->second, m_firstTrackInsert));
    }
    buildAutomation();

    for (const auto& [beat, name] : m_global.markers) {
        execute(std::make_unique<project::AddMarker>(ticksOfBeats(beat), name));
    }

    if (m_velocityRescaled) {
        m_diag.add(code::kV1VelocityRescaled, Span{},
                   "velocities were rescaled from v1's 0..1 to 0..127");
    }
}

void V1Migration::run() {
    if (m_options.groupAsOneStep) {
        m_stack.beginGroup("Load v1 project");
    }
    collect();
    build();
    m_project.resources.baseDirectory = m_options.baseDirectory;
    m_project.residue = m_doc.residue();
    if (m_options.groupAsOneStep) {
        m_stack.endGroup();
    }
}

} // namespace

void migrateV1(Document& document, Project& project, project::CommandStack& stack,
               DiagnosticList& diagnostics, const ParseOptions& options) {
    V1Migration migration(document, project, stack, diagnostics, options);
    migration.run();
}

} // namespace adx::format

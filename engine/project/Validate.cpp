#include "engine/project/Validate.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"

namespace adx::project {
namespace {

using format::code::kBadNameCharacter;
using format::code::kDuplicateId;
using format::code::kDuplicateName;
using format::code::kDuplicateNoteClip;
using format::code::kMissingRequiredKey;
using format::code::kMissingSampleFile;
using format::code::kNegativeDuration;
using format::code::kNoteOutsidePattern;
using format::code::kRoutingCycle;
using format::code::kUnknownChannelRef;
using format::code::kUnknownInsertRef;
using format::code::kUnknownPatternRef;
using format::code::kUnresolvedParamPath;
using format::code::kZeroId;

/// Characters a name may not contain.
///
/// `.` would split a parameter path in the wrong place, `"` would end a quoted
/// segment, and `[` / `]` would end a section header. Everything else - including
/// spaces - is allowed, because `[CHANNEL Hardstyle Kick]` has to keep working.
constexpr std::string_view kForbiddenNameChars = ".\"[]";

/// Where an entity was written, when the project came from text (P2-2); otherwise no
/// position at all.
class Where {
public:
    explicit Where(const SourceMap* sources) noexcept : m_sources(sources) {}

    [[nodiscard]] format::Span operator()(SourceKind kind, std::uint32_t id) const noexcept {
        return m_sources != nullptr ? m_sources->find(kind, id) : kNoSpan;
    }

private:
    const SourceMap* m_sources;
};

void checkUniqueNames(const Project& project, format::DiagnosticList& diagnostics,
                      const Where& where) {
    std::unordered_set<std::string> seen;
    for (const Channel& channel : project.channels) {
        if (!isValidEntityName(channel.name)) {
            diagnostics.add(kBadNameCharacter, where(SourceKind::Channel, channel.id.value),
                            "channel name '" + channel.name + "' is empty or contains one of " +
                                std::string(kForbiddenNameChars));
        }
        if (!seen.insert("c/" + channel.name).second) {
            diagnostics.add(kDuplicateName, where(SourceKind::Channel, channel.id.value),
                            "two channels are named '" + channel.name +
                                "'; automation paths address channels by name");
        }
    }
    for (const Pattern& pattern : project.patterns) {
        if (!isValidEntityName(pattern.name)) {
            diagnostics.add(kBadNameCharacter, where(SourceKind::Pattern, pattern.id.value),
                            "pattern name '" + pattern.name + "' is empty or contains one of " +
                                std::string(kForbiddenNameChars));
        }
        if (!seen.insert("p/" + pattern.name).second) {
            diagnostics.add(kDuplicateName, where(SourceKind::Pattern, pattern.id.value),
                            "two patterns are named '" + pattern.name +
                                "'; the playlist addresses patterns by name");
        }
    }
}

/// Every id in the project must be non-zero and unique within its kind.
void checkIds(const Project& project, format::DiagnosticList& diagnostics, const Where& where) {
    std::unordered_set<std::uint64_t> seen;
    // The kinds below, in order, and where each was written (samples have no line).
    static constexpr std::array<int, 12> kSource{
        static_cast<int>(SourceKind::Channel), static_cast<int>(SourceKind::Pattern),
        static_cast<int>(SourceKind::Note),    static_cast<int>(SourceKind::AutomationClip),
        static_cast<int>(SourceKind::Insert),  static_cast<int>(SourceKind::Slot),
        static_cast<int>(SourceKind::Send),    static_cast<int>(SourceKind::Route),
        static_cast<int>(SourceKind::Track),   static_cast<int>(SourceKind::Item),
        static_cast<int>(SourceKind::Marker),  -1,
    };
    const auto spanOf = [&](std::uint32_t kind, std::uint32_t id) {
        const int source = kind < kSource.size() ? kSource.at(kind) : -1;
        return source < 0 ? kNoSpan : where(static_cast<SourceKind>(source), id);
    };
    const auto check = [&](std::uint32_t kind, std::uint32_t id, std::string_view what) {
        if (id == 0) {
            diagnostics.add(kZeroId, kNoSpan, std::string(what) + " has the null id");
            return;
        }
        const std::uint64_t key = (static_cast<std::uint64_t>(kind) << 32U) | id;
        if (!seen.insert(key).second) {
            diagnostics.add(kDuplicateId, spanOf(kind, id),
                            "two entities share " + std::string(what) + " id " +
                                std::to_string(id));
        }
    };

    for (const Channel& channel : project.channels) {
        check(0, channel.id.value, "channel");
    }
    for (const Pattern& pattern : project.patterns) {
        check(1, pattern.id.value, "pattern");
        for (const NoteClip& clip : pattern.noteClips) {
            for (const Note& note : clip.notes) {
                check(2, note.id.value, "note");
            }
        }
        for (const AutomationClip& clip : pattern.autoClips) {
            check(3, clip.id.value, "automation clip");
        }
    }
    for (const Insert& insert : project.mixer.inserts) {
        check(4, insert.id.value, "insert");
        for (const Slot& slot : insert.slots) {
            check(5, slot.id.value, "slot");
        }
        for (const Send& send : insert.sends) {
            check(6, send.id.value, "send");
        }
    }
    for (const Route& route : project.mixer.routes) {
        check(7, route.id.value, "route");
    }
    for (const PlaylistTrack& track : project.playlist.tracks) {
        check(8, track.id.value, "playlist track");
        for (const PlaylistItem& item : track.items) {
            check(9, item.id.value, "playlist item");
        }
    }
    for (const Marker& marker : project.markers) {
        check(10, marker.id.value, "marker");
    }
    for (const SampleRef& sample : project.resources.samples) {
        check(11, sample.id.value, "sample");
    }
    for (const AutomationClip& clip : project.playlist.autoClips) {
        check(3, clip.id.value, "automation clip");
    }
}

void checkMixer(const Project& project, format::DiagnosticList& diagnostics, const Where& where) {
    if (!project.mixer.master.valid() || project.mixer.find(project.mixer.master) == nullptr) {
        diagnostics.add(kMissingRequiredKey, kNoSpan,
                        "the project has no master insert; every signal path needs somewhere to "
                        "end");
    }
    for (const Insert& insert : project.mixer.inserts) {
        for (const Slot& slot : insert.slots) {
            if (slot.sidechain.valid() && project.mixer.find(slot.sidechain) == nullptr) {
                diagnostics.add(kUnknownInsertRef, where(SourceKind::Slot, slot.id.value),
                                "slot " + std::to_string(slot.id.value) + " is keyed from insert " +
                                    std::to_string(slot.sidechain.value) +
                                    ", which does not exist");
            }
        }
        for (const Send& send : insert.sends) {
            if (project.mixer.find(send.target) == nullptr) {
                diagnostics.add(kUnknownInsertRef, where(SourceKind::Send, send.id.value),
                                "insert " + std::to_string(insert.id.value) + " sends to insert " +
                                    std::to_string(send.target.value) + ", which does not exist");
            }
        }
    }
    for (const Route& route : project.mixer.routes) {
        if (project.mixer.find(route.from) == nullptr) {
            diagnostics.add(kUnknownInsertRef, where(SourceKind::Route, route.id.value),
                            "route from insert " + std::to_string(route.from.value) +
                                ", which does not exist");
        }
        if (project.mixer.find(route.to) == nullptr) {
            diagnostics.add(kUnknownInsertRef, where(SourceKind::Route, route.id.value),
                            "route to insert " + std::to_string(route.to.value) +
                                ", which does not exist");
        }
    }
}

/// Depth-first cycle detection over the routing DAG, reporting the cycle itself.
///
/// "There is a cycle" is not an actionable diagnostic on a forty-strip mixer; the
/// path is. Sends are edges too - a send from A to B and a route from B to A is a
/// feedback loop just as surely as two routes are.
void checkRoutingCycles(const Project& project, format::DiagnosticList& diagnostics,
                        const Where& where) {
    // Each edge remembers the line that made it, so the cycle is reported at the edge
    // that closes it (P2-2).
    struct Edge {
        std::uint32_t to{0};
        SourceKind kind{SourceKind::Route};
        std::uint32_t id{0};
    };
    std::unordered_map<std::uint32_t, std::vector<Edge>> edges;
    for (const Route& route : project.mixer.routes) {
        edges[route.from.value].push_back(
            Edge{.to = route.to.value, .kind = SourceKind::Route, .id = route.id.value});
    }
    for (const Insert& insert : project.mixer.inserts) {
        for (const Slot& slot : insert.slots) {
            // A sidechain is an edge from its key into the slot's insert: a key that
            // is fed by the insert it keys is a cycle like any other.
            if (slot.sidechain.valid()) {
                edges[slot.sidechain.value].push_back(
                    Edge{.to = insert.id.value, .kind = SourceKind::Slot, .id = slot.id.value});
            }
        }
        for (const Send& send : insert.sends) {
            edges[insert.id.value].push_back(
                Edge{.to = send.target.value, .kind = SourceKind::Send, .id = send.id.value});
        }
    }

    enum class Mark : std::uint8_t { kWhite, kGrey, kBlack };
    std::unordered_map<std::uint32_t, Mark> marks;
    std::vector<std::uint32_t> stack;
    bool reported = false;

    // Explicit stack rather than recursion: misc-no-recursion is on for the whole
    // project, and a mixer graph is exactly the shape where an adversarial file
    // could make a recursive walk blow the stack.
    const auto describe = [&project](std::uint32_t id) {
        const Insert* insert = project.mixer.find(core::InsertId{id});
        return insert != nullptr && !insert->name.empty() ? insert->name
                                                          : "insert " + std::to_string(id);
    };

    for (const Insert& start : project.mixer.inserts) {
        if (marks[start.id.value] != Mark::kWhite) {
            continue;
        }
        struct Frame {
            std::uint32_t node{0};
            std::size_t next{0};
        };
        std::vector<Frame> frames{Frame{.node = start.id.value, .next = 0}};
        marks[start.id.value] = Mark::kGrey;
        stack.push_back(start.id.value);

        while (!frames.empty()) {
            Frame& top = frames.back();
            const auto& outgoing = edges[top.node];
            if (top.next >= outgoing.size()) {
                marks[top.node] = Mark::kBlack;
                stack.pop_back();
                frames.pop_back();
                continue;
            }
            const Edge& edge = outgoing[top.next];
            const std::uint32_t next = edge.to;
            ++top.next;
            if (marks[next] == Mark::kGrey) {
                if (!reported) {
                    std::string path;
                    bool inCycle = false;
                    for (const std::uint32_t node : stack) {
                        inCycle = inCycle || node == next;
                        if (!inCycle) {
                            continue;
                        }
                        if (!path.empty()) {
                            path += " -> ";
                        }
                        path += describe(node);
                    }
                    path += " -> " + describe(next);
                    diagnostics.add(kRoutingCycle, where(edge.kind, edge.id),
                                    "routing cycle: " + path);
                    reported = true;
                }
                continue;
            }
            if (marks[next] == Mark::kBlack) {
                continue;
            }
            marks[next] = Mark::kGrey;
            stack.push_back(next);
            frames.push_back(Frame{.node = next, .next = 0});
        }
    }
}

void checkPatterns(const Project& project, format::DiagnosticList& diagnostics,
                   const Where& where) {
    for (const Pattern& pattern : project.patterns) {
        if (pattern.length.value < 0) {
            diagnostics.add(kNegativeDuration, where(SourceKind::Pattern, pattern.id.value),
                            "pattern '" + pattern.name + "' has a negative length");
        }
        std::unordered_set<std::uint32_t> channelsSeen;
        for (const NoteClip& clip : pattern.noteClips) {
            if (project.find(clip.channel) == nullptr) {
                diagnostics.add(kUnknownChannelRef, where(SourceKind::Pattern, pattern.id.value),
                                "pattern '" + pattern.name +
                                    "' has notes for a channel that does not exist");
            }
            if (!channelsSeen.insert(clip.channel.value).second) {
                diagnostics.add(kDuplicateNoteClip, where(SourceKind::Pattern, pattern.id.value),
                                "pattern '" + pattern.name +
                                    "' has two note clips for the same channel");
            }
            for (const Note& note : clip.notes) {
                if (note.length.value < 0) {
                    diagnostics.add(kNegativeDuration, where(SourceKind::Note, note.id.value),
                                    "a note in pattern '" + pattern.name +
                                        "' has a negative length");
                }
                if (note.start.value < 0 || note.start >= pattern.length) {
                    diagnostics.add(kNoteOutsidePattern, where(SourceKind::Note, note.id.value),
                                    "a note in pattern '" + pattern.name +
                                        "' starts outside the pattern's length");
                }
            }
        }
        for (const MiniNotationSource& source : pattern.mini) {
            if (project.find(source.channel) == nullptr) {
                diagnostics.add(kUnknownChannelRef, where(SourceKind::Pattern, pattern.id.value),
                                "pattern '" + pattern.name +
                                    "' has mini-notation for a channel that does not exist");
            }
        }
    }
}

void checkPlaylist(const Project& project, format::DiagnosticList& diagnostics,
                   const Where& where) {
    for (const PlaylistTrack& track : project.playlist.tracks) {
        for (const PlaylistItem& item : track.items) {
            if (item.length.value < 0 || item.sourceOffset.value < 0) {
                diagnostics.add(kNegativeDuration, where(SourceKind::Item, item.id.value),
                                "playlist item on track '" + track.name +
                                    "' has a negative length or offset");
            }
            if (const auto* ref = std::get_if<PatternRef>(&item.content)) {
                if (project.find(ref->pattern) == nullptr) {
                    diagnostics.add(kUnknownPatternRef, where(SourceKind::Item, item.id.value),
                                    "playlist item on track '" + track.name +
                                        "' places a pattern that does not exist");
                }
            } else if (const auto* audio = std::get_if<AudioClipRef>(&item.content)) {
                if (project.resources.find(audio->sample) == nullptr) {
                    diagnostics.add(kMissingSampleFile, where(SourceKind::Item, item.id.value),
                                    "playlist item on track '" + track.name +
                                        "' references a sample that is not in the pool");
                }
            } else if (const auto* lane = std::get_if<AutomationRef>(&item.content)) {
                if (project.findAutomationClip(lane->clip) == nullptr) {
                    diagnostics.add(kUnresolvedParamPath, where(SourceKind::Item, item.id.value),
                                    "playlist item on track '" + track.name +
                                        "' places an automation lane that does not exist");
                }
            }
        }
    }
}

void checkAutomation(const Project& project, format::DiagnosticList& diagnostics,
                     const Where& where) {
    const auto checkLane = [&](const AutomationClip& clip) {
        if (!clip.target.valid()) {
            diagnostics.add(kUnresolvedParamPath, where(SourceKind::AutomationClip, clip.id.value),
                            "automation lane '" + clip.targetPath + "' does not resolve");
            return;
        }
        if (ParamRegistry::pathOf(clip.target, project).empty()) {
            diagnostics.add(kUnresolvedParamPath, where(SourceKind::AutomationClip, clip.id.value),
                            "automation lane '" + clip.targetPath +
                                "' points at an entity that has been deleted");
        }
    };
    for (const Pattern& pattern : project.patterns) {
        for (const AutomationClip& clip : pattern.autoClips) {
            checkLane(clip);
        }
    }
    for (const AutomationClip& clip : project.playlist.autoClips) {
        checkLane(clip);
    }
}

void checkChannels(const Project& project, format::DiagnosticList& diagnostics,
                   const Where& where) {
    for (const Channel& channel : project.channels) {
        if (project.mixer.find(channel.output) == nullptr) {
            diagnostics.add(kUnknownInsertRef, where(SourceKind::Channel, channel.id.value),
                            "channel '" + channel.name + "' feeds insert " +
                                std::to_string(channel.output.value) + ", which does not exist");
        }
        for (const SampleZone& zone : channel.instrument.zones) {
            if (project.resources.find(zone.sample) == nullptr) {
                diagnostics.add(kMissingSampleFile, where(SourceKind::Channel, channel.id.value),
                                "a zone on channel '" + channel.name +
                                    "' references a sample that is not in the pool");
            }
        }
    }
}

void checkSamples(const Project& project, format::DiagnosticList& diagnostics) {
    const std::filesystem::path base{project.resources.baseDirectory};
    for (const SampleRef& sample : project.resources.samples) {
        std::error_code error;
        const std::filesystem::path full = project.resources.baseDirectory.empty()
                                               ? std::filesystem::path{sample.path}
                                               : base / sample.path;
        if (!std::filesystem::exists(full, error)) {
            diagnostics.add(kMissingSampleFile, kNoSpan, "sample not found: " + sample.path);
        }
    }
}

} // namespace

bool isValidEntityName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        if (kForbiddenNameChars.find(c) != std::string_view::npos) {
            return false;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
    }
    // A name that is entirely whitespace would round-trip as an empty section
    // argument, which reads back as a different entity.
    return name.find_first_not_of(" \t") != std::string_view::npos;
}

std::string sanitizeEntityName(std::string_view name, std::string_view fallback) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (kForbiddenNameChars.find(c) != std::string_view::npos ||
            static_cast<unsigned char>(c) < 0x20) {
            out.push_back('_');
            continue;
        }
        out.push_back(c);
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out.empty() ? std::string(fallback) : out;
}

bool validate(const Project& project, format::DiagnosticList& diagnostics,
              const ValidationOptions& options) {
    const std::size_t before = diagnostics.size();
    const Where where(options.sources);

    checkUniqueNames(project, diagnostics, where);
    checkIds(project, diagnostics, where);
    checkMixer(project, diagnostics, where);
    checkRoutingCycles(project, diagnostics, where);
    checkChannels(project, diagnostics, where);
    checkPatterns(project, diagnostics, where);
    checkPlaylist(project, diagnostics, where);
    checkAutomation(project, diagnostics, where);
    if (options.checkSampleFiles) {
        checkSamples(project, diagnostics);
    }

    const auto added = diagnostics.all().subspan(before);
    return std::ranges::none_of(added, [](const format::Diagnostic& item) {
        return item.severity == format::Severity::Error;
    });
}

} // namespace adx::project

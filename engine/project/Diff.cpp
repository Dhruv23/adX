#include "engine/project/Diff.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <variant>

#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"

namespace adx::project {
namespace {

/// Notes are compared by content rather than by id: two loads of the same file
/// assign the same ids, but a file edited by hand and reloaded need not, and "you
/// changed nothing" must not be reported as "you deleted every note and added them
/// back".
using NoteKey = std::tuple<std::int64_t, std::uint8_t, std::int64_t, std::uint8_t>;

[[nodiscard]] std::multiset<NoteKey> noteKeys(const NoteClip& clip) {
    std::multiset<NoteKey> keys;
    for (const Note& note : clip.notes) {
        keys.emplace(note.start.value, note.pitch, note.length.value, note.velocity);
    }
    return keys;
}

/// Counts how many elements of `lhs` are not matched in `rhs`.
[[nodiscard]] std::size_t unmatched(const std::multiset<NoteKey>& lhs,
                                    const std::multiset<NoteKey>& rhs) {
    std::vector<NoteKey> difference;
    std::ranges::set_difference(lhs, rhs, std::back_inserter(difference));
    return difference.size();
}

void push(std::vector<Change>& out, ChangeKind kind, std::string subject, std::string detail = {}) {
    out.push_back(Change{.kind = kind, .subject = std::move(subject), .detail = std::move(detail)});
}

void diffMeta(const Project& before, const Project& after, std::vector<Change>& out) {
    if (before.meta.title != after.meta.title) {
        push(out, ChangeKind::Changed, "title",
             "'" + before.meta.title + "' -> '" + after.meta.title + "'");
    }
    if (before.meta.tuning != after.meta.tuning) {
        push(out, ChangeKind::Changed, "tuning");
    }
    if (before.meta.loopEnabled != after.meta.loopEnabled ||
        before.meta.loopStart != after.meta.loopStart ||
        before.meta.loopEnd != after.meta.loopEnd) {
        push(out, ChangeKind::Changed, "loop");
    }
    if (before.tempo.tempoEvents().size() != after.tempo.tempoEvents().size() ||
        !std::ranges::equal(before.tempo.tempoEvents(), after.tempo.tempoEvents())) {
        push(out, ChangeKind::Changed, "tempo");
    }
    if (!std::ranges::equal(before.tempo.meterEvents(), after.tempo.meterEvents())) {
        push(out, ChangeKind::Changed, "meter");
    }
}

void diffChannels(const Project& before, const Project& after, std::vector<Change>& out) {
    for (const Channel& old : before.channels) {
        const Channel* now = after.findChannelByName(old.name);
        if (now == nullptr) {
            push(out, ChangeKind::Removed, "channel " + old.name);
            continue;
        }
        std::vector<std::string> fields;
        if (old.instrument.type != now->instrument.type) {
            fields.emplace_back("instrument");
        }
        if (old.volume != now->volume) {
            fields.emplace_back("volume");
        }
        if (old.pan != now->pan) {
            fields.emplace_back("pan");
        }
        if (old.maxPolyphony != now->maxPolyphony) {
            fields.emplace_back("polyphony");
        }
        if (old.muted != now->muted || old.soloed != now->soloed) {
            fields.emplace_back("mute/solo");
        }
        if (old.output != now->output) {
            fields.emplace_back("output");
        }
        if (!(old.arp == now->arp)) {
            fields.emplace_back("arp");
        }
        for (const ParamValue& param : old.instrument.params) {
            const ParamValue* match = now->instrument.find(param.name);
            if (match == nullptr || !(*match == param)) {
                fields.push_back(param.name);
            }
        }
        for (const ParamValue& param : now->instrument.params) {
            if (old.instrument.find(param.name) == nullptr) {
                fields.push_back(param.name);
            }
        }
        if (!fields.empty()) {
            std::string detail;
            for (const std::string& field : fields) {
                detail += detail.empty() ? field : ", " + field;
            }
            push(out, ChangeKind::Changed, "channel " + old.name, detail);
        }
    }
    for (const Channel& now : after.channels) {
        if (before.findChannelByName(now.name) == nullptr) {
            push(out, ChangeKind::Added, "channel " + now.name);
        }
    }
}

void diffPatterns(const Project& before, const Project& after, std::vector<Change>& out) {
    for (const Pattern& old : before.patterns) {
        const Pattern* now = after.findPatternByName(old.name);
        if (now == nullptr) {
            push(out, ChangeKind::Removed, "pattern " + old.name);
            continue;
        }
        std::vector<std::string> details;
        if (old.length != now->length) {
            details.emplace_back("length");
        }
        // Clips are matched by channel *name*, the same way the file addresses them.
        std::map<std::string, std::pair<std::multiset<NoteKey>, std::multiset<NoteKey>>> clips;
        for (const NoteClip& clip : old.noteClips) {
            if (const Channel* channel = before.find(clip.channel)) {
                clips[channel->name].first = noteKeys(clip);
            }
        }
        for (const NoteClip& clip : now->noteClips) {
            if (const Channel* channel = after.find(clip.channel)) {
                clips[channel->name].second = noteKeys(clip);
            }
        }
        for (const auto& [channel, sets] : clips) {
            const std::size_t removed = unmatched(sets.first, sets.second);
            const std::size_t added = unmatched(sets.second, sets.first);
            if (removed == 0 && added == 0) {
                continue;
            }
            std::string detail = channel + ":";
            if (added > 0) {
                detail += " +" + std::to_string(added) + " notes";
            }
            if (removed > 0) {
                detail += " -" + std::to_string(removed) + " notes";
            }
            details.push_back(detail);
        }
        if (old.autoClips.size() != now->autoClips.size() ||
            !std::ranges::equal(old.autoClips, now->autoClips, {}, &AutomationClip::points,
                                &AutomationClip::points)) {
            details.emplace_back("automation");
        }
        if (old.mini.size() != now->mini.size() ||
            !std::ranges::equal(old.mini, now->mini, {}, &MiniNotationSource::lines,
                                &MiniNotationSource::lines)) {
            details.emplace_back("mini-notation");
        }
        if (!details.empty()) {
            std::string detail;
            for (const std::string& item : details) {
                detail += detail.empty() ? item : "; " + item;
            }
            push(out, ChangeKind::Changed, "pattern " + old.name, detail);
        }
    }
    for (const Pattern& now : after.patterns) {
        if (before.findPatternByName(now.name) == nullptr) {
            push(out, ChangeKind::Added, "pattern " + now.name);
        }
    }
}

void diffPlaylist(const Project& before, const Project& after, std::vector<Change>& out) {
    for (const PlaylistTrack& old : before.playlist.tracks) {
        const PlaylistTrack* now = after.playlist.find(old.id);
        const std::string subject = "playlist track " + std::to_string(old.id.value);
        if (now == nullptr) {
            push(out, ChangeKind::Removed, subject);
            continue;
        }
        if (old.name != now->name || old.muted != now->muted ||
            old.items.size() != now->items.size()) {
            push(out, ChangeKind::Changed, subject,
                 std::to_string(old.items.size()) + " -> " + std::to_string(now->items.size()) +
                     " items");
            continue;
        }
        for (std::size_t i = 0; i < old.items.size(); ++i) {
            if (old.items[i].start != now->items[i].start ||
                old.items[i].length != now->items[i].length ||
                old.items[i].muted != now->items[i].muted) {
                push(out, ChangeKind::Changed, subject, "items moved or trimmed");
                break;
            }
        }
    }
    for (const PlaylistTrack& now : after.playlist.tracks) {
        if (before.playlist.find(now.id) == nullptr) {
            push(out, ChangeKind::Added, "playlist track " + std::to_string(now.id.value));
        }
    }
}

void diffMixer(const Project& before, const Project& after, std::vector<Change>& out) {
    for (const Insert& old : before.mixer.inserts) {
        const Insert* now = after.mixer.find(old.id);
        const std::string subject = "insert " + std::to_string(old.id.value) +
                                    (old.name.empty() ? "" : " (" + old.name + ")");
        if (now == nullptr) {
            push(out, ChangeKind::Removed, subject);
            continue;
        }
        std::vector<std::string> fields;
        if (old.gain != now->gain) {
            fields.emplace_back("gain");
        }
        if (old.pan != now->pan) {
            fields.emplace_back("pan");
        }
        if (old.muted != now->muted || old.soloed != now->soloed) {
            fields.emplace_back("mute/solo");
        }
        if (!(old.slots == now->slots)) {
            fields.emplace_back("effects");
        }
        if (!(old.sends == now->sends)) {
            fields.emplace_back("sends");
        }
        if (!fields.empty()) {
            std::string detail;
            for (const std::string& field : fields) {
                detail += detail.empty() ? field : ", " + field;
            }
            push(out, ChangeKind::Changed, subject, detail);
        }
    }
    for (const Insert& now : after.mixer.inserts) {
        if (before.mixer.find(now.id) == nullptr) {
            push(out, ChangeKind::Added, "insert " + std::to_string(now.id.value));
        }
    }
    const auto routeKeys = [](const Project& project) {
        std::multiset<std::pair<std::uint32_t, std::uint32_t>> keys;
        for (const Route& route : project.mixer.routes) {
            keys.emplace(route.from.value, route.to.value);
        }
        return keys;
    };
    if (routeKeys(before) != routeKeys(after)) {
        push(out, ChangeKind::Changed, "routing");
    }
}

void diffMarkers(const Project& before, const Project& after, std::vector<Change>& out) {
    const auto keys = [](const Project& project) {
        std::multiset<std::pair<std::int64_t, std::string>> set;
        for (const Marker& marker : project.markers) {
            set.emplace(marker.at.value, marker.name);
        }
        return set;
    };
    const auto old = keys(before);
    const auto now = keys(after);
    for (const auto& [at, name] : old) {
        if (!now.contains({at, name})) {
            push(out, ChangeKind::Removed, "marker " + name);
        }
    }
    for (const auto& [at, name] : now) {
        if (!old.contains({at, name})) {
            push(out, ChangeKind::Added, "marker " + name);
        }
    }
}

} // namespace

const char* toString(ChangeKind kind) noexcept {
    switch (kind) {
    case ChangeKind::Added:
        return "+";
    case ChangeKind::Removed:
        return "-";
    case ChangeKind::Changed:
        return "~";
    }
    return "~";
}

std::vector<Change> diff(const Project& before, const Project& after) {
    std::vector<Change> out;
    diffMeta(before, after, out);
    diffChannels(before, after, out);
    diffPatterns(before, after, out);
    diffPlaylist(before, after, out);
    diffMixer(before, after, out);
    diffMarkers(before, after, out);
    return out;
}

} // namespace adx::project

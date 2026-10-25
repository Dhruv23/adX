// adx-thread: main
#include "engine/format/adx/Patch.h"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "engine/project/Project.h"
#include "engine/project/commands/AutomationCommands.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatchCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"

namespace adx::format {

namespace {

using project::Channel;
using project::Insert;
using project::Note;
using project::NoteClip;
using project::Pattern;
using project::PlaylistItem;
using project::PlaylistTrack;
using project::Project;

// --- normalising: the orders that carry no meaning ---------------------------------

template<class T, class Key> void sortBy(std::vector<T>& items, Key key) {
    std::ranges::sort(items, {}, key);
}

/// A copy with every meaningless order put into one canonical order.
///
/// Only two kinds of order mean nothing: the order of whole sections (channels,
/// patterns, controllers - moving a section in the file is formatting), and the order
/// of what the writer sorts anyway (notes in a clip, markers, the sample pool, which it
/// derives). Every other list - inserts, slots, sends, routes, a track's items, a
/// pattern's NOTES, MINI and AUTOMATION blocks - is written in its order, so its order
/// is content: comparing it sorted let a patch leave a file that wrote differently
/// (patch_apply_equals_target found items, then routes; phase_7.md §10).
Project normalised(const Project& in) {
    Project p = in;
    sortBy(p.channels, [](const Channel& c) { return c.id.value; });
    sortBy(p.patterns, [](const Pattern& x) { return x.id.value; });
    for (Pattern& pattern : p.patterns) {
        for (NoteClip& clip : pattern.noteClips) {
            sortBy(clip.notes, [](const Note& n) { return n.id.value; });
        }
    }
    sortBy(p.resources.samples, [](const project::SampleRef& s) { return s.id.value; });
    sortBy(p.markers, [](const project::Marker& m) { return m.id.value; });
    sortBy(p.controllers, [](const project::Controller& c) { return c.id.value; });
    // Where residue sat in its file is not content: a line added above an unknown
    // section must not make the reload structural.
    for (ResidueBlock& block : p.residue.blocks) {
        block.sourceLine = 0;
        for (ResidueLine& line : block.lines) {
            line.sourceLine = 0;
        }
    }
    return p;
}

bool contentEqual(const Project& a, const Project& b) {
    return a.meta == b.meta && a.tempo == b.tempo && a.channels == b.channels &&
           a.patterns == b.patterns && a.playlist == b.playlist && a.mixer == b.mixer &&
           a.resources == b.resources && a.markers == b.markers && a.controllers == b.controllers &&
           a.residue == b.residue;
}

bool sameClip(const NoteClip& a, const NoteClip& b) {
    if (a.channel != b.channel || a.notes.size() != b.notes.size() || a.extras != b.extras) {
        return false;
    }
    std::vector<Note> x = a.notes;
    std::vector<Note> y = b.notes;
    sortBy(x, [](const Note& n) { return n.id.value; });
    sortBy(y, [](const Note& n) { return n.id.value; });
    return x == y;
}

// --- step 1: remapping the target into the current project's ids -------------------

class Remapper {
public:
    Remapper(const Project& current, const Project& target)
        : m_cur(current), m_target(target), m_goal(current) {}

    Project run() {
        mapIds();
        m_goal.meta = m_target.meta;
        m_goal.tempo = m_target.tempo;
        m_goal.residue = m_target.residue;
        m_goal.resources.samples.clear();
        for (const project::SampleRef& sample : m_target.resources.samples) {
            m_goal.resources.samples.push_back(
                project::SampleRef{.id = m_samples.at(sample.id.value), .path = sample.path});
        }
        m_goal.channels.clear();
        for (const Channel& channel : m_target.channels) {
            m_goal.channels.push_back(remapChannel(channel));
        }
        m_goal.mixer = m_target.mixer;
        for (Insert& insert : m_goal.mixer.inserts) {
            for (project::Slot& slot : insert.slots) {
                slot.impulse = sample(slot.impulse);
            }
        }
        for (project::Route& route : m_goal.mixer.routes) {
            route.id = m_routes.at(route.id.value);
        }
        m_goal.patterns.clear();
        for (const Pattern& pattern : m_target.patterns) {
            m_goal.patterns.push_back(remapPattern(pattern));
        }
        remapPlaylist();
        m_goal.markers.clear();
        for (const project::Marker& marker : m_target.markers) {
            project::Marker copy = marker;
            copy.id = m_markers.at(marker.id.value);
            m_goal.markers.push_back(copy);
        }
        m_goal.controllers.clear();
        for (const project::Controller& controller : m_target.controllers) {
            project::Controller copy = controller;
            copy.id = m_controllers.at(controller.id.value);
            copy.target = param(controller.target);
            copy.sourceChannel = channel(controller.sourceChannel);
            m_goal.controllers.push_back(copy);
        }
        return std::move(m_goal);
    }

private:
    const Project& m_cur;
    const Project& m_target;
    Project m_goal;

    std::map<std::uint32_t, core::ChannelId> m_channels;
    std::map<std::uint32_t, core::PatternId> m_patterns;
    std::map<std::uint32_t, core::SampleId> m_samples;
    std::map<std::uint32_t, core::MarkerId> m_markers;
    std::map<std::uint32_t, core::ControllerId> m_controllers;
    std::map<std::uint32_t, core::RouteId> m_routes;
    std::map<std::uint32_t, core::AutomationClipId> m_autoClips;

    // Ids the target names by number in the file need no mapping, but the counters
    // must know about them, so the next new entity of each kind still gets an id
    // nothing has had.
    void observeNumbered() {
        for (const Insert& insert : m_target.mixer.inserts) {
            static_cast<void>(m_goal.newInsertId(insert.id));
            for (const project::Slot& slot : insert.slots) {
                static_cast<void>(m_goal.newSlotId(slot.id));
            }
            for (const project::Send& send : insert.sends) {
                static_cast<void>(m_goal.newSendId(send.id));
            }
        }
        for (const PlaylistTrack& track : m_target.playlist.tracks) {
            static_cast<void>(m_goal.newPlaylistTrackId(track.id));
        }
        for (const project::Arrangement& arrangement : m_target.playlist.arrangements) {
            for (const PlaylistTrack& track : arrangement.tracks) {
                static_cast<void>(m_goal.newPlaylistTrackId(track.id));
            }
        }
    }

    void mapIds() {
        observeNumbered();
        for (const project::SampleRef& s : m_target.resources.samples) {
            const project::SampleRef* match = m_cur.resources.findByPath(s.path);
            m_samples[s.id.value] = match != nullptr ? match->id : m_goal.newSampleId();
        }
        for (const Channel& c : m_target.channels) {
            const Channel* match = m_cur.findChannelByName(c.name);
            m_channels[c.id.value] = match != nullptr ? match->id : m_goal.newChannelId();
        }
        for (const Pattern& p : m_target.patterns) {
            const Pattern* match = m_cur.findPatternByName(p.name);
            m_patterns[p.id.value] = match != nullptr ? match->id : m_goal.newPatternId();
        }
        for (const project::Controller& c : m_target.controllers) {
            const project::Controller* match = m_cur.findControllerByName(c.name);
            m_controllers[c.id.value] = match != nullptr ? match->id : m_goal.newControllerId();
        }
        mapMarkers();
        mapRoutes();
        // Playlist-level automation clips: by target path, in order.
        mapAutoClips(m_cur.playlist.autoClips, m_target.playlist.autoClips);
    }

    void mapMarkers() {
        std::vector<bool> used(m_cur.markers.size(), false);
        const auto claim = [&](auto&& matches) -> std::optional<core::MarkerId> {
            for (std::size_t i = 0; i < m_cur.markers.size(); ++i) {
                if (!used[i] && matches(m_cur.markers[i])) {
                    used[i] = true;
                    return m_cur.markers[i].id;
                }
            }
            return std::nullopt;
        };
        // Exact first, then by name (a moved marker), then by position (a renamed one).
        std::vector<std::optional<core::MarkerId>> found(m_target.markers.size());
        for (std::size_t i = 0; i < m_target.markers.size(); ++i) {
            const project::Marker& m = m_target.markers[i];
            found[i] =
                claim([&](const project::Marker& c) { return c.at == m.at && c.name == m.name; });
        }
        for (std::size_t i = 0; i < m_target.markers.size(); ++i) {
            const project::Marker& m = m_target.markers[i];
            if (!found[i]) {
                found[i] = claim([&](const project::Marker& c) { return c.name == m.name; });
            }
        }
        for (std::size_t i = 0; i < m_target.markers.size(); ++i) {
            const project::Marker& m = m_target.markers[i];
            if (!found[i]) {
                found[i] = claim([&](const project::Marker& c) { return c.at == m.at; });
            }
            m_markers[m.id.value] = found[i] ? *found[i] : m_goal.newMarkerId();
        }
    }

    void mapRoutes() {
        for (const project::Route& r : m_target.mixer.routes) {
            const auto match =
                std::ranges::find_if(m_cur.mixer.routes, [&](const project::Route& c) {
                    return c.from == r.from && c.to == r.to;
                });
            m_routes[r.id.value] =
                match != m_cur.mixer.routes.end() ? match->id : m_goal.newRouteId();
        }
    }

    void mapAutoClips(const std::vector<project::AutomationClip>& current,
                      const std::vector<project::AutomationClip>& target) {
        std::vector<bool> used(current.size(), false);
        for (const project::AutomationClip& clip : target) {
            core::AutomationClipId id;
            for (std::size_t i = 0; i < current.size(); ++i) {
                if (!used[i] && current[i].targetPath == clip.targetPath) {
                    used[i] = true;
                    id = current[i].id;
                    break;
                }
            }
            m_autoClips[clip.id.value] = id.valid() ? id : m_goal.newAutomationClipId();
        }
    }

    [[nodiscard]] core::ChannelId channel(core::ChannelId id) const {
        const auto found = m_channels.find(id.value);
        return found != m_channels.end() ? found->second : core::ChannelId{};
    }
    [[nodiscard]] core::SampleId sample(core::SampleId id) const {
        const auto found = m_samples.find(id.value);
        return found != m_samples.end() ? found->second : core::SampleId{};
    }
    [[nodiscard]] core::PatternId pattern(core::PatternId id) const {
        const auto found = m_patterns.find(id.value);
        return found != m_patterns.end() ? found->second : core::PatternId{};
    }
    [[nodiscard]] core::AutomationClipId autoClip(core::AutomationClipId id) const {
        const auto found = m_autoClips.find(id.value);
        return found != m_autoClips.end() ? found->second : core::AutomationClipId{};
    }

    /// A channel parameter names its channel by id; everything else by a number the
    /// file spells, which needs no mapping.
    [[nodiscard]] project::ParamRef param(project::ParamRef ref) const {
        switch (ref.kind) {
        case project::ParamKind::ChannelVolume:
        case project::ParamKind::ChannelPan:
        case project::ParamKind::ChannelPitch:
        case project::ParamKind::ChannelArpGate:
        case project::ParamKind::ChannelInstrumentParam:
            ref.owner = channel(core::ChannelId{ref.owner}).value;
            break;
        default:
            break;
        }
        return ref;
    }

    Channel remapChannel(const Channel& in) {
        Channel c = in;
        c.id = channel(in.id);
        for (project::SampleZone& zone : c.instrument.zones) {
            zone.sample = sample(zone.sample);
        }
        return c;
    }

    void remapAutomation(project::AutomationClip& clip, core::AutomationClipId id) const {
        clip.id = id;
        clip.target = param(clip.target);
    }

    /// Notes match by (start, pitch): the identity a person sees. Several notes at the
    /// same place pair up in order.
    NoteClip remapClip(const NoteClip& in, const NoteClip* existing) {
        NoteClip clip = in;
        clip.channel = channel(in.channel);
        std::map<std::pair<std::int64_t, int>, std::deque<core::NoteId>> available;
        if (existing != nullptr) {
            std::vector<const Note*> ordered;
            ordered.reserve(existing->notes.size());
            for (const Note& note : existing->notes) {
                ordered.push_back(&note);
            }
            std::ranges::sort(ordered, [](const Note* a, const Note* b) {
                return project::noteOrderBefore(*a, *b);
            });
            for (const Note* note : ordered) {
                available[{note->start.value, note->pitch}].push_back(note->id);
            }
        }
        std::map<std::uint32_t, core::NoteId> ids;
        for (Note& note : clip.notes) {
            auto& queue = available[{note.start.value, note.pitch}];
            core::NoteId id;
            if (!queue.empty()) {
                id = queue.front();
                queue.pop_front();
            } else {
                id = m_goal.newNoteId();
            }
            ids[note.id.value] = id;
            note.id = id;
        }
        for (project::NoteExtras& extras : clip.extras) {
            extras.note = ids.at(extras.note.value);
        }
        sortBy(clip.extras, [](const project::NoteExtras& e) { return e.note.value; });
        return clip;
    }

    Pattern remapPattern(const Pattern& in) {
        Pattern p = in;
        p.id = pattern(in.id);
        const Pattern* existing = m_cur.find(p.id);
        for (NoteClip& clip : p.noteClips) {
            const NoteClip* old =
                existing != nullptr ? existing->clipFor(channel(clip.channel)) : nullptr;
            clip = remapClip(clip, old);
        }
        for (project::MiniNotationSource& source : p.mini) {
            source.channel = channel(source.channel);
        }
        static const std::vector<project::AutomationClip> kNone;
        mapAutoClips(existing != nullptr ? existing->autoClips : kNone, in.autoClips);
        for (project::AutomationClip& clip : p.autoClips) {
            remapAutomation(clip, autoClip(clip.id));
        }
        return p;
    }

    [[nodiscard]] static std::tuple<std::size_t, std::uint32_t>
    contentKey(const PlaylistItem& item) {
        if (const auto* placed = std::get_if<project::PatternRef>(&item.content)) {
            return {0, placed->pattern.value};
        }
        if (const auto* audio = std::get_if<project::AudioClipRef>(&item.content)) {
            return {1, audio->sample.value};
        }
        return {2, std::get<project::AutomationRef>(item.content).clip.value};
    }

    void remapContent(PlaylistItem& item) const {
        if (auto* placed = std::get_if<project::PatternRef>(&item.content)) {
            placed->pattern = pattern(placed->pattern);
        } else if (auto* audio = std::get_if<project::AudioClipRef>(&item.content)) {
            audio->sample = sample(audio->sample);
        } else if (auto* automation = std::get_if<project::AutomationRef>(&item.content)) {
            automation->clip = autoClip(automation->clip);
        }
        for (project::ClipEnvelope& envelope : item.envelopes) {
            envelope.target = param(envelope.target);
        }
    }

    /// Items match within a track by (start, what they place); a moved item is
    /// matched by what it places alone, in order, so it keeps its id.
    void remapTracks(std::vector<PlaylistTrack>& tracks,
                     const std::vector<PlaylistTrack>& current) {
        for (PlaylistTrack& track : tracks) {
            const auto old = std::ranges::find(current, track.id, &PlaylistTrack::id);
            std::vector<bool> used(old != current.end() ? old->items.size() : 0, false);
            for (PlaylistItem& item : track.items) {
                remapContent(item);
            }
            std::vector<core::ItemId> ids(track.items.size());
            const auto claim = [&](std::size_t index, bool requireStart) {
                if (old == current.end()) {
                    return;
                }
                const PlaylistItem& item = track.items[index];
                for (std::size_t i = 0; i < old->items.size(); ++i) {
                    if (!used[i] && contentKey(old->items[i]) == contentKey(item) &&
                        (!requireStart || old->items[i].start == item.start)) {
                        used[i] = true;
                        ids[index] = old->items[i].id;
                        return;
                    }
                }
            };
            for (std::size_t i = 0; i < track.items.size(); ++i) {
                claim(i, true);
            }
            for (std::size_t i = 0; i < track.items.size(); ++i) {
                if (!ids[i].valid()) {
                    claim(i, false);
                }
            }
            for (std::size_t i = 0; i < track.items.size(); ++i) {
                track.items[i].id = ids[i].valid() ? ids[i] : m_goal.newItemId();
            }
        }
    }

    void remapPlaylist() {
        m_goal.playlist = m_target.playlist;
        remapTracks(m_goal.playlist.tracks, m_cur.playlist.tracks);
        for (project::Arrangement& arrangement : m_goal.playlist.arrangements) {
            const auto old = std::ranges::find(m_cur.playlist.arrangements, arrangement.name,
                                               &project::Arrangement::name);
            static const std::vector<PlaylistTrack> kNone;
            remapTracks(arrangement.tracks,
                        old != m_cur.playlist.arrangements.end() ? old->tracks : kNone);
        }
        for (project::AutomationClip& clip : m_goal.playlist.autoClips) {
            remapAutomation(clip, autoClip(clip.id));
        }
    }
};

// --- step 2: the narrow commands ---------------------------------------------------

using Commands = std::vector<std::unique_ptr<project::Command>>;

/// One changed note field, or nothing when the clips differ in more than that.
std::unique_ptr<project::Command> singleNoteEdit(core::PatternId pattern, const NoteClip& a,
                                                 const NoteClip& g) {
    if (a.notes.size() != g.notes.size() || a.extras != g.extras) {
        return nullptr;
    }
    const Note* before = nullptr;
    const Note* after = nullptr;
    for (const Note& note : g.notes) {
        const auto match = std::ranges::find(a.notes, note.id, &Note::id);
        if (match == a.notes.end()) {
            return nullptr;
        }
        if (!(*match == note)) {
            if (after != nullptr) {
                return nullptr;
            }
            before = &*match;
            after = &note;
        }
    }
    if (after == nullptr) {
        return nullptr;
    }
    using F = project::NoteField;
    std::vector<std::pair<F, double>> changed;
    const auto check = [&](bool differs, F field, double value) {
        if (differs) {
            changed.emplace_back(field, value);
        }
    };
    check(before->start != after->start, F::Start, static_cast<double>(after->start.value));
    check(before->length != after->length, F::Length, static_cast<double>(after->length.value));
    check(before->pitch != after->pitch, F::Pitch, after->pitch);
    check(before->velocity != after->velocity, F::Velocity, after->velocity);
    check(before->fineTuneCents != after->fineTuneCents, F::FineTuneCents, after->fineTuneCents);
    check(before->releaseVelocity != after->releaseVelocity, F::ReleaseVelocity,
          after->releaseVelocity);
    check(before->pan != after->pan, F::Pan, after->pan);
    check(before->cutoff != after->cutoff, F::Cutoff, after->cutoff);
    check(before->resonance != after->resonance, F::Resonance, after->resonance);
    check(before->muted != after->muted, F::Muted, after->muted ? 1.0 : 0.0);
    if (changed.size() != 1) {
        return nullptr;
    }
    return std::make_unique<project::SetNoteValue>(pattern, g.channel,
                                                   std::vector<core::NoteId>{after->id},
                                                   changed[0].first, changed[0].second);
}

void channelCommands(const Channel& a, const Channel& g, Commands& out) {
    if (a == g) {
        return;
    }
    Channel x = a;
    Commands found;
    using F = project::ChannelField;
    const auto value = [&](auto member, F field, double v) {
        if (x.*member != g.*member) {
            x.*member = g.*member;
            found.push_back(std::make_unique<project::SetChannelValue>(a.id, field, v));
        }
    };
    value(&Channel::volume, F::Volume, g.volume);
    value(&Channel::pan, F::Pan, g.pan);
    value(&Channel::pitchOffsetCents, F::PitchCents, g.pitchOffsetCents);
    value(&Channel::muted, F::Muted, g.muted ? 1.0 : 0.0);
    value(&Channel::soloed, F::Soloed, g.soloed ? 1.0 : 0.0);
    value(&Channel::maxPolyphony, F::MaxPolyphony, g.maxPolyphony);
    value(&Channel::stealMode, F::StealMode, static_cast<double>(g.stealMode));
    const bool sameNames =
        std::ranges::equal(x.instrument.params, g.instrument.params, {}, &project::ParamValue::name,
                           &project::ParamValue::name);
    if (sameNames && x.instrument.type == g.instrument.type) {
        for (std::size_t i = 0; i < g.instrument.params.size(); ++i) {
            const project::ParamValue& want = g.instrument.params[i];
            if (x.instrument.params[i] == want) {
                continue;
            }
            x.instrument.params[i] = want;
            found.push_back(want.hasCurve ? std::make_unique<project::SetChannelParam>(
                                                a.id, want.name, want.value, want.curve)
                                          : std::make_unique<project::SetChannelParam>(
                                                a.id, want.name, want.value));
        }
    }
    if (x == g) {
        std::ranges::move(found, std::back_inserter(out));
    }
}

void insertCommands(const Insert& a, const Insert& g, Commands& out) {
    if (a == g) {
        return;
    }
    Insert x = a;
    Commands found;
    using F = project::InsertField;
    const auto value = [&](auto member, F field, double v) {
        if (x.*member != g.*member) {
            x.*member = g.*member;
            found.push_back(std::make_unique<project::SetInsertValue>(a.id, field, v));
        }
    };
    value(&Insert::gain, F::Gain, g.gain);
    value(&Insert::pan, F::Pan, g.pan);
    value(&Insert::stereoSeparation, F::StereoSeparation, g.stereoSeparation);
    value(&Insert::muted, F::Muted, g.muted ? 1.0 : 0.0);
    value(&Insert::soloed, F::Soloed, g.soloed ? 1.0 : 0.0);
    value(&Insert::polarityInvert, F::PolarityInvert, g.polarityInvert ? 1.0 : 0.0);
    if (x.slots.size() == g.slots.size()) {
        for (std::size_t s = 0; s < g.slots.size(); ++s) {
            project::Slot& slot = x.slots[s];
            const project::Slot& want = g.slots[s];
            if (slot.id != want.id || slot.type != want.type) {
                continue;
            }
            if (slot.mix != want.mix) {
                slot.mix = want.mix;
                found.push_back(std::make_unique<project::SetSlotValue>(
                    slot.id, project::SlotField::Mix, want.mix));
            }
            if (slot.bypass != want.bypass) {
                slot.bypass = want.bypass;
                found.push_back(std::make_unique<project::SetSlotValue>(
                    slot.id, project::SlotField::Bypass, want.bypass ? 1.0 : 0.0));
            }
            const bool sameNames = std::ranges::equal(
                slot.params, want.params, {}, &project::SlotParam::name, &project::SlotParam::name);
            if (sameNames) {
                for (std::size_t p = 0; p < want.params.size(); ++p) {
                    if (slot.params[p].value != want.params[p].value) {
                        slot.params[p].value = want.params[p].value;
                        found.push_back(std::make_unique<project::SetSlotValue>(
                            slot.id, want.params[p].name, want.params[p].value));
                    }
                }
            }
        }
    }
    for (project::Send& send : x.sends) {
        const auto want = std::ranges::find(g.sends, send.id, &project::Send::id);
        if (want != g.sends.end() && want->target == send.target &&
            want->preFader == send.preFader && want->level != send.level) {
            send.level = want->level;
            found.push_back(std::make_unique<project::SetSendLevel>(send.id, want->level));
        }
    }
    if (x == g) {
        std::ranges::move(found, std::back_inserter(out));
    }
}

void patternCommands(const Pattern& a, const Pattern& g, Commands& out) {
    Pattern x = a;
    Commands found;
    if (x.length != g.length) {
        x.length = g.length;
        found.push_back(std::make_unique<project::SetPatternLength>(a.id, g.length));
    }
    if (x.color != g.color) {
        x.color = g.color;
        found.push_back(std::make_unique<project::SetPatternColor>(a.id, g.color));
    }
    // Mini-notation: SetMiniNotation adds, changes and (given no lines) removes.
    for (const project::MiniNotationSource& want : g.mini) {
        const auto have =
            std::ranges::find(x.mini, want.channel, &project::MiniNotationSource::channel);
        if (have == x.mini.end() || have->lines != want.lines) {
            found.push_back(
                std::make_unique<project::SetMiniNotation>(a.id, want.channel, want.lines));
        }
    }
    for (const project::MiniNotationSource& have : a.mini) {
        if (std::ranges::find(g.mini, have.channel, &project::MiniNotationSource::channel) ==
            g.mini.end()) {
            found.push_back(std::make_unique<project::SetMiniNotation>(a.id, have.channel,
                                                                       std::vector<std::string>{}));
        }
    }
    x.mini = g.mini;
    // Note clips: one command per clip that changed.
    for (const NoteClip& want : g.noteClips) {
        const NoteClip* have = x.clipFor(want.channel);
        if (have != nullptr && sameClip(*have, want)) {
            continue;
        }
        std::unique_ptr<project::Command> edit =
            have != nullptr ? singleNoteEdit(a.id, *have, want) : nullptr;
        found.push_back(edit ? std::move(edit)
                             : std::make_unique<project::SetNoteClip>(a.id, want.channel, want));
    }
    for (const NoteClip& have : a.noteClips) {
        if (g.clipFor(have.channel) == nullptr) {
            found.push_back(
                std::make_unique<project::SetNoteClip>(a.id, have.channel, std::nullopt));
        }
    }
    x.noteClips = g.noteClips;
    // Automation: breakpoints of a clip both sides have.
    if (x.autoClips.size() == g.autoClips.size()) {
        for (project::AutomationClip& clip : x.autoClips) {
            const auto want = std::ranges::find(g.autoClips, clip.id, &project::AutomationClip::id);
            if (want != g.autoClips.end() && want->targetPath == clip.targetPath &&
                want->points != clip.points) {
                clip.points = want->points;
                found.push_back(std::make_unique<project::SetBreakpoints>(clip.id, want->points));
            }
        }
    }
    if (x.name == g.name && x.length == g.length && x.color == g.color &&
        x.autoClips == g.autoClips && x.mini == g.mini) {
        std::ranges::move(found, std::back_inserter(out));
    }
}

void tempoCommands(const core::TempoMap& a, const core::TempoMap& g, Commands& out) {
    if (a == g) {
        return;
    }
    const auto ae = a.view().events;
    const auto ge = g.view().events;
    if (ae.size() != ge.size() || !std::ranges::equal(a.meterEvents(), g.meterEvents())) {
        return;
    }
    Commands found;
    for (std::size_t i = 0; i < ge.size(); ++i) {
        if (ae[i].at != ge[i].at) {
            return;
        }
        if (!(ae[i] == ge[i])) {
            found.push_back(
                std::make_unique<project::SetTempoEvent>(ge[i].at, ge[i].bpm, ge[i].ramp));
        }
    }
    std::ranges::move(found, std::back_inserter(out));
}

void playlistCommands(const project::Playlist& a, const project::Playlist& g, Commands& out) {
    // An item that only moved along its track.
    for (const PlaylistTrack& track : g.tracks) {
        const PlaylistTrack* old = a.find(track.id);
        if (old == nullptr) {
            continue;
        }
        for (const PlaylistItem& item : track.items) {
            const auto was = std::ranges::find(old->items, item.id, &PlaylistItem::id);
            if (was == old->items.end() || was->start == item.start) {
                continue;
            }
            PlaylistItem moved = *was;
            moved.start = item.start;
            if (moved == item) {
                out.push_back(std::make_unique<project::MovePlaylistItem>(track.id, item.id,
                                                                          item.start, track.id));
            }
        }
    }
}

Commands narrowCommands(const Project& cur, const Project& goal) {
    Commands out;
    if (!(cur.meta == goal.meta)) {
        out.push_back(std::make_unique<project::SetMeta>(goal.meta));
    }
    tempoCommands(cur.tempo, goal.tempo, out);
    for (const Channel& g : goal.channels) {
        if (const Channel* a = cur.find(g.id)) {
            channelCommands(*a, g, out);
        }
    }
    for (const Insert& g : goal.mixer.inserts) {
        if (const Insert* a = cur.mixer.find(g.id)) {
            insertCommands(*a, g, out);
        }
    }
    for (const Pattern& g : goal.patterns) {
        if (const Pattern* a = cur.find(g.id)) {
            patternCommands(*a, g, out);
        }
    }
    for (const project::Marker& g : goal.markers) {
        const project::Marker* a = cur.find(g.id);
        if (a != nullptr && !(*a == g)) {
            out.push_back(std::make_unique<project::MoveMarker>(g.id, g.at, g.name));
        }
    }
    playlistCommands(cur.playlist, goal.playlist, out);
    return out;
}

} // namespace

Project remapInto(const Project& current, const Project& target) {
    return Remapper(current, target).run();
}

bool sameContent(const Project& a, const Project& b) {
    return contentEqual(normalised(a), normalised(b));
}

PatchResult applyPatch(Project& current, project::CommandStack& stack, const Project& target,
                       std::string_view label) {
    PatchResult result;
    Project goal = remapInto(current, target);
    if (sameContent(current, goal)) {
        return result;
    }
    stack.beginGroup(label);
    for (std::unique_ptr<project::Command>& command : narrowCommands(current, goal)) {
        stack.execute(std::move(command), current);
        ++result.commands;
    }
    if (!sameContent(current, goal)) {
        stack.execute(std::make_unique<project::ReplaceProject>(std::move(goal)), current);
        ++result.commands;
        result.structural = true;
    }
    stack.endGroup();
    return result;
}

} // namespace adx::format

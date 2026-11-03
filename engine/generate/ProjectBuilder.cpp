// adx-thread: main
#include "engine/generate/ProjectBuilder.h"

#include <algorithm>

namespace adx::generate {

ProjectBuilder::ProjectBuilder(const preset::PresetLibrary& presets, std::string pack, double bpm,
                               std::string title)
    : m_presets(presets), m_pack(std::move(pack)) {
    m_project.meta.title = std::move(title);
    m_project.tempo.setTempo(core::Ticks{0}, bpm, false);
    m_project.tempo.setMeter(core::Ticks{0}, 4, 4);
    project::Insert master;
    master.id = m_project.newInsertId();
    master.name = "Master";
    m_project.mixer.inserts.push_back(master);
    m_project.mixer.master = master.id;
}

const preset::Preset* ProjectBuilder::find(std::string_view name) {
    const preset::Preset* found = m_presets.find(name, m_pack);
    if (found == nullptr) {
        m_notes.push_back("preset not found: " + std::string(name) + " (" + m_pack + ")");
    }
    return found;
}

core::InsertId ProjectBuilder::insert(std::string name, core::InsertId to, float gain) {
    project::Insert bus;
    bus.id = m_project.newInsertId();
    bus.name = std::move(name);
    bus.gain = gain;
    m_project.mixer.inserts.push_back(bus);
    m_project.mixer.routes.push_back(
        project::Route{.id = m_project.newRouteId(),
                       .from = bus.id,
                       .to = to.valid() ? to : m_project.mixer.master});
    return bus.id;
}

core::ChannelId ProjectBuilder::channel(std::string name, std::string_view preset,
                                        core::InsertId output, float volume, float pan,
                                        std::uint16_t polyphony) {
    project::Channel channel;
    channel.id = m_project.newChannelId();
    channel.name = std::move(name);
    channel.output = output.valid() ? output : m_project.mixer.master;
    channel.volume = volume;
    channel.pan = pan;
    channel.maxPolyphony = polyphony;
    if (const preset::Preset* found = find(preset)) {
        channel.instrument = preset::instrumentOf(*found);
    }
    m_project.channels.push_back(std::move(channel));
    return m_project.channels.back().id;
}

core::SlotId ProjectBuilder::effect(core::InsertId insert, std::string_view preset,
                                    core::InsertId sidechain) {
    project::Insert* target = m_project.mixer.find(insert);
    const preset::Preset* found = find(preset);
    if (target == nullptr || found == nullptr) {
        return {};
    }
    project::Slot slot;
    slot.id = m_project.newSlotId();
    slot.type = found->type;
    slot.mix = static_cast<float>(found->mix.value_or(1.0));
    for (const project::ParamValue& p : found->params) {
        slot.params.push_back(project::SlotParam{.name = p.name, .value = p.value});
    }
    slot.sidechain = sidechain;
    target->slots.push_back(slot);
    return slot.id;
}

core::SlotId ProjectBuilder::effectType(core::InsertId insert, std::string type) {
    project::Insert* target = m_project.mixer.find(insert);
    if (target == nullptr) {
        return {};
    }
    project::Slot slot;
    slot.id = m_project.newSlotId();
    slot.type = std::move(type);
    target->slots.push_back(slot);
    return slot.id;
}

void ProjectBuilder::send(core::InsertId from, core::InsertId to, float level) {
    if (project::Insert* source = m_project.mixer.find(from)) {
        source->sends.push_back(project::Send{
            .id = m_project.newSendId(), .target = to, .level = level, .preFader = false});
    }
}

core::PatternId ProjectBuilder::pattern(
    std::string name, std::int64_t bars,
    const std::vector<std::pair<core::ChannelId, std::vector<GenNote>>>& parts) {
    project::Pattern pattern;
    pattern.id = m_project.newPatternId();
    pattern.name = std::move(name);
    pattern.length = core::Ticks{bars * kBar};
    for (const auto& [channel, notes] : parts) {
        project::NoteClip clip{.channel = channel, .notes = {}, .extras = {}};
        std::vector<GenNote> sorted = notes;
        std::ranges::stable_sort(sorted, [](const GenNote& a, const GenNote& b) {
            return a.start < b.start || (a.start == b.start && a.pitch < b.pitch);
        });
        for (const GenNote& n : sorted) {
            if (n.start < 0 || n.start >= bars * kBar) {
                continue;
            }
            project::Note note;
            note.id = m_project.newNoteId();
            note.start = core::Ticks{n.start};
            note.length = core::Ticks{std::min(n.length, (bars * kBar) - n.start)};
            note.pitch = n.pitch;
            note.velocity = n.velocity;
            clip.notes.push_back(note);
        }
        pattern.noteClips.push_back(std::move(clip));
    }
    m_project.patterns.push_back(std::move(pattern));
    return m_project.patterns.back().id;
}

void ProjectBuilder::place(std::string_view track, core::PatternId pattern, std::int64_t startBar,
                           std::int64_t bars) {
    auto it = std::ranges::find_if(m_project.playlist.tracks, [&](const project::PlaylistTrack& t) {
        return t.name == track;
    });
    if (it == m_project.playlist.tracks.end()) {
        project::PlaylistTrack created;
        created.id = m_project.newPlaylistTrackId();
        created.name = std::string(track);
        m_project.playlist.tracks.push_back(std::move(created));
        it = std::prev(m_project.playlist.tracks.end());
    }
    it->items.push_back(project::PlaylistItem{.id = m_project.newItemId(),
                                              .start = core::Ticks{startBar * kBar},
                                              .length = core::Ticks{bars * kBar},
                                              .sourceOffset = core::Ticks{0},
                                              .content = project::PatternRef{pattern},
                                              .muted = false,
                                              .envelopes = {}});
}

void ProjectBuilder::marker(std::int64_t bar, std::string name) {
    m_project.markers.push_back(project::Marker{
        .id = m_project.newMarkerId(), .at = core::Ticks{bar * kBar}, .name = std::move(name)});
}

} // namespace adx::generate

// The project's structure, read by the Phase 6 panels (phase_6.md §3 rule 2: "everything
// else is read from the engine").
//
// Channels, inserts, patterns, tracks, controllers: a few hundred entities at most, read
// once per edit - O(interactions), which FINAL_PLAN §2.2 Rule 2 allows - as plain dicts.
// Anything per placement or per breakpoint crosses as ONE numpy array, never a list of
// objects: a playlist's items are one structured array, an automation clip's points one
// (n, 8) float64 array. Notes stay where Phase 5 put them (bindings/notes.cpp).

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/PendingCommand.h"
#include "bindings/ProjectHandle.h"
#include "engine/core/Curve.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/TypeCatalog.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"

namespace py = pybind11;

using adx::bindings::colorText;
using adx::bindings::ProjectHandle;
namespace project = adx::project;

namespace {

const char* unitName(project::Unit unit) noexcept {
    switch (unit) {
    case project::Unit::Normalized:
        return "normalized";
    case project::Unit::Hertz:
        return "Hz";
    case project::Unit::Decibels:
        return "dB";
    case project::Unit::Milliseconds:
        return "ms";
    case project::Unit::Seconds:
        return "s";
    case project::Unit::Ratio:
        return "ratio";
    case project::Unit::Semitones:
        return "st";
    case project::Unit::Cents:
        return "cents";
    case project::Unit::Percent:
        return "%";
    case project::Unit::Count:
        return "count";
    case project::Unit::Boolean:
        return "bool";
    }
    return "normalized";
}

const char* scaleName(project::ScaleKind scale) noexcept {
    switch (scale) {
    case project::ScaleKind::Logarithmic:
        return "log";
    case project::ScaleKind::Stepped:
        return "stepped";
    case project::ScaleKind::Linear:
        break;
    }
    return "linear";
}

const char* rateName(project::RateClass rate) noexcept {
    switch (rate) {
    case project::RateClass::Block:
        return "block";
    case project::RateClass::Baked:
        return "baked";
    case project::RateClass::Sample:
        break;
    }
    return "sample";
}

py::dict descriptorDict(const project::ParamDescriptor& d) {
    py::dict out;
    out["name"] = std::string(d.name);
    out["min"] = d.minimum;
    out["max"] = d.maximum;
    out["default"] = d.defaultValue;
    out["unit"] = unitName(d.unit);
    out["scale"] = scaleName(d.scale);
    out["rate"] = rateName(d.rate);
    out["curve_part"] = static_cast<int>(d.curve);
    return out;
}

py::list typeList(std::span<const project::TypeInfo> types, const char* kind) {
    py::list out;
    for (const project::TypeInfo& type : types) {
        py::dict entry;
        entry["name"] = std::string(type.name);
        entry["kind"] = kind;
        entry["summary"] = std::string(type.summary);
        py::list params;
        for (const project::ParamDescriptor& d : type.params) {
            params.append(descriptorDict(d));
        }
        entry["params"] = params;
        out.append(entry);
    }
    return out;
}

py::tuple curveTuple(const adx::core::Curve& curve) {
    return py::make_tuple(static_cast<int>(curve.kind), curve.tension, curve.c1x, curve.c1y,
                          curve.c2x, curve.c2y);
}

py::list channelsInfo(const ProjectHandle& handle) {
    py::list out;
    for (const project::Channel& c : handle.project.channels) {
        py::dict d;
        d["id"] = c.id.value;
        d["name"] = c.name;
        d["color"] = colorText(c.color);
        d["instrument"] = c.instrument.type;
        d["known"] = project::findInstrumentType(c.instrument.type) != nullptr;
        py::list params;
        for (const project::ParamValue& p : c.instrument.params) {
            params.append(py::make_tuple(p.name, p.value, p.hasCurve, curveTuple(p.curve)));
        }
        d["params"] = params;
        d["output"] = c.output.value;
        d["polyphony"] = c.maxPolyphony;
        d["steal"] = std::string(project::toString(c.stealMode));
        d["muted"] = c.muted;
        d["soloed"] = c.soloed;
        d["volume"] = c.volume;
        d["pan"] = c.pan;
        d["pitch"] = c.pitchOffsetCents;
        d["arp"] = py::make_tuple(std::string(project::toString(c.arp.mode)), c.arp.rate.numerator,
                                  c.arp.rate.denominator, c.arp.octaves, c.arp.gate);
        d["zones"] = c.instrument.zones.size();
        d["voicebank"] = c.instrument.voicebank;
        out.append(d);
    }
    return out;
}

py::dict mixerInfo(const ProjectHandle& handle) {
    const project::Mixer& mixer = handle.project.mixer;
    py::list inserts;
    for (const project::Insert& insert : mixer.inserts) {
        py::dict d;
        d["id"] = insert.id.value;
        d["name"] = insert.name;
        d["color"] = colorText(insert.color);
        d["gain"] = insert.gain;
        d["pan"] = insert.pan;
        d["width"] = insert.stereoSeparation;
        d["muted"] = insert.muted;
        d["soloed"] = insert.soloed;
        d["invert"] = insert.polarityInvert;
        d["master"] = insert.id == mixer.master;
        py::list slots;
        for (const project::Slot& slot : insert.slots) {
            py::dict s;
            s["id"] = slot.id.value;
            s["type"] = slot.type;
            s["known"] = project::findEffectType(slot.type) != nullptr;
            s["mix"] = slot.mix;
            s["bypass"] = slot.bypass;
            s["sidechain"] = slot.sidechain.value;
            const project::SampleRef* ir = handle.project.resources.find(slot.impulse);
            s["ir"] = ir != nullptr ? ir->path : std::string{};
            py::list params;
            for (const project::SlotParam& p : slot.params) {
                params.append(py::make_tuple(p.name, p.value));
            }
            s["params"] = params;
            slots.append(s);
        }
        d["slots"] = slots;
        py::list sends;
        for (const project::Send& send : insert.sends) {
            py::dict s;
            s["id"] = send.id.value;
            s["target"] = send.target.value;
            s["level"] = send.level;
            s["pre"] = send.preFader;
            sends.append(s);
        }
        d["sends"] = sends;
        inserts.append(d);
    }
    py::list routes;
    for (const project::Route& route : mixer.routes) {
        routes.append(py::make_tuple(route.id.value, route.from.value, route.to.value));
    }
    py::dict out;
    out["inserts"] = inserts;
    out["routes"] = routes;
    out["master"] = mixer.master.value;
    return out;
}

py::list patternsInfo(const ProjectHandle& handle) {
    py::list out;
    for (const project::Pattern& pattern : handle.project.patterns) {
        py::dict d;
        d["id"] = pattern.id.value;
        d["name"] = pattern.name;
        d["color"] = colorText(pattern.color);
        d["length"] = pattern.length.value;
        std::vector<std::uint32_t> channels;
        std::size_t notes = 0;
        for (const project::NoteClip& clip : pattern.noteClips) {
            channels.push_back(clip.channel.value);
            notes += clip.notes.size();
        }
        d["channels"] = channels;
        d["notes"] = notes;
        std::vector<std::uint32_t> clips;
        clips.reserve(pattern.autoClips.size());
        for (const project::AutomationClip& clip : pattern.autoClips) {
            clips.push_back(clip.id.value);
        }
        d["auto_clips"] = clips;
        out.append(d);
    }
    return out;
}

/// One placement, as a row of the structured array playlist_items() returns.
struct ItemRecord {
    std::uint32_t track;
    std::uint32_t item;
    std::int64_t start;
    std::int64_t length;
    std::int64_t offset;
    std::uint32_t ref;
    float stretch;
    float pitch;
    std::uint16_t envelopes;
    std::uint8_t kind;
    std::uint8_t muted;
    std::uint8_t reverse;
    std::uint8_t explicitLength;
};

py::dtype itemDtype() {
    py::list names;
    py::list formats;
    py::list offsets;
    const auto field = [&](const char* name, const char* format, std::size_t offset) {
        names.append(name);
        formats.append(format);
        offsets.append(offset);
    };
    field("track", "<u4", offsetof(ItemRecord, track));
    field("item", "<u4", offsetof(ItemRecord, item));
    field("start", "<i8", offsetof(ItemRecord, start));
    field("length", "<i8", offsetof(ItemRecord, length));
    field("offset", "<i8", offsetof(ItemRecord, offset));
    field("ref", "<u4", offsetof(ItemRecord, ref));
    field("stretch", "<f4", offsetof(ItemRecord, stretch));
    field("pitch", "<f4", offsetof(ItemRecord, pitch));
    field("envelopes", "<u2", offsetof(ItemRecord, envelopes));
    field("kind", "u1", offsetof(ItemRecord, kind));
    field("muted", "u1", offsetof(ItemRecord, muted));
    field("reverse", "u1", offsetof(ItemRecord, reverse));
    field("explicit_length", "u1", offsetof(ItemRecord, explicitLength));
    return {names, formats, offsets, static_cast<py::ssize_t>(sizeof(ItemRecord))};
}

const py::dtype& itemDtypeObject() {
    // Never destroyed, for the reason NOTE_DTYPE is not (bindings/notes.cpp).
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    static const py::dtype* const kDtype = new py::dtype(itemDtype());
    return *kDtype;
}

std::int64_t resolvedLength(const project::Project& p, const project::PlaylistItem& item) {
    if (item.length.value > 0) {
        return item.length.value;
    }
    if (const auto* ref = std::get_if<project::PatternRef>(&item.content)) {
        const project::Pattern* pattern = p.find(ref->pattern);
        return pattern != nullptr ? pattern->length.value : adx::core::kPpq * 4;
    }
    return adx::core::kPpq * 4;
}

py::array playlistItems(const ProjectHandle& handle) {
    const project::Project& p = handle.project;
    std::size_t count = 0;
    for (const project::PlaylistTrack& track : p.playlist.tracks) {
        count += track.items.size();
    }
    py::array out(itemDtypeObject(), std::vector<py::ssize_t>{static_cast<py::ssize_t>(count)},
                  std::vector<py::ssize_t>{});
    if (count == 0) {
        return out;
    }
    auto* rows = static_cast<ItemRecord*>(out.mutable_data());
    const py::gil_scoped_release released;
    std::size_t row = 0;
    for (const project::PlaylistTrack& track : p.playlist.tracks) {
        for (const project::PlaylistItem& item : track.items) {
            ItemRecord record{.track = track.id.value,
                              .item = item.id.value,
                              .start = item.start.value,
                              .length = resolvedLength(p, item),
                              .offset = item.sourceOffset.value,
                              .ref = 0,
                              .stretch = 1.0F,
                              .pitch = 0.0F,
                              .envelopes = static_cast<std::uint16_t>(item.envelopes.size()),
                              .kind = 0,
                              .muted = static_cast<std::uint8_t>(item.muted ? 1 : 0),
                              .reverse = 0,
                              .explicitLength = static_cast<std::uint8_t>(item.length.value > 0)};
            if (const auto* ref = std::get_if<project::PatternRef>(&item.content)) {
                record.ref = ref->pattern.value;
            } else if (const auto* audio = std::get_if<project::AudioClipRef>(&item.content)) {
                record.kind = 1;
                record.ref = audio->sample.value;
                record.stretch = audio->stretch;
                record.pitch = audio->pitchSemitones;
                record.reverse = static_cast<std::uint8_t>(audio->reverse ? 1 : 0);
            } else if (const auto* automation =
                           std::get_if<project::AutomationRef>(&item.content)) {
                record.kind = 2;
                record.ref = automation->clip.value;
            }
            rows[row++] = record;
        }
    }
    return out;
}

py::dict playlistInfo(const ProjectHandle& handle) {
    const project::Playlist& playlist = handle.project.playlist;
    py::list tracks;
    for (const project::PlaylistTrack& track : playlist.tracks) {
        py::dict d;
        d["id"] = track.id.value;
        d["name"] = track.name;
        d["color"] = colorText(track.color);
        d["height"] = track.height;
        d["muted"] = track.muted;
        d["soloed"] = track.soloed;
        d["group"] = track.group.value;
        d["collapsed"] = track.collapsed;
        d["audible"] = playlist.audible(track);
        d["items"] = track.items.size();
        tracks.append(d);
    }
    std::vector<std::string> arrangements;
    arrangements.reserve(playlist.arrangements.size());
    for (const project::Arrangement& arrangement : playlist.arrangements) {
        arrangements.push_back(arrangement.name);
    }
    py::dict out;
    out["name"] = playlist.arrangementName;
    out["arrangements"] = arrangements;
    out["tracks"] = tracks;
    return out;
}

/// Breakpoints as one (n, 8) float64 array: tick, value, curve kind, tension, c1x, c1y,
/// c2x, c2y.
py::array_t<double> pointsArray(const std::vector<project::Breakpoint>& points) {
    py::array_t<double> out({static_cast<py::ssize_t>(points.size()), py::ssize_t{8}});
    auto rows = out.mutable_unchecked<2>();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const project::Breakpoint& p = points[i];
        const auto r = static_cast<py::ssize_t>(i);
        rows(r, 0) = static_cast<double>(p.at.value);
        rows(r, 1) = p.value;
        rows(r, 2) = static_cast<double>(p.curve.kind);
        rows(r, 3) = p.curve.tension;
        rows(r, 4) = p.curve.c1x;
        rows(r, 5) = p.curve.c1y;
        rows(r, 6) = p.curve.c2x;
        rows(r, 7) = p.curve.c2y;
    }
    return out;
}

std::string pathOrWritten(const project::Project& p, project::ParamRef ref,
                          const std::string& written) {
    const std::string path = project::ParamRegistry::pathOf(ref, p);
    return path.empty() ? written : path;
}

py::list automationClips(const ProjectHandle& handle) {
    const project::Project& p = handle.project;
    py::list out;
    const auto add = [&](const project::AutomationClip& clip, std::uint32_t pattern) {
        py::dict d;
        d["id"] = clip.id.value;
        d["pattern"] = pattern;
        d["target"] = pathOrWritten(p, clip.target, clip.targetPath);
        d["valid"] = clip.target.valid();
        d["points"] = clip.points.size();
        out.append(d);
    };
    for (const project::Pattern& pattern : p.patterns) {
        for (const project::AutomationClip& clip : pattern.autoClips) {
            add(clip, pattern.id.value);
        }
    }
    for (const project::AutomationClip& clip : p.playlist.autoClips) {
        add(clip, 0);
    }
    return out;
}

py::list itemEnvelopes(const ProjectHandle& handle, std::uint32_t track, std::uint32_t item) {
    const project::PlaylistTrack* owner =
        handle.project.playlist.find(adx::core::PlaylistTrackId{track});
    py::list out;
    if (owner == nullptr) {
        return out;
    }
    const auto match =
        std::ranges::find(owner->items, adx::core::ItemId{item}, &project::PlaylistItem::id);
    if (match == owner->items.end()) {
        return out;
    }
    for (const project::ClipEnvelope& envelope : match->envelopes) {
        py::dict d;
        d["local"] = envelope.local == project::ClipTarget::Param
                         ? std::string{}
                         : std::string(project::toString(envelope.local));
        d["target"] = envelope.local == project::ClipTarget::Param
                          ? pathOrWritten(handle.project, envelope.target, envelope.targetPath)
                          : std::string(project::toString(envelope.local));
        d["points"] = pointsArray(envelope.points);
        out.append(d);
    }
    return out;
}

py::list controllersInfo(const ProjectHandle& handle) {
    py::list out;
    for (const project::Controller& c : handle.project.controllers) {
        py::dict d;
        d["id"] = c.id.value;
        d["name"] = c.name;
        d["kind"] = std::string(project::toString(c.kind));
        d["target"] = pathOrWritten(handle.project, c.target, c.targetPath);
        d["valid"] = c.target.valid();
        d["depth"] = c.depth;
        d["center"] = c.center;
        d["shape"] = std::string(project::toString(c.shape));
        d["period"] = c.period.value;
        d["phase"] = c.phase;
        d["source_channel"] = c.sourceChannel.value;
        d["source_insert"] = c.sourceInsert.value;
        d["attack"] = c.attackMs;
        d["decay"] = c.decayMs;
        d["sustain"] = c.sustain;
        d["release"] = c.releaseMs;
        d["gain"] = c.gainDb;
        out.append(d);
    }
    return out;
}

/// The value a parameter path names in the model, or None.
py::object paramValue(const ProjectHandle& handle, const std::string& path) {
    const project::ParamResolution resolved = project::ParamRegistry::resolve(path, handle.project);
    if (!resolved.ok()) {
        return py::none();
    }
    const project::Project& p = handle.project;
    const project::ParamRef ref = resolved.ref;
    const project::ParamDescriptor* descriptor = project::ParamRegistry::descriptorFor(ref, p);
    switch (ref.kind) {
    case project::ParamKind::ChannelVolume:
        return py::float_(p.find(adx::core::ChannelId{ref.owner})->volume);
    case project::ParamKind::ChannelPan:
        return py::float_(p.find(adx::core::ChannelId{ref.owner})->pan);
    case project::ParamKind::ChannelPitch:
        return py::float_(p.find(adx::core::ChannelId{ref.owner})->pitchOffsetCents);
    case project::ParamKind::ChannelArpGate:
        return py::float_(p.find(adx::core::ChannelId{ref.owner})->arp.gate);
    case project::ParamKind::ChannelInstrumentParam: {
        const project::Channel* channel = p.find(adx::core::ChannelId{ref.owner});
        if (ref.index < channel->instrument.params.size()) {
            return py::float_(channel->instrument.params[ref.index].value);
        }
        break;
    }
    case project::ParamKind::InsertGain:
        return py::float_(p.mixer.find(adx::core::InsertId{ref.owner})->gain);
    case project::ParamKind::InsertPan:
        return py::float_(p.mixer.find(adx::core::InsertId{ref.owner})->pan);
    case project::ParamKind::InsertWidth:
        return py::float_(p.mixer.find(adx::core::InsertId{ref.owner})->stereoSeparation);
    case project::ParamKind::SlotMix:
        return py::float_(p.mixer.findSlot(adx::core::SlotId{ref.owner})->mix);
    case project::ParamKind::SlotBypass:
        return py::float_(p.mixer.findSlot(adx::core::SlotId{ref.owner})->bypass ? 1.0 : 0.0);
    case project::ParamKind::SlotParam: {
        const project::Slot* slot = p.mixer.findSlot(adx::core::SlotId{ref.owner});
        if (ref.index < slot->params.size()) {
            return py::float_(slot->params[ref.index].value);
        }
        break;
    }
    case project::ParamKind::SendLevel:
        return py::float_(p.mixer.findSend(adx::core::SendId{ref.owner})->level);
    case project::ParamKind::None:
        return py::none();
    }
    return descriptor != nullptr ? py::object(py::float_(descriptor->defaultValue)) : py::none();
}

/// A parameter path's descriptor (range, unit, scale), or None.
py::object paramDescriptor(const ProjectHandle& handle, const std::string& path) {
    const project::ParamResolution resolved = project::ParamRegistry::resolve(path, handle.project);
    if (!resolved.ok()) {
        return py::none();
    }
    const project::ParamDescriptor* descriptor =
        project::ParamRegistry::descriptorFor(resolved.ref, handle.project);
    if (descriptor == nullptr) {
        descriptor = &project::ParamRegistry::describe(resolved.ref.kind);
    }
    return descriptorDict(*descriptor);
}

/// Notes `ids` of one clip as `.adx` text: a project fragment with one channel and one
/// pattern, written by the Phase 2 writer (phase_6.md §4.7) - so a copy pastes into a
/// text editor, another adX, or back.
std::string notesFragment(const ProjectHandle& handle, const std::string& pattern,
                          const std::string& channel, const std::vector<std::uint32_t>& ids) {
    const project::Pattern& sourcePattern = adx::bindings::patternNamed(handle.project, pattern);
    const project::Channel& sourceChannel = adx::bindings::channelNamed(handle.project, channel);
    const project::NoteClip* clip = sourcePattern.clipFor(sourceChannel.id);
    project::Project fragment;
    project::CommandStack stack;
    project::Insert master;
    master.name = "Master";
    stack.execute(std::make_unique<project::AddInsert>(std::move(master)), fragment);
    project::Channel prototype;
    prototype.name = sourceChannel.name;
    prototype.instrument.type = sourceChannel.instrument.type;
    prototype.output = fragment.mixer.master;
    stack.execute(std::make_unique<project::AddChannel>(std::move(prototype)), fragment);
    project::Pattern patternPrototype;
    patternPrototype.name = "Clipboard";
    std::vector<project::Note> notes;
    std::int64_t first = std::numeric_limits<std::int64_t>::max();
    if (clip != nullptr) {
        for (const project::Note& note : clip->notes) {
            if (std::ranges::find(ids, note.id.value) != ids.end()) {
                notes.push_back(note);
                first = std::min(first, note.start.value);
            }
        }
    }
    // Relative to the first copied note's bar, so a paste lands where the user points.
    const std::int64_t bar = adx::core::kPpq * 4;
    const std::int64_t origin = notes.empty() ? 0 : (first / bar) * bar;
    std::int64_t end = bar;
    for (project::Note& note : notes) {
        note.start = adx::core::Ticks{note.start.value - origin};
        end = std::max(end, note.end().value);
    }
    patternPrototype.length = adx::core::Ticks{((end + bar - 1) / bar) * bar};
    auto addPattern = std::make_unique<project::AddPattern>(std::move(patternPrototype));
    const project::AddPattern* raw = addPattern.get();
    stack.execute(std::move(addPattern), fragment);
    if (!notes.empty()) {
        stack.execute(std::make_unique<project::AddNotes>(
                          raw->created(), fragment.channels.front().id, std::move(notes)),
                      fragment);
    }
    return adx::format::write(fragment);
}

} // namespace

namespace adx::bindings {

void defineStructureAccess(py::class_<ProjectHandle>& project) {
    project
        // GIL: trivial - one dict per channel
        .def("channels_info", &channelsInfo, "Every channel's settings, in id order.")
        // GIL: trivial - one dict per insert, slot and send
        .def("mixer_info", &mixerInfo, "Inserts with their slots and sends, routes, the master.")
        // GIL: trivial - one dict per pattern
        .def("patterns_info", &patternsInfo, "Every pattern: id, name, colour, length, clips.")
        // GIL: trivial - one dict per track
        .def("playlist_info", &playlistInfo, "Tracks and arrangements (items: playlist_items).")
        .def("playlist_items", &playlistItems,
             "Every placement as ONE structured numpy array (adx_engine.ITEM_DTYPE).")
        // GIL: trivial - one dict per automation clip
        .def("automation_clips", &automationClips, "Every automation clip and its target.")
        .def(
            "automation_points",
            [](const ProjectHandle& handle, std::uint32_t clip) {
                const project::AutomationClip* found =
                    handle.project.findAutomationClip(adx::core::AutomationClipId{clip});
                if (found == nullptr) {
                    throw py::value_error("no automation clip " + std::to_string(clip));
                }
                return pointsArray(found->points);
            }, // GIL: trivial - one lane's points, one array
            py::arg("clip"),
            "A clip's breakpoints as one (n, 8) float64 array: tick, value, curve kind, "
            "tension, c1x, c1y, c2x, c2y.")
        // GIL: trivial - one placement's envelopes
        .def("item_envelopes", &itemEnvelopes, py::arg("track"), py::arg("item"),
             "A placement's clip envelopes: target and an (n, 8) points array each.")
        // GIL: trivial - one dict per controller
        .def("controllers_info", &controllersInfo, "Every controller's settings.")
        .def(
            "markers",
            [](const ProjectHandle& handle) {
                py::list out;
                for (const project::Marker& marker : handle.project.markers) {
                    out.append(py::make_tuple(marker.id.value, marker.at.value, marker.name));
                }
                return out;
            }, // GIL: trivial - one tuple per marker
            "(id, tick, name) per marker.")
        .def(
            "samples",
            [](const ProjectHandle& handle) {
                py::list out;
                const project::Resources& resources = handle.project.resources;
                for (const project::SampleRef& sample : resources.samples) {
                    std::filesystem::path full(sample.path);
                    if (full.is_relative() && !resources.baseDirectory.empty()) {
                        full = std::filesystem::path(resources.baseDirectory) / full;
                    }
                    out.append(py::make_tuple(sample.id.value, sample.path, full.generic_string()));
                }
                return out;
            }, // GIL: trivial - one tuple per pool entry
            "(id, path as written, resolved path) per pool entry.")
        .def(
            "history_info",
            [](const ProjectHandle& handle) {
                std::vector<std::string> done;
                for (const project::HistoryEntry& entry : handle.stack.history()) {
                    done.push_back(entry.label);
                }
                return py::make_tuple(done, handle.stack.redoLabels());
            }, // GIL: trivial - one label per history entry
            "(done labels oldest first, redo labels next first).")
        // GIL: trivial - one lookup
        .def("param_value", &paramValue, py::arg("path"),
             "The model's value of a parameter path, or None when it names nothing.")
        // GIL: trivial - one lookup
        .def("param_descriptor", &paramDescriptor, py::arg("path"),
             "A parameter path's range, unit and scale, or None.")
        .def(
            "param_valid",
            [](const ProjectHandle& handle, const std::string& path) {
                return project::ParamRegistry::resolve(path, handle.project).ok();
            }, // GIL: trivial - one lookup
            py::arg("path"))
        .def(
            "notes_fragment",
            [](const ProjectHandle& handle, const std::string& pattern, const std::string& channel,
               const std::vector<std::uint32_t>& ids) {
                const py::gil_scoped_release released;
                return notesFragment(handle, pattern, channel, ids);
            },
            py::arg("pattern"), py::arg("channel"), py::arg("ids"),
            "Notes as `.adx` text: a one-channel, one-pattern project (the clipboard).")
        .def("content_length", [](const ProjectHandle& handle) {
            return handle.project.contentLength().value;
        }); // GIL: trivial - the end of the last placement
}

void registerCatalog(py::module_& m) {
    m.attr("ITEM_DTYPE") = itemDtypeObject();
    // GIL: trivial - the static type tables
    m.def(
        "type_catalog",
        []() {
            py::list out = typeList(project::instrumentTypes(), "instrument");
            for (const py::handle entry : typeList(project::effectTypes(), "effect")) {
                out.append(entry);
            }
            return out;
        },
        "Every instrument and effect type, with its parameter descriptors.");
    // GIL: trivial - one curve evaluation
    m.def(
        "curve_evaluate",
        [](int kind, float tension, float c1x, float c1y, float c2x, float c2y, float t) {
            const adx::core::Curve curve{
                .kind = static_cast<adx::core::CurveKind>(
                    std::clamp(kind, 0, static_cast<int>(adx::core::kCurveKindCount) - 1)),
                .tension = tension,
                .c1x = c1x,
                .c1y = c1y,
                .c2x = c2x,
                .c2y = c2y};
            return curve.evaluate(t);
        },
        py::arg("kind"), py::arg("tension"), py::arg("c1x"), py::arg("c1y"), py::arg("c2x"),
        py::arg("c2y"), py::arg("t"), "core::Curve::evaluate: the engine's own curve.");
}

} // namespace adx::bindings

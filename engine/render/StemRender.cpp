// adx-thread: main
#include "engine/render/StemRender.h"

#include <algorithm>
#include <variant>

#include "engine/project/Project.h"

namespace adx::render {

namespace {

bool holdsAudio(const project::PlaylistTrack& track) {
    return std::ranges::any_of(track.items, [](const project::PlaylistItem& item) {
        return std::holds_alternative<project::AudioClipRef>(item.content);
    });
}

core::InsertId audioOutput(const project::Project& project, const project::PlaylistTrack& track) {
    return project.mixer.find(track.output) != nullptr ? track.output : project.mixer.master;
}

std::string trackName(const project::PlaylistTrack& track) {
    return track.name.empty() ? "Track " + std::to_string(track.id.value) : track.name;
}

} // namespace

const char* toString(StemMode mode) noexcept {
    switch (mode) {
    case StemMode::Channels:
        return "channels";
    case StemMode::Inserts:
        return "inserts";
    case StemMode::Patterns:
        return "patterns";
    }
    return "";
}

bool stemModeFromString(std::string_view name, StemMode& out) noexcept {
    for (const StemMode mode : {StemMode::Channels, StemMode::Inserts, StemMode::Patterns}) {
        if (name == toString(mode)) {
            out = mode;
            return true;
        }
    }
    return false;
}

namespace {

/// A stem with a name and nothing in it yet.
Stem named(std::string name) {
    Stem stem;
    stem.name = std::move(name);
    return stem;
}

} // namespace

std::vector<Stem> stemsOf(const project::Project& project, StemMode mode) {
    std::vector<Stem> stems;
    switch (mode) {
    case StemMode::Channels:
        for (const project::Channel& channel : project.channels) {
            if (project.mixer.find(channel.output) != nullptr) {
                Stem stem = named(channel.name);
                stem.spec.channels.push_back(channel.id);
                stems.push_back(std::move(stem));
            }
        }
        for (const project::PlaylistTrack& track : project.playlist.tracks) {
            if (holdsAudio(track)) {
                Stem stem = named(trackName(track));
                stem.spec.audioTracks.push_back(track.id);
                stems.push_back(std::move(stem));
            }
        }
        break;
    case StemMode::Inserts:
        for (const project::Insert& insert : project.mixer.inserts) {
            Stem stem = named(insert.name.empty() ? "Insert " + std::to_string(insert.id.value)
                                                  : insert.name);
            for (const project::Channel& channel : project.channels) {
                if (channel.output == insert.id) {
                    stem.spec.channels.push_back(channel.id);
                }
            }
            for (const project::PlaylistTrack& track : project.playlist.tracks) {
                if (holdsAudio(track) && audioOutput(project, track) == insert.id) {
                    stem.spec.audioTracks.push_back(track.id);
                }
            }
            if (!stem.spec.channels.empty() || !stem.spec.audioTracks.empty()) {
                stems.push_back(std::move(stem));
            }
        }
        break;
    case StemMode::Patterns: {
        std::vector<core::ChannelId> all;
        all.reserve(project.channels.size());
        for (const project::Channel& channel : project.channels) {
            all.push_back(channel.id);
        }
        for (const project::Pattern& pattern : project.patterns) {
            const bool placed =
                std::ranges::any_of(project.playlist.tracks, [&](const auto& track) {
                    return std::ranges::any_of(track.items, [&](const project::PlaylistItem& item) {
                        const auto* ref = std::get_if<project::PatternRef>(&item.content);
                        return ref != nullptr && ref->pattern == pattern.id;
                    });
                });
            if (placed) {
                Stem stem = named(pattern.name);
                stem.spec.channels = all;
                stem.spec.pattern = pattern.id;
                stems.push_back(std::move(stem));
            }
        }
        Stem frozen = named("Frozen channels");
        for (const project::Channel& channel : project.channels) {
            if (!channel.frozen.empty()) {
                frozen.spec.channels.push_back(channel.id);
            }
        }
        if (!frozen.spec.channels.empty()) {
            stems.push_back(std::move(frozen));
        }
        Stem audio = named("Audio clips");
        for (const project::PlaylistTrack& track : project.playlist.tracks) {
            if (holdsAudio(track)) {
                audio.spec.audioTracks.push_back(track.id);
            }
        }
        if (!audio.spec.audioTracks.empty()) {
            stems.push_back(std::move(audio));
        }
        break;
    }
    }
    // Each stem is a file named after it, so two with one name - a channel and the
    // playlist track a v1 file made beside it - would overwrite each other.
    std::vector<std::string> seen;
    for (Stem& stem : stems) {
        const std::string base = stem.name;
        for (int n = 2; std::ranges::find(seen, stem.name) != seen.end(); ++n) {
            stem.name = base + " " + std::to_string(n);
        }
        seen.push_back(stem.name);
    }
    return stems;
}

} // namespace adx::render

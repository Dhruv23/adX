// adx-thread: main
#include "engine/instruments/voice/VoiceSetup.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

#include "engine/dsp/Math.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/instruments/voice/VoiceRenderCache.h"
#include "engine/instruments/voice/WorldAnalysis.h"
#include "engine/project/EventCompile.h"
#include "engine/project/Project.h"
#include "engine/project/TypeCatalog.h"

namespace adx::instruments {

struct VoicePins {
    std::vector<std::shared_ptr<const VoiceRenderCache::Entry>> entries;
    /// What it was built from: every render key, in note order.
    std::vector<std::uint64_t> keys;
};

void destroyVoicePins(VoicePins* pins) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - the node's opaque owner.
    delete pins;
}

namespace {

/// Notes closer than this are joined: the next one is sung out of this one.
constexpr double kLegatoMs = 10.0;
constexpr double kDefaultFadeMs = 30.0;
/// What a note without a lyric sings.
constexpr std::string_view kDefaultLyric = "あ";

std::mutex& bankMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::filesystem::path, std::shared_ptr<const Voicebank>>& banks() {
    static std::map<std::filesystem::path, std::shared_ptr<const Voicebank>> loaded;
    return loaded;
}

double paramOf(const project::Channel& channel, VoiceParam param) {
    const project::ParamDescriptor& descriptor = kVoiceParams[static_cast<std::size_t>(param)];
    if (const project::ParamValue* value = channel.instrument.find(descriptor.name)) {
        return value->value;
    }
    return descriptor.defaultValue;
}

std::filesystem::path bankPath(const project::Channel& channel,
                               const project::Resources* resources) {
    if (channel.instrument.voicebank.empty() || resources == nullptr) {
        return {};
    }
    std::u8string u8;
    for (const char c : channel.instrument.voicebank) {
        u8.push_back(static_cast<char8_t>(c));
    }
    std::filesystem::path relative{u8};
    if (relative.is_absolute()) {
        return relative;
    }
    return std::filesystem::path(resources->baseDirectory) / relative;
}

/// The first tick at which `pattern` is placed, or 0 when it is not.
core::Ticks placementOf(const project::Project& project, core::PatternId pattern) {
    std::optional<core::Ticks> first;
    for (const project::PlaylistTrack& track : project.playlist.tracks) {
        for (const project::PlaylistItem& item : track.items) {
            const auto* ref = std::get_if<project::PatternRef>(&item.content);
            if (ref != nullptr && ref->pattern == pattern && (!first || item.start < *first)) {
                first = item.start;
            }
        }
    }
    return first.value_or(core::Ticks{0});
}

struct PlannedNote {
    std::uint32_t noteId;
    NoteRenderRequest request;
};

std::vector<PlannedNote> plan(const project::Channel& channel, const project::Project& project,
                              const std::shared_ptr<const Voicebank>& bank) {
    std::vector<PlannedNote> planned;
    const core::TempoMap& tempo = project.tempo;
    const double vibratoDepth = paramOf(channel, VoiceParam::VibratoDepth);
    const double vibratoRate = paramOf(channel, VoiceParam::VibratoRate);
    const double vibratoDelay = paramOf(channel, VoiceParam::VibratoDelay);
    for (const project::Pattern& pattern : project.patterns) {
        const project::NoteClip* clip = pattern.clipFor(channel.id);
        if (clip == nullptr) {
            continue;
        }
        const core::Ticks at = placementOf(project, pattern.id);
        std::vector<const project::Note*> notes;
        notes.reserve(clip->notes.size());
        for (const project::Note& note : clip->notes) {
            notes.push_back(&note);
        }
        std::ranges::sort(notes, [](const project::Note* a, const project::Note* b) {
            return project::noteOrderBefore(*a, *b);
        });
        const auto msAt = [&](core::Ticks tick) { return 1000.0 * tempo.secondsAt(at + tick); };
        const auto lyricOf = [clip](const project::Note& note) {
            const project::NoteExtras* extras = clip->extrasFor(note.id);
            return extras != nullptr && !extras->lyric.empty() ? extras->lyric
                                                               : std::string(kDefaultLyric);
        };
        // Resolve every alias first: a note's cut depends on the next one's timings.
        std::vector<const OtoEntry*> aliases(notes.size(), nullptr);
        for (std::size_t n = 0; n < notes.size(); ++n) {
            const bool joined =
                n > 0 && msAt(notes[n]->start) - msAt(notes[n - 1]->end()) < kLegatoMs;
            aliases[n] = resolveAlias(*bank, lyricOf(*notes[n]),
                                      joined ? lyricOf(*notes[n - 1]) : std::string{});
        }
        for (std::size_t n = 0; n < notes.size(); ++n) {
            if (aliases[n] == nullptr) {
                continue; // a lyric the bank cannot sing: silent
            }
            const project::Note& note = *notes[n];
            NoteRenderRequest request;
            request.bank = bank;
            request.oto = *aliases[n];
            request.noteHz = dsp::midiToHz(static_cast<float>(note.pitch) +
                                           (static_cast<float>(note.fineTuneCents) * 0.01F));
            request.lengthMs = std::max(msAt(note.end()) - msAt(note.start), 10.0);
            request.cutMs = request.lengthMs;
            request.fadeOutMs = kDefaultFadeMs;
            if (n + 1 < notes.size() && aliases[n + 1] != nullptr &&
                msAt(notes[n + 1]->start) - msAt(note.end()) < kLegatoMs) {
                // Sung into the next note: stop where its overlap ends, fading across it.
                const OtoEntry& next = *aliases[n + 1];
                const double nextStart = msAt(notes[n + 1]->start) - msAt(note.start);
                request.cutMs =
                    std::max(nextStart - next.preutterance + std::max(next.overlap, 5.0),
                             request.lengthMs * 0.25);
                request.fadeOutMs = std::max(next.overlap, 5.0);
            }
            // Pitch: the note's slide and curve, then vibrato after its delay, per 5 ms.
            const project::NoteExtras* extras = clip->extrasFor(note.id);
            const std::vector<project::Knot> chain = extras != nullptr
                                                         ? project::pitchOffsetChain(*extras)
                                                         : std::vector<project::Knot>{};
            const auto frames = static_cast<std::size_t>(request.cutMs / kWorldFramePeriodMs) + 2;
            request.cents.assign(frames, 0.0F);
            const double ticksPerMs =
                static_cast<double>(note.length.value) / std::max(request.lengthMs, 1.0);
            for (std::size_t f = 0; f < frames; ++f) {
                const double ms = static_cast<double>(f) * kWorldFramePeriodMs;
                float cents = 0.0F;
                if (!chain.empty()) {
                    cents += project::knotValueAt(chain.data(), chain.size(),
                                                  static_cast<std::int64_t>(ms * ticksPerMs));
                }
                const double sinceDelay = (ms / 1000.0) - vibratoDelay;
                if (vibratoDepth > 0.0 && sinceDelay > 0.0) {
                    // Faded in over 100 ms so the vibrato does not start with a jolt.
                    const double fade = std::min(sinceDelay / 0.1, 1.0);
                    cents += static_cast<float>(vibratoDepth * fade *
                                                dsp::sinTurns(vibratoRate * sinceDelay));
                }
                request.cents[f] = cents;
            }
            request.gender = paramOf(channel, VoiceParam::Gender);
            request.breathiness = paramOf(channel, VoiceParam::Breathiness);
            request.tuningCents = paramOf(channel, VoiceParam::Tuning);
            request.peakCompression = paramOf(channel, VoiceParam::PeakCompression);
            planned.push_back(PlannedNote{.noteId = note.id.value, .request = std::move(request)});
        }
    }
    std::ranges::stable_sort(planned, {}, &PlannedNote::noteId);
    // One render per id: the first pattern that has the note wins.
    const auto [first, last] = std::ranges::unique(planned, {}, &PlannedNote::noteId);
    planned.erase(first, last);
    return planned;
}

} // namespace

std::shared_ptr<const Voicebank> loadVoicebank(const std::filesystem::path& path,
                                               std::vector<std::string>* warnings) {
    const std::scoped_lock lock(bankMutex());
    if (const auto found = banks().find(path); found != banks().end()) {
        return found->second;
    }
    auto bank = std::make_shared<Voicebank>();
    std::vector<std::string> local;
    if (!bank->load(path, warnings != nullptr ? *warnings : local)) {
        return nullptr;
    }
    std::shared_ptr<const Voicebank> shared = std::move(bank);
    banks().emplace(path, shared);
    return shared;
}

void configureVoice(VoiceInstrument& node, const project::Channel& channel,
                    const project::Project* project, const project::Resources* resources) {
    auto pins = std::make_unique<VoicePins>();
    rt::OwnedArray<VoiceNoteRef> table;
    const std::filesystem::path path = bankPath(channel, resources);
    const std::shared_ptr<const Voicebank> bank = path.empty() ? nullptr : loadVoicebank(path);
    if (project != nullptr && bank != nullptr) {
        const std::vector<PlannedNote> planned = plan(channel, *project, bank);
        table.allocate(planned.size());
        const std::span<VoiceNoteRef> refs = table.view();
        for (std::size_t n = 0; n < planned.size(); ++n) {
            std::shared_ptr<const VoiceRenderCache::Entry> entry =
                VoiceRenderCache::global().request(planned[n].request);
            refs[n] = VoiceNoteRef{.noteId = planned[n].noteId, .clip = &entry->clip};
            pins->keys.push_back(entry->key);
            pins->entries.push_back(std::move(entry));
        }
    }
    node.setNotes(std::move(table), pins.release());
}

std::vector<std::string> resolvedAliases(const project::Project& project,
                                         const project::Channel& channel,
                                         const project::NoteClip& clip) {
    std::vector<std::string> out(clip.notes.size());
    const std::filesystem::path path = bankPath(channel, &project.resources);
    const std::shared_ptr<const Voicebank> bank = path.empty() ? nullptr : loadVoicebank(path);
    if (bank == nullptr) {
        return out;
    }
    const project::Pattern* owner = nullptr;
    for (const project::Pattern& pattern : project.patterns) {
        if (pattern.clipFor(channel.id) == &clip) {
            owner = &pattern;
        }
    }
    const core::Ticks at = owner == nullptr ? core::Ticks{0} : placementOf(project, owner->id);
    const auto msAt = [&](core::Ticks tick) { return 1000.0 * project.tempo.secondsAt(at + tick); };
    const auto lyricOf = [&clip](const project::Note& note) {
        const project::NoteExtras* extras = clip.extrasFor(note.id);
        return extras != nullptr && !extras->lyric.empty() ? extras->lyric
                                                           : std::string(kDefaultLyric);
    };
    std::vector<std::size_t> order(clip.notes.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&clip](std::size_t a, std::size_t b) {
        return project::noteOrderBefore(clip.notes[a], clip.notes[b]);
    });
    for (std::size_t n = 0; n < order.size(); ++n) {
        const project::Note& note = clip.notes[order[n]];
        const project::Note* previous = n > 0 ? &clip.notes[order[n - 1]] : nullptr;
        const bool joined =
            previous != nullptr && msAt(note.start) - msAt(previous->end()) < kLegatoMs;
        const OtoEntry* entry =
            resolveAlias(*bank, lyricOf(note), joined ? lyricOf(*previous) : std::string{});
        if (entry != nullptr) {
            out[order[n]] = entry->alias;
        }
    }
    return out;
}

bool voiceMatches(const VoiceInstrument& node, const project::Channel& channel,
                  const project::Project* project, const project::Resources* resources) {
    const VoicePins* pins = node.pins();
    if (pins == nullptr) {
        return false;
    }
    std::vector<std::uint64_t> keys;
    const std::filesystem::path path = bankPath(channel, resources);
    const std::shared_ptr<const Voicebank> bank = path.empty() ? nullptr : loadVoicebank(path);
    if (project != nullptr && bank != nullptr) {
        for (const PlannedNote& note : plan(channel, *project, bank)) {
            keys.push_back(renderKey(note.request));
        }
    }
    return keys == pins->keys;
}

} // namespace adx::instruments

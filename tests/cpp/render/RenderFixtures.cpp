#include "tests/cpp/render/RenderFixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

#include "engine/audio/NullBackend.h"
#include "engine/format/adx/Parser.h"
#include "engine/render/RenderEngine.h"
#include "engine/rt/Violation.h"

namespace adx::tests {

std::unique_ptr<Loaded> loadText(std::string_view text) {
    auto loaded = std::make_unique<Loaded>();
    format::load(text, loaded->project, loaded->stack, loaded->diagnostics);
    std::string report;
    for (const format::Diagnostic& diagnostic : loaded->diagnostics.all()) {
        report += diagnostic.codeString() + " line " + std::to_string(diagnostic.span.line) + ": " +
                  diagnostic.message + '\n';
    }
    INFO(report);
    REQUIRE_FALSE(loaded->diagnostics.hasErrors());
    return loaded;
}

namespace {

/// xorshift32: the same generator the archived pattern compiler used, for the same
/// reason - a fixture must be the same project on every machine, forever.
struct XorShift {
    std::uint32_t state;

    std::uint32_t next() noexcept {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    }
};

} // namespace

project::Project syntheticProject(const SyntheticSpec& spec) {
    project::Project out;
    out.tempo.setTempo(core::Ticks{0}, spec.bpm, false);

    project::Insert master;
    master.id = out.newInsertId();
    master.name = "Master";
    out.mixer.inserts.push_back(master);
    out.mixer.master = master.id;

    std::vector<project::Pattern> patterns;
    for (std::uint32_t c = 0; c < spec.channels; ++c) {
        project::Insert bus;
        bus.id = out.newInsertId();
        bus.name = "Bus" + std::to_string(c);
        out.mixer.inserts.push_back(bus);
        out.mixer.routes.push_back(
            project::Route{.id = out.newRouteId(), .from = bus.id, .to = master.id});

        project::Channel channel;
        channel.id = out.newChannelId();
        channel.name = "Ch" + std::to_string(c);
        channel.output = bus.id;
        channel.maxPolyphony = 8;
        out.channels.push_back(channel);

        project::Pattern pattern;
        pattern.id = out.newPatternId();
        pattern.name = "P" + std::to_string(c);
        pattern.length = spec.length;
        pattern.noteClips.push_back(project::NoteClip{.channel = channel.id, .notes = {}});
        patterns.push_back(std::move(pattern));
    }

    XorShift random{spec.seed == 0 ? 1U : spec.seed};
    for (std::uint32_t n = 0; n < spec.notes; ++n) {
        project::Pattern& pattern = patterns[n % spec.channels];
        project::Note note;
        note.id = out.newNoteId();
        note.start = core::Ticks{static_cast<std::int64_t>(
            random.next() % static_cast<std::uint64_t>(spec.length.value))};
        note.length =
            core::Ticks{1 + static_cast<std::int64_t>(
                                random.next() % static_cast<std::uint64_t>(spec.maxNoteTicks))};
        note.pitch = static_cast<std::uint8_t>(36 + (random.next() % 48));
        note.velocity = static_cast<std::uint8_t>(40 + (random.next() % 88));
        pattern.noteClips.front().notes.push_back(note);
    }

    for (std::uint32_t c = 0; c < spec.channels; ++c) {
        project::PlaylistTrack track;
        track.id = out.newPlaylistTrackId();
        track.name = "T" + std::to_string(c);
        track.items.push_back(project::PlaylistItem{.id = out.newItemId(),
                                                    .start = core::Ticks{0},
                                                    .length = core::Ticks{0},
                                                    .sourceOffset = core::Ticks{0},
                                                    .content = project::PatternRef{patterns[c].id},
                                                    .muted = false});
        out.playlist.tracks.push_back(std::move(track));
    }
    out.patterns = std::move(patterns);
    return out;
}

project::Project toneProject(const std::vector<NoteSpec>& notes, double bpm,
                             std::uint16_t polyphony) {
    project::Project out;
    out.tempo.setTempo(core::Ticks{0}, bpm, false);

    project::Insert master;
    master.id = out.newInsertId();
    master.name = "Master";
    out.mixer.inserts.push_back(master);
    out.mixer.master = master.id;

    project::Channel channel;
    channel.id = out.newChannelId();
    channel.name = "Tone";
    channel.output = master.id;
    channel.maxPolyphony = polyphony;
    out.channels.push_back(channel);

    project::Pattern pattern;
    pattern.id = out.newPatternId();
    pattern.name = "Notes";
    project::NoteClip clip{.channel = channel.id, .notes = {}};
    std::int64_t end = core::kPpq;
    for (const NoteSpec& spec : notes) {
        project::Note note;
        note.id = out.newNoteId();
        note.start = core::Ticks{spec.start};
        note.length = core::Ticks{spec.length};
        note.pitch = spec.pitch;
        note.velocity = spec.velocity;
        clip.notes.push_back(note);
        end = std::max(end, spec.start + spec.length);
    }
    pattern.length = core::Ticks{end};
    pattern.noteClips.push_back(std::move(clip));

    project::PlaylistTrack track;
    track.id = out.newPlaylistTrackId();
    track.name = "T";
    track.items.push_back(project::PlaylistItem{.id = out.newItemId(),
                                                .start = core::Ticks{0},
                                                .length = core::Ticks{0},
                                                .sourceOffset = core::Ticks{0},
                                                .content = project::PatternRef{pattern.id},
                                                .muted = false});
    out.patterns.push_back(std::move(pattern));
    out.playlist.tracks.push_back(std::move(track));
    return out;
}

RealtimeCapture captureRealtime(const project::Project& project, std::uint32_t blockFrames,
                                std::uint64_t frames, std::uint32_t sampleRate) {
    RealtimeCapture result;
    result.samples.assign(static_cast<std::size_t>(frames) * 2, 0.0F);

    auto backend = std::make_unique<audio::NullBackend>();
    audio::NullBackend& null = *backend;
    render::RenderEngine engine{std::move(backend),
                                render::EngineOptions{.sampleRate = sampleRate,
                                                      .blockFrames = blockFrames,
                                                      .outputChannels = 2}};
    REQUIRE(engine.open().code == audio::Error::Code::None);
    null.setCapture(result.samples);

    // The same messages, in the same order, that renderOffline posts - so both paths
    // start from the same state on the same first block.
    REQUIRE(engine.setProject(project, 0).rebuilt);
    engine.setLoop(transport::LoopRegion{});
    engine.seek(core::Ticks{0});
    engine.play();

    const std::uint64_t before = rt::ViolationLog::instance().count();
    REQUIRE(engine.start().code == audio::Error::Code::None);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (null.capturedFrames() < frames && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        engine.pump();
    }
    engine.stop();
    REQUIRE(null.capturedFrames() >= frames);

    result.violations = rt::ViolationLog::instance().count() - before;
    result.arenaHighWater = engine.audio().arenaHighWaterMark();
    result.arenaCapacity = engine.audio().callbacks().arenaCapacity();
    result.worstCallbackNs = engine.audio().callbacks().worstCallbackNs();
    return result;
}

std::vector<float> renderFrames(const project::Project& project, std::uint32_t blockFrames,
                                std::uint64_t frames, render::RenderStats& stats,
                                std::uint32_t sampleRate) {
    render::OfflineRenderOptions options;
    options.sampleRate = sampleRate;
    options.blockFrames = blockFrames;
    options.frames = frames;
    std::vector<float> out = render::renderOffline(project, options, stats);
    INFO(stats.error);
    REQUIRE(stats.error.empty());
    return out;
}

OfflineRig::OfflineRig(std::uint32_t blockFrames, std::uint32_t sampleRate) {
    auto owned = std::make_unique<audio::OfflineBackend>();
    backend = owned.get();
    engine = std::make_unique<render::RenderEngine>(
        std::move(owned), render::EngineOptions{.sampleRate = sampleRate,
                                                .blockFrames = blockFrames,
                                                .outputChannels = 2});
    REQUIRE(engine->open().code == audio::Error::Code::None);
    REQUIRE(engine->start().code == audio::Error::Code::None);
}

void OfflineRig::render(std::uint64_t frames, std::vector<float>& out) const {
    const std::span<const float> rendered = backend->renderFrames(frames);
    out.insert(out.end(), rendered.begin(), rendered.end());
    engine->pump();
}

std::vector<float> OfflineRig::render(std::uint64_t frames) const {
    std::vector<float> out;
    render(frames, out);
    return out;
}

std::size_t firstSoundFrom(const std::vector<float>& samples, std::size_t from,
                           std::uint32_t channels) {
    const std::size_t frames = samples.size() / channels;
    for (std::size_t f = from; f < frames; ++f) {
        if (samples[f * channels] != 0.0F) {
            return f;
        }
    }
    return frames;
}

} // namespace adx::tests

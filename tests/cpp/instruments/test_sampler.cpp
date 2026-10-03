// The Sampler (phase_4.md §4.5, §6): loop modes, round robin, velocity layers, the
// root-key fast path, and playing silence - not blocking - while a sample loads.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/format/audio/SamplePool.h"
#include "engine/instruments/Factory.h"
#include "engine/instruments/sampler/SamplerInstrument.h"
#include "engine/project/Project.h"
#include "engine/render/OfflineRender.h"
#include "engine/render/RenderHash.h"
#include "tests/cpp/instruments/InstrumentHarness.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace fs = std::filesystem;
using adx::project::LoopMode;
using adx::project::SampleZone;
using adx::tests::InstrumentRun;
using adx::tests::noteOff;
using adx::tests::noteOn;

namespace {

fs::path scratch() {
    const fs::path dir = fs::temp_directory_path() / "adx_sampler_tests";
    fs::create_directories(dir);
    return dir;
}

/// Writes a mono 48 kHz float WAV of `samples`.
fs::path writeMono(const std::string& name, const std::vector<float>& samples) {
    const fs::path path = scratch() / name;
    REQUIRE(adx::render::writeWavFloat32(path.string(), samples, 1, 48000));
    return path;
}

/// A sampler channel on a fresh project, its zones pointing at `files` (one each).
struct Rig {
    adx::project::Project project;
    adx::format::SamplePool pool{1};
    std::shared_ptr<adx::graph::ChannelNode> node;

    Rig(const std::vector<fs::path>& files, std::vector<SampleZone> zones) {
        project.resources.baseDirectory = scratch().string();
        adx::project::Channel channel;
        channel.id = project.newChannelId();
        channel.name = "S";
        channel.instrument.type = "sampler";
        for (std::size_t z = 0; z < zones.size(); ++z) {
            adx::project::SampleRef ref;
            ref.id = project.newSampleId();
            ref.path = files[z].filename().string();
            project.resources.samples.push_back(ref);
            zones[z].sample = ref.id;
        }
        channel.instrument.zones = std::move(zones);
        project.channels.push_back(channel);
        node = adx::instruments::makeInstrument(
            project.channels.front(),
            adx::instruments::InstrumentContext{.resources = &project.resources, .pool = &pool});
        node->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
        pool.waitAll();
    }

    /// Sampler parameters with the envelope out of the way: instant attack, full
    /// sustain, velocity ignored - so the output is the sample, scaled by nothing.
    static std::vector<float> flatParams(bool oneShot = false) {
        std::vector<float> params = adx::tests::instrumentParams("sampler");
        params[adx::tests::instrumentParamIndex("sampler", "env.attack")] = 0.0F;
        params[adx::tests::instrumentParamIndex("sampler", "env.sustain")] = 1.0F;
        params[adx::tests::instrumentParamIndex("sampler", "velocity")] = 0.0F;
        params[adx::tests::instrumentParamIndex("sampler", "oneShot")] = oneShot ? 1.0F : 0.0F;
        return params;
    }
};

/// 0, 1, 2, ... scaled to stay small: frame k of the sample holds k / 1000, so the
/// output names the frame it came from.
std::vector<float> ramp(std::size_t length) {
    std::vector<float> out(length);
    for (std::size_t k = 0; k < length; ++k) {
        out[k] = static_cast<float>(k) / 1000.0F;
    }
    return out;
}

/// Which source frame each output frame played, read back from a ramp.
std::vector<int> framesPlayed(const InstrumentRun& run, std::size_t count) {
    std::vector<int> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<int>(std::lround(run.left[i] * 1000.0F)));
    }
    return out;
}

} // namespace

TEST_CASE("sampler_root_key_fast_path", "[instruments][sampler]") {
    // phase_4.md §6: at ratio 1.0 the output is bit-identical to the source samples - a
    // copy, not an interpolation that happens to be close.
    std::vector<float> source(4800);
    std::uint32_t state = 0x1234567U;
    for (float& s : source) {
        state = (state * 1664525U) + 1013904223U;
        s = (static_cast<float>(state >> 8U) / 16777216.0F) - 0.5F;
    }
    SampleZone zone;
    zone.rootKey = 60;
    Rig const rig({writeMono("fast.wav", source)}, {zone});
    const InstrumentRun run =
        adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 60, 127)}, 4800, Rig::flatParams());
    for (std::size_t i = 0; i < source.size(); ++i) {
        REQUIRE(run.left[i] == source[i]);
        REQUIRE(run.right[i] == source[i]);
    }

    // An octave up reads every other frame - still whole frames, so still exact.
    const InstrumentRun octave =
        adx::tests::runInstrument(*rig.node, {noteOn(0, 2, 72, 127)}, 2000, Rig::flatParams());
    for (std::size_t i = 0; i < 2000; ++i) {
        REQUIRE(octave.left[i] == source[2 * i]);
    }
}

TEST_CASE("sampler_loop_modes", "[instruments][sampler]") {
    // Each LoopMode against a ramp, so the sequence of source frames played can be
    // written down by hand. The loop is [4, 8).
    const fs::path file = writeMono("ramp.wav", ramp(12));
    const auto zoneWith = [](LoopMode mode) {
        SampleZone zone;
        zone.rootKey = 60;
        zone.loop = mode;
        zone.loopStart = 4;
        zone.loopEnd = 8;
        return zone;
    };

    SECTION("off: once through, then silence") {
        Rig const rig({file}, {zoneWith(LoopMode::Off)});
        const auto run =
            adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 60, 127)}, 16, Rig::flatParams());
        CHECK(framesPlayed(run, 16) ==
              std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0, 0, 0});
    }
    SECTION("forward: [4, 8) for as long as the voice lasts") {
        Rig const rig({file}, {zoneWith(LoopMode::Forward)});
        const auto run =
            adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 60, 127)}, 16, Rig::flatParams());
        CHECK(framesPlayed(run, 16) ==
              std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 4, 5, 6, 7, 4, 5, 6, 7});
    }
    SECTION("pingpong: forward, then back, endpoints once") {
        Rig const rig({file}, {zoneWith(LoopMode::PingPong)});
        const auto run =
            adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 60, 127)}, 16, Rig::flatParams());
        CHECK(framesPlayed(run, 16) ==
              std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4, 5, 6, 7, 6, 5});
    }
    SECTION("sustain: loops while held, plays out to the end after release") {
        Rig const rig({file}, {zoneWith(LoopMode::Sustain)});
        // Released at frame 10 (it is playing 6 then); a one-shot, so the envelope does
        // not fade it and the sequence is the sample's own.
        const auto run = adx::tests::runInstrument(
            *rig.node, {noteOn(0, 1, 60, 127), noteOff(10, 1, 60)}, 18, Rig::flatParams(true), 1);
        CHECK(framesPlayed(run, 18) ==
              std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0});
    }
    SECTION("release: plays through while held, the note-off jumps to the tail") {
        Rig const rig({file}, {zoneWith(LoopMode::Release)});
        const auto run = adx::tests::runInstrument(
            *rig.node, {noteOn(0, 1, 60, 127), noteOff(3, 1, 60)}, 10, Rig::flatParams(true), 1);
        CHECK(framesPlayed(run, 10) == std::vector<int>{0, 1, 2, 8, 9, 10, 11, 0, 0, 0});
    }
}

TEST_CASE("sampler_velocity_layers", "[instruments][sampler]") {
    SampleZone soft;
    soft.rootKey = 60;
    soft.keyLow = 60;
    soft.keyHigh = 60;
    soft.velocityHigh = 63;
    SampleZone hard = soft;
    hard.velocityLow = 64;
    hard.velocityHigh = 127;
    Rig const rig({writeMono("soft.wav", std::vector<float>(64, 0.25F)),
                   writeMono("hard.wav", std::vector<float>(64, 0.75F))},
                  {soft, hard});
    // A reset between runs: each run's note is still held when it ends.
    const auto low =
        adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 60, 40)}, 32, Rig::flatParams());
    rig.node->reset();
    const auto high =
        adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 60, 100)}, 32, Rig::flatParams());
    rig.node->reset();
    CHECK(low.left[10] == 0.25F);
    CHECK(high.left[10] == 0.75F);
    // No zone for this key: the note is silent, and its voice is freed at once.
    const auto none =
        adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 30, 100)}, 64, Rig::flatParams());
    CHECK(none.left[10] == 0.0F);
    CHECK(none.sounding.back() == 0);
}

TEST_CASE("sampler_roundrobin_deterministic", "[instruments][sampler][render]") {
    // Zones in a group take turns, and the turn is part of the render: offline and
    // realtime choose the same zones, sample for sample (phase_4.md §6).
    const fs::path a = writeMono("rr_a.wav", std::vector<float>(2400, 0.25F));
    const fs::path b = writeMono("rr_b.wav", std::vector<float>(2400, 0.5F));

    // At node level first: four hits alternate a, b, a, b.
    SampleZone first;
    first.rootKey = 60;
    first.roundRobinGroup = 1;
    first.roundRobinIndex = 0;
    SampleZone second = first;
    second.roundRobinIndex = 1;
    Rig const rig({a, b}, {first, second});
    const auto run = adx::tests::runInstrument(*rig.node,
                                               {noteOn(0, 1, 60, 127), noteOff(100, 1, 60),
                                                noteOn(2400, 2, 60, 127), noteOff(2500, 2, 60),
                                                noteOn(4800, 3, 60, 127), noteOff(4900, 3, 60),
                                                noteOn(7200, 4, 60, 127), noteOff(7300, 4, 60)},
                                               9600, Rig::flatParams());
    CHECK(run.left[50] == 0.25F);
    CHECK(run.left[2450] == 0.5F);
    CHECK(run.left[4850] == 0.25F);
    CHECK(run.left[7250] == 0.5F);

    // And through the whole engine, offline against realtime.
    adx::project::Project project =
        adx::tests::toneProject({{.start = 0, .length = 240, .pitch = 60, .velocity = 127},
                                 {.start = 480, .length = 240, .pitch = 60, .velocity = 127},
                                 {.start = 960, .length = 240, .pitch = 60, .velocity = 127},
                                 {.start = 1440, .length = 240, .pitch = 60, .velocity = 127}});
    project.resources.baseDirectory = scratch().string();
    adx::project::Channel& channel = project.channels.front();
    channel.instrument.type = "sampler";
    for (const fs::path& file : {a, b}) {
        adx::project::SampleRef ref;
        ref.id = project.newSampleId();
        ref.path = file.filename().string();
        project.resources.samples.push_back(ref);
    }
    first.sample = project.resources.samples[0].id;
    second.sample = project.resources.samples[1].id;
    channel.instrument.zones = {first, second};

    // Warm the global pool, so the realtime engine finds its samples already decoded -
    // as a session that loaded the project a moment ago would.
    const auto warmA = adx::format::SamplePool::global().request(a);
    const auto warmB = adx::format::SamplePool::global().request(b);
    adx::format::SamplePool::global().waitAll();

    adx::render::RenderStats stats;
    const std::vector<float> offline = adx::tests::renderFrames(project, 256, 48000, stats);
    const adx::tests::RealtimeCapture realtime = adx::tests::captureRealtime(project, 256, 48000);
    CHECK(adx::render::hashSamples(offline) == adx::render::hashSamples(realtime.samples));
    // Not trivially: the hits are there, alternating.
    CHECK(offline[std::size_t{2} * 100] != 0.0F);
}

TEST_CASE("sampler_plays_silence_while_loading", "[instruments][sampler]") {
    // A sample that is still decoding: the note is silent and the call returns at once;
    // once the sample is ready, the next note sounds. Built by hand, so "loading" is a
    // state the test controls rather than a race it hopes to win.
    std::vector<float> data(64, 0.5F);
    adx::format::SampleHandle handle;
    adx::instruments::SamplerInstrument node{1, 8, adx::project::VoiceStealMode::OldestReleased};
    adx::rt::OwnedArray<adx::instruments::SamplerZone> zones;
    zones.allocate(1);
    zones.view()[0] = adx::instruments::SamplerZone{
        .sample = &handle, .zone = SampleZone{}, .gainLeft = 1.0F, .gainRight = 1.0F};
    node.setZones(std::move(zones), nullptr);
    node.prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});

    const auto loading =
        adx::tests::runInstrument(node, {noteOn(0, 1, 60, 127)}, 32, Rig::flatParams());
    for (const float s : loading.left) {
        REQUIRE(s == 0.0F);
    }
    CHECK(loading.violations == 0);

    node.reset();
    handle.publish(adx::format::SampleView{
        .left = data.data(), .right = data.data(), .frames = 64, .sampleRate = 48000});
    const auto ready =
        adx::tests::runInstrument(node, {noteOn(0, 2, 60, 127)}, 32, Rig::flatParams());
    CHECK(ready.left[5] == 0.5F);
}

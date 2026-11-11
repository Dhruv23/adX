// The Sampler (phase_4.md §4.5, §6): loop modes, round robin, velocity layers, the
// root-key fast path, and playing silence - not blocking - while a sample loads.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/format/adx/Writer.h"
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

// --- phase_12.md §4.7.1: keyswitches, release zones, dynamic crossfades, chokes ---

namespace {

/// A constant: the output names which zone played.
std::vector<float> level(float value, std::size_t length = 9600) {
    return std::vector<float>(length, value);
}

float at(const InstrumentRun& run, std::size_t frame) {
    return run.left[frame];
}

} // namespace

TEST_CASE("sampler_keyswitch", "[instruments][sampler]") {
    // Two articulations on the same keys, selected by keys 24 and 25. The last switch
    // pressed selects the group; the switch keys themselves are silent.
    SampleZone sustain;
    sustain.switchLow = 24;
    sustain.switchHigh = 25;
    sustain.switchKey = 24;
    SampleZone staccato = sustain;
    staccato.switchKey = 25;
    Rig rig({writeMono("ks_sus.wav", level(0.25F)), writeMono("ks_stac.wav", level(0.5F))},
            {sustain, staccato});
    const InstrumentRun run = adx::tests::runInstrument(
        *rig.node,
        {noteOn(0, 1, 60), noteOff(1000, 1, 60),    // before any switch: the lowest key's
         noteOn(2000, 2, 25), noteOff(2100, 2, 25), // switch to staccato: silent
         noteOn(3000, 3, 60), noteOff(4000, 3, 60), // staccato
         noteOn(5000, 4, 24), noteOff(5100, 4, 24), // back
         noteOn(6000, 5, 60), noteOff(7000, 5, 60)},
        8000, Rig::flatParams(true));
    CHECK(at(run, 500) == 0.25F);
    CHECK(at(run, 2050) == 0.25F); // only the first note's one-shot: the switch adds nothing
    CHECK(at(run, 3500) == 0.25F + 0.5F);
    CHECK(run.violations == 0);

    // With the first note over before the switch, the switch key is exactly silent.
    Rig quiet(
        {writeMono("ks_sus2.wav", level(0.25F, 500)), writeMono("ks_stac2.wav", level(0.5F, 500))},
        {sustain, staccato});
    const InstrumentRun second =
        adx::tests::runInstrument(*quiet.node,
                                  {noteOn(0, 1, 60), noteOn(2000, 2, 25), noteOn(3000, 3, 60),
                                   noteOn(5000, 4, 24), noteOn(6000, 5, 60)},
                                  8000, Rig::flatParams(true));
    CHECK(at(second, 100) == 0.25F);
    CHECK(at(second, 2100) == 0.0F);
    CHECK(at(second, 3100) == 0.5F);
    CHECK(at(second, 5100) == 0.0F);
    CHECK(at(second, 6100) == 0.25F);
}

TEST_CASE("sampler_release_trigger", "[instruments][sampler]") {
    // A release zone fires at the note-off, and is 6 dB quieter for each second the
    // note was held.
    SampleZone attack;
    SampleZone release;
    release.trigger = adx::project::ZoneTrigger::Release;
    release.rtDecay = 6.0F;
    Rig rig({writeMono("rt_attack.wav", level(0.25F, 96000)),
             writeMono("rt_release.wav", level(0.5F, 2400))},
            {attack, release});
    std::vector<float> params = Rig::flatParams();
    params[adx::tests::instrumentParamIndex("sampler", "env.release")] = 0.0F;
    const InstrumentRun run = adx::tests::runInstrument(
        *rig.node, {noteOn(0, 1, 60), noteOff(48000, 1, 60)}, 60000, params);
    CHECK(at(run, 24000) == 0.25F); // held: the attack zone alone
    // After the note-off: the release zone, one second's worth of decay down.
    const float expected = 0.5F * std::pow(10.0F, -6.0F / 20.0F);
    CHECK(std::abs(at(run, 48100) - expected) < 1e-4F);
    // It plays to its end whatever the envelope does, then the voice is done.
    CHECK(std::abs(at(run, 50300) - expected) < 1e-4F);
    CHECK(at(run, 50500) == 0.0F);
    CHECK(run.sounding.back() == 0);
    CHECK(run.violations == 0);
}

TEST_CASE("sampler_dynamic_crossfade", "[instruments][sampler]") {
    // Two layers crossfading across 40..90. By velocity the note's velocity fixes the
    // blend; by `dynamics` the blend moves while the note sustains.
    SampleZone soft;
    soft.xfadeOutLow = 40;
    soft.xfadeOutHigh = 90;
    SampleZone loud;
    loud.xfadeInLow = 40;
    loud.xfadeInHigh = 90;
    const std::vector<fs::path> files{writeMono("xf_soft.wav", level(0.25F, 96000)),
                                      writeMono("xf_loud.wav", level(0.5F, 96000))};
    const auto play = [&](std::uint8_t velocity) {
        Rig rig(files, {soft, loud});
        return at(adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 60, velocity)}, 2000,
                                            Rig::flatParams()),
                  1000);
    };
    CHECK(play(20) == 0.25F);
    CHECK(play(120) == 0.5F);
    // Halfway: equal power, each layer at cos(45 degrees).
    CHECK(std::abs(play(65) - (0.75F * std::sqrt(0.5F))) < 1e-4F);

    // By dynamics: a lane sweeps the parameter and the blend follows mid-note.
    Rig rig(files, {soft, loud});
    std::vector<float> params = Rig::flatParams();
    params[adx::tests::instrumentParamIndex("sampler", "xfade.source")] = 1.0F;
    params[adx::tests::instrumentParamIndex("sampler", "dynamics")] = 0.0F;
    const InstrumentRun low =
        adx::tests::runInstrument(*rig.node, {noteOn(0, 1, 60, 127)}, 2000, params);
    CHECK(at(low, 1000) == 0.25F);
    Rig other(files, {soft, loud});
    params[adx::tests::instrumentParamIndex("sampler", "dynamics")] = 1.0F;
    const InstrumentRun high =
        adx::tests::runInstrument(*other.node, {noteOn(0, 1, 60, 1)}, 2000, params);
    CHECK(at(high, 1000) == 0.5F);
}

TEST_CASE("sampler_choke_group", "[instruments][sampler]") {
    // The closed hat (group 1) cuts the open hat (off by 1) within 2 ms.
    SampleZone open;
    open.keyLow = open.keyHigh = 46;
    open.rootKey = 46;
    open.offBy = 1;
    SampleZone closed;
    closed.keyLow = closed.keyHigh = 42;
    closed.rootKey = 42;
    closed.group = 1;
    Rig rig({writeMono("hat_open.wav", level(0.25F, 48000)),
             writeMono("hat_closed.wav", level(0.5F, 4800))},
            {open, closed});
    const InstrumentRun run = adx::tests::runInstrument(
        *rig.node, {noteOn(0, 1, 46), noteOn(9600, 2, 42)}, 20000, Rig::flatParams(true));
    CHECK(at(run, 9000) == 0.25F);
    CHECK(at(run, 9600 + 200) == 0.5F); // 96 frames of fade, then the closed hat alone
    CHECK(at(run, 15000) == 0.0F);
    CHECK(run.violations == 0);
}

TEST_CASE("zone_extension_fields_roundtrip", "[instruments][sampler][format]") {
    const std::string text =
        "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL S]\nINSTRUMENT=sampler\nOUTPUT=insert.1\n"
        "ZONE \"a.wav\" root=60 sw=24..27:25 trigger=release rtdecay=6 xfin=40..90 "
        "xfout=100..120 group=3 offby=4\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    const auto loaded = adx::tests::loadText(text);
    const SampleZone& zone = loaded->project.channels.front().instrument.zones.front();
    CHECK(zone.switchLow == 24);
    CHECK(zone.switchHigh == 27);
    CHECK(zone.switchKey == 25);
    CHECK(zone.trigger == adx::project::ZoneTrigger::Release);
    CHECK(zone.rtDecay == 6.0F);
    CHECK(zone.xfadeInLow == 40);
    CHECK(zone.xfadeInHigh == 90);
    CHECK(zone.xfadeOutLow == 100);
    CHECK(zone.xfadeOutHigh == 120);
    CHECK(zone.group == 3);
    CHECK(zone.offBy == 4);
    const std::string written = adx::format::write(loaded->project);
    CHECK(adx::format::write(adx::tests::loadText(written)->project) == written);
}

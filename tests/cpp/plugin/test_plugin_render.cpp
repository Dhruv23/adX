// Hosted plugins in a render (phase_9.md §4.5, §4.7, §5): delay compensation, latency
// reports that lie or change, state that saves and comes back, instruments - in both
// formats where the format matters.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "engine/format/adx/Writer.h"
#include "engine/plugin/Bridge/AudioLink.h"
#include "engine/plugin/PluginHost.h"
#include "engine/plugin/StateBlob.h"
#include "engine/project/commands/PluginCommands.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/plugin/PluginFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

using namespace adx;
using namespace adx::plugin;
namespace fx = adx::tests::plugins;

/// Two copies of the tone, one through the hosted effect (latency `latency`, unity
/// gain), one dry, summed at the master. Either can be muted.
project::Project parallel(PluginFormat format, std::uint32_t latency, bool muteHosted,
                          bool muteDry) {
    project::Project project =
        tests::toneProject({{.start = 0, .length = 960, .pitch = 69, .velocity = 30},
                            {.start = 1200, .length = 480, .pitch = 76, .velocity = 30}});
    project::Channel dry = project.channels.front();
    fx::addFixtureEffect(project,
                         fx::EffectSetup{.format = format,
                                         .state = fx::fixtureState(1.0, latency, 0.0, format),
                                         .params = {},
                                         .mix = 1.0F});
    project.channels.front().muted = muteHosted;
    dry.id = project.newChannelId();
    dry.name = "Dry";
    dry.output = project.mixer.master;
    dry.muted = muteDry;
    project.channels.push_back(dry);
    project::NoteClip clip = project.patterns.front().noteClips.front();
    clip.channel = dry.id;
    project.patterns.front().noteClips.push_back(clip);
    return project;
}

std::size_t firstSound(const std::vector<float>& samples) {
    std::size_t frame = 0;
    while (frame * 2 < samples.size() && samples[frame * 2] == 0.0F) {
        ++frame;
    }
    return frame;
}

template<class Predicate> bool waitFor(Predicate predicate, double seconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

bridge::BlockResult sendParams(bridge::RemoteInstance& instance,
                               std::vector<bridge::WireParam> params) {
    std::vector<float> in(256, 0.0F);
    std::vector<float> outLeft(256);
    std::vector<float> outRight(256);
    const std::array<const float*, 2> inputs{in.data(), in.data()};
    const std::array<float*, 2> outputs{outLeft.data(), outRight.data()};
    std::vector<bridge::WireParam> echoes(bridge::kMaxOutParams);
    bridge::BlockRequest request;
    request.inputs = inputs;
    request.frames = 256;
    request.params = params;
    request.transport.tempo = 120.0;
    request.deadlineMicros = 2'000'000;
    std::uint32_t count = 0;
    return bridge::runBlock(instance.link(), request, outputs, echoes, count);
}

} // namespace

TEST_CASE("plugin_latency_compensated", "[plugin][pdc]") {
    fx::useTestHost();
    for (const PluginFormat format : {PluginFormat::Clap, PluginFormat::Vst3}) {
        INFO(toString(format));
        const std::vector<float> hosted =
            fx::renderPlugin(parallel(format, 100, false, true), 48000);
        const std::vector<float> dry = fx::renderPlugin(parallel(format, 100, true, false), 48000);
        REQUIRE(fx::peakOf(hosted) > 0.01F);
        // PDC delayed the dry path by the plugin's 100 samples: each path alone is the
        // other, sample for sample - alignment within less than one sample.
        CHECK(firstSound(hosted) == firstSound(dry));
        CHECK(hosted == dry);
        // And it was compensated rather than absent: the dry path really is late.
        const std::vector<float> noLatency =
            fx::renderPlugin(parallel(format, 0, true, false), 48000);
        CHECK(firstSound(dry) == firstSound(noLatency) + 100);
    }
}

TEST_CASE("plugin_wrong_latency_detected", "[plugin][pdc]") {
    fx::useTestHost();
    PluginHost& host = PluginHost::instance();
    static_cast<void>(host.takeNotices());
    std::string error;
    auto honest = host.create(fx::effectId(PluginFormat::Clap), nullptr, 48000, 512, false, error);
    REQUIRE(honest);
    // Latency 200, honestly reported: measured and reported agree.
    REQUIRE(sendParams(*honest, {{0, 3, 200.0, 0, 0}}) == bridge::BlockResult::Done);
    REQUIRE(waitFor([&] { return honest->latency() == 200; }, 3.0));
    CHECK(host.checkLatency(honest->id()) == 200);
    auto notices = host.takeNotices();
    CHECK(std::ranges::count(notices, bridge::Notice::Kind::LatencyMismatch,
                             &bridge::Notice::kind) == 0);

    // The same latency reported as zero (mode 4): the measurement catches it.
    auto liar = host.create(fx::effectId(PluginFormat::Clap), nullptr, 48000, 512, false, error);
    REQUIRE(liar);
    REQUIRE(sendParams(*liar, {{0, 2, 4.0, 0, 0}, {0, 3, 200.0, 0, 0}}) ==
            bridge::BlockResult::Done);
    // Its own notice: the honest one's may still be on its way.
    REQUIRE(waitFor(
        [&] {
            const auto n = host.takeNotices();
            notices.insert(notices.end(), n.begin(), n.end());
            return std::ranges::any_of(notices, [&](const bridge::Notice& notice) {
                return notice.kind == bridge::Notice::Kind::LatencyChanged &&
                       notice.instance == liar->id();
            });
        },
        3.0));
    CHECK(liar->latency() == 0);
    CHECK(host.checkLatency(liar->id()) == 200);
    notices = host.takeNotices();
    REQUIRE(std::ranges::count(notices, bridge::Notice::Kind::LatencyMismatch,
                               &bridge::Notice::kind) == 1);
    const auto mismatch =
        std::ranges::find(notices, bridge::Notice::Kind::LatencyMismatch, &bridge::Notice::kind);
    CHECK(mismatch->message.find("reports 0") != std::string::npos);
    CHECK(mismatch->message.find("measures 200") != std::string::npos);
}

TEST_CASE("plugin_latency_change_midplay", "[plugin][pdc]") {
    fx::useTestHost();
    PluginHost& host = PluginHost::instance();
    static_cast<void>(host.takeNotices());
    project::Project project = tests::toneProject(
        {{.start = 0, .length = std::int64_t{3840} * 4, .pitch = 69, .velocity = 100}});
    fx::addFixtureEffect(project, fx::EffectSetup{.format = PluginFormat::Clap,
                                                  .state = {},
                                                  .params = {{.name = "mode", .value = 5.0}},
                                                  .mix = 1.0F}); // +64 at block 100
    project::CommandStack const stack;
    tests::OfflineRig const rig(256);
    REQUIRE(rig.engine->setProject(project, 0).rebuilt);
    rig.engine->play();
    static_cast<void>(rig.render(std::uint64_t{256} * 120));
    // Wait on the instance, not on a notice: a notice from an earlier test's instance can
    // still be on its way, and on a slow machine it arrives first.
    std::vector<bridge::Notice> notices;
    const auto grow = [&] {
        const auto n = host.takeNotices();
        notices.insert(notices.end(), n.begin(), n.end());
    };
    const auto changed = [&]() -> std::shared_ptr<bridge::RemoteInstance> {
        for (const auto& instance : host.instances()) {
            if (instance->plugin().uid == "com.adx.test.effect" && instance->latency() == 64) {
                return instance;
            }
        }
        return nullptr;
    };
    std::shared_ptr<bridge::RemoteInstance> moved;
    REQUIRE(waitFor(
        [&] {
            grow();
            moved = changed();
            return moved != nullptr;
        },
        3.0));
    // Short renders after the change: the plugin moves again 100 blocks after every restart,
    // and a quick restart must not leave room for that.
    // One recompensation: the app answers the notice with a rebuild, and the plan now
    // holds the new latency. The notice is the log line.
    static_cast<void>(rig.render(std::uint64_t{256} * 20));
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    static_cast<void>(rig.render(std::uint64_t{256} * 20));
    const auto ours = [&] {
        grow();
        return std::ranges::count_if(notices, [&](const bridge::Notice& notice) {
            return notice.kind == bridge::Notice::Kind::LatencyChanged &&
                   notice.instance == moved->id();
        });
    };
    CHECK(waitFor([&] { return ours() >= 1; }, 3.0));
    CHECK(ours() == 1);
    // The rebuild may swap in a fresh instance, so the old one is checked directly.
    CHECK(moved->latency() == 64);
}

TEST_CASE("plugin_state_roundtrip", "[plugin][state]") {
    fx::useTestHost();
    for (const PluginFormat format : {PluginFormat::Clap, PluginFormat::Vst3}) {
        INFO(toString(format));
        // Saved in a project, written as text, read back: renders identically.
        project::Project project = tests::toneProject({{.start = 0, .length = 1920, .pitch = 64}});
        const core::SlotId slot = fx::addFixtureEffect(
            project, fx::EffectSetup{.format = format,
                                     .state = fx::fixtureState(0.375, 32, 0.625, format),
                                     .params = {},
                                     .mix = 1.0F});
        static_cast<void>(PluginHost::instance().takeNotices());
        const std::vector<float> first = fx::renderPlugin(project, 24000);
        const auto rejected = PluginHost::instance().takeNotices();
        CHECK(std::ranges::count(rejected, bridge::Notice::Kind::StateRejected,
                                 &bridge::Notice::kind) == 0);
        const auto reloaded = tests::loadText(format::write(project));
        const std::vector<float> second = fx::renderPlugin(reloaded->project, 24000);
        CHECK(fx::peakOf(first) > 0.01F);
        CHECK(first == second);

        // A live instance's state, taken and given to a fresh one: the same values.
        std::string error;
        PluginHost& host = PluginHost::instance();
        auto live = host.create(fx::effectId(format), nullptr, 48000, 256, false, error);
        REQUIRE(live);
        const double gain = format == PluginFormat::Clap ? 1.5 : 0.75; // plain / normalised
        REQUIRE(sendParams(*live, {{0, 1, gain, 0, 0}, {0, 4, 0.5, 0, 0}}) ==
                bridge::BlockResult::Done);
        StateBlob state;
        REQUIRE(live->saveState(state));
        auto copy = host.create(fx::effectId(format), &state, 48000, 256, false, error);
        REQUIRE(copy);
        std::vector<std::pair<std::uint32_t, double>> a;
        std::vector<std::pair<std::uint32_t, double>> b;
        REQUIRE(live->paramValues(a));
        REQUIRE(copy->paramValues(b));
        CHECK(a == b);

        // Into the project through the command: the file now carries it.
        project::CommandStack stack;
        stack.execute(std::make_unique<project::SetPluginState>(slot, encodeBase64(state)),
                      project);
        CHECK(project.mixer.findSlot(slot)->plugin.state == encodeBase64(state));
        REQUIRE(stack.undo(project));
        CHECK(project.mixer.findSlot(slot)->plugin.state ==
              fx::fixtureState(0.375, 32, 0.625, format));
    }
}

TEST_CASE("plugin_instrument_plays", "[plugin][instrument]") {
    fx::useTestHost();
    for (const PluginFormat format : {PluginFormat::Clap, PluginFormat::Vst3}) {
        INFO(toString(format));
        const project::Project project = fx::synthProject(format);
        const std::vector<float> first = fx::renderPlugin(project, std::uint64_t{48000} * 2);
        const std::vector<float> second = fx::renderPlugin(project, std::uint64_t{48000} * 2, 64);
        REQUIRE(fx::peakOf(first) > 0.05F);
        // Deterministic, and the same at another block size: notes reach the plugin at
        // their offsets, not at block starts.
        CHECK(first == second);
        // The second note (tick 1920: a quarter second at 120 BPM) sounds after the first
        // (an eighth of a second, and a 50 ms release) has died away.
        const std::size_t secondNote = 12000;
        float before = 0.0F;
        for (std::size_t f = secondNote - 2400; f < secondNote - 100; ++f) {
            before = std::max(before, std::abs(first[f * 2]));
        }
        float after = 0.0F;
        for (std::size_t f = secondNote + 480; f < secondNote + 4800; ++f) {
            after = std::max(after, std::abs(first[f * 2]));
        }
        CHECK(before < 0.001F);
        CHECK(after > 0.05F);
    }
}

TEST_CASE("plugin_instrument_round_trips_through_text", "[plugin][instrument][format]") {
    fx::useTestHost();
    project::Project project = fx::synthProject(PluginFormat::Vst3);
    project.channels.front().instrument.params.push_back(
        project::ParamValue{.name = "gain", .value = 0.25, .hasCurve = false, .curve = {}});
    project.channels.front().instrument.plugin.state =
        fx::fixtureState(0.5, 0, 0.0, PluginFormat::Vst3);
    const std::string written = format::write(project);
    CHECK(written.find("INSTRUMENT=vst3\nPLUGIN=\"545844415953544E0000000100000002\"\n") !=
          std::string::npos);
    CHECK(written.find("STATE=\"base64:") != std::string::npos);
    const auto loaded = tests::loadText(written);
    CHECK(loaded->project.channels.front().instrument == project.channels.front().instrument);
    CHECK(format::write(loaded->project) == written);
    // gain=0.25 normalised is half of unity (VST3 plain = 2 x normalised).
    const std::vector<float> quiet = fx::renderPlugin(loaded->project, 24000);
    const std::vector<float> loud = fx::renderPlugin(fx::synthProject(PluginFormat::Vst3), 24000);
    CHECK(fx::peakOf(quiet) == fx::peakOf(loud) * 0.5F);
}

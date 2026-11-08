// The sandbox's failure handling (phase_9.md §4.4, §5): every adx_test_plugin
// misbehaviour, and what the host does about it. None of them may take the host down,
// and none may disturb anything but the plugin that misbehaved.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/plugin/Bridge/AudioLink.h"
#include "engine/plugin/Bridge/HostProcess.h"
#include "engine/plugin/PluginHost.h"
#include "engine/plugin/StateBlob.h"
#include "engine/project/ParamRegistry.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/plugin/PluginFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

using namespace adx;
using namespace adx::plugin;
namespace fx = adx::tests::plugins;
using Clock = std::chrono::steady_clock;

struct Notices {
    std::mutex mutex;
    std::vector<bridge::Notice> seen;

    bridge::NoticeSink sink() {
        return [this](const bridge::Notice& n) {
            const std::scoped_lock lock(mutex);
            seen.push_back(n);
        };
    }
    [[nodiscard]] std::size_t count(bridge::Notice::Kind kind) {
        const std::scoped_lock lock(mutex);
        return static_cast<std::size_t>(std::ranges::count(seen, kind, &bridge::Notice::kind));
    }
};

struct Block {
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> outLeft;
    std::vector<float> outRight;

    explicit Block(std::size_t frames, float value = 0.25F)
        : left(frames, value), right(frames, value), outLeft(frames, 7.0F), outRight(frames, 7.0F) {
    }
};

bridge::BlockResult send(bridge::RemoteInstance& instance, Block& block,
                         std::uint32_t deadlineMicros,
                         std::span<const bridge::WireParam> params = {}) {
    const std::array<const float*, 2> inputs{block.left.data(), block.right.data()};
    const std::array<float*, 2> outputs{block.outLeft.data(), block.outRight.data()};
    std::vector<bridge::WireParam> echoes(bridge::kMaxOutParams);
    bridge::BlockRequest request;
    request.inputs = inputs;
    request.frames = static_cast<std::uint32_t>(block.left.size());
    request.params = params;
    request.transport.tempo = 120.0;
    request.deadlineMicros = deadlineMicros;
    std::uint32_t count = 0;
    return bridge::runBlock(instance.link(), request, outputs, echoes, count);
}

bridge::WireParam modeParam(int mode) {
    return bridge::WireParam{
        .offset = 0, .id = 2, .value = static_cast<double>(mode), .flags = 0, .reserved = 0};
}

template<class Predicate> bool waitFor(Predicate predicate, double seconds) {
    const auto until = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < until) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

/// Two channels into the master: A through the hosted effect (unity gain), B direct.
/// `slot` receives the hosted slot's id.
project::Project crashProject(bool withPlugin, bool muteA, core::SlotId* slot = nullptr) {
    project::Project project = tests::toneProject(
        {{.start = 0, .length = std::int64_t{3840} * 8, .pitch = 69, .velocity = 90}});
    project::Channel second = project.channels.front();
    second.id = project.newChannelId();
    second.name = "Other";
    second.output = project.mixer.master;
    project.channels.push_back(second);
    project::NoteClip clip = project.patterns.front().noteClips.front();
    clip.channel = second.id;
    project.patterns.front().noteClips.push_back(clip);
    project.channels.front().muted = muteA;
    if (withPlugin) {
        const core::SlotId id = fx::addFixtureEffect(
            project, fx::EffectSetup{
                         .format = PluginFormat::Clap,
                         .state = {},
                         .params = {{.name = "gain", .value = 1.0}, {.name = "mode", .value = 0.0}},
                         .mix = 1.0F});
        if (slot != nullptr) {
            *slot = id;
        }
    } else {
        project::Insert dry;
        dry.id = project.newInsertId();
        dry.name = "Hosted";
        project.mixer.inserts.push_back(dry);
        project.mixer.routes.push_back(
            project::Route{.id = project.newRouteId(), .from = dry.id, .to = project.mixer.master});
        project.channels.front().output = dry.id;
    }
    return project;
}

} // namespace

TEST_CASE("watchdog_crash_bypasses", "[plugin][watchdog]") {
    fx::useTestHost();
    static_cast<void>(PluginHost::instance().takeNotices());
    constexpr std::uint32_t kBlock = 256;
    constexpr std::uint64_t kBefore = 48000 / 2;
    constexpr std::uint64_t kAfter = 48000;

    const std::vector<float> reference =
        fx::renderPlugin(crashProject(false, false), kBefore + kAfter, kBlock);

    core::SlotId slotId;
    project::Project project = crashProject(true, false, &slotId);
    project::CommandStack stack;
    tests::OfflineRig const rig(kBlock);
    REQUIRE(rig.engine->setProject(project, 0).rebuilt);
    rig.engine->play();
    std::vector<float> out = rig.render(kBefore);
    const core::InsertId owner = project.mixer.ownerOfSlot(slotId);
    const project::ParamResolution mode = project::ParamRegistry::resolve(
        "insert." + std::to_string(owner.value) + ".slot." + std::to_string(slotId.value) + ".mode",
        project);
    REQUIRE(mode.ok());
    REQUIRE(rig.engine->setParam(project, stack, mode.ref, 1.0F)); // crash on process
    rig.render(kAfter, out);
    REQUIRE(out.size() == reference.size());

    // Playback went on: everything after the crash is the dry path, sample for sample -
    // the insert is bypassed, not silenced - and the channel that never went near the
    // plugin is untouched throughout. Only the block or two the crash happened in lost
    // the plugin's own output.
    std::size_t differing = 0;
    for (std::size_t frame = 0; frame < out.size() / 2; ++frame) {
        if (out[frame * 2] != reference[frame * 2]) {
            ++differing;
            REQUIRE(frame >= kBefore);
        }
    }
    CHECK(differing <= std::size_t{3} * kBlock);
    const std::size_t tail = out.size() - (std::size_t{2} * 4800);
    for (std::size_t i = tail; i < out.size(); ++i) {
        REQUIRE(out[i] == reference[i]);
    }
    const auto notices = PluginHost::instance().takeNotices();
    CHECK(std::ranges::count(notices, bridge::Notice::Kind::Crashed, &bridge::Notice::kind) >= 1);
}

TEST_CASE("watchdog_hang_restarts", "[plugin][watchdog]") {
    fx::useTestHost();
    Notices notices;
    auto host = std::make_shared<bridge::HostProcess>(fx::hostExe(), notices.sink());
    std::string error;
    REQUIRE(host->start(error));
    StateBlob state;
    REQUIRE(decodeBase64(fx::fixtureState(0.5, 0, 0.25), state));
    auto effect = host->load(fx::effectId(PluginFormat::Clap), &state, 48000, 256, error);
    INFO(error);
    REQUIRE(effect);

    Block block(256);
    REQUIRE(send(*effect, block, 1'000'000) == bridge::BlockResult::Done);
    CHECK(block.outLeft.front() == 0.125F);

    // Hang for 10 s: realtime deadlines from here on, as the audio thread would.
    const std::array<bridge::WireParam, 1> hang{modeParam(2)};
    const auto start = Clock::now();
    CHECK(send(*effect, block, 3000, hang) == bridge::BlockResult::Late);
    std::int64_t worstMicros = 0;
    bool resumed = false;
    while (Clock::now() - start < std::chrono::seconds(8)) {
        const auto before = Clock::now();
        const bridge::BlockResult result = send(*effect, block, 3000);
        worstMicros = std::max<std::int64_t>(
            worstMicros,
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - before).count());
        if (result == bridge::BlockResult::Done) {
            resumed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double took = std::chrono::duration<double>(Clock::now() - start).count();
    REQUIRE(resumed);
    // Killed at the 2 s watchdog, not after the plugin's 10 s.
    CHECK(took >= 1.9);
    CHECK(took < 6.0);
    // Its state came back with it: half gain, not the default unity.
    CHECK(block.outLeft.front() == 0.125F);
    CHECK(notices.count(bridge::Notice::Kind::Hung) == 1);
    CHECK(notices.count(bridge::Notice::Kind::Restarted) == 1);
    // And the host never waited on it past the deadline.
    CHECK(worstMicros < 20'000);
}

TEST_CASE("watchdog_late_block_silences", "[plugin][watchdog]") {
    fx::useTestHost();
    auto host = std::make_shared<bridge::HostProcess>(fx::hostExe(), nullptr);
    std::string error;
    REQUIRE(host->start(error));
    auto effect = host->load(fx::effectId(PluginFormat::Clap), nullptr, 48000, 256, error);
    REQUIRE(effect);
    Block block(256);
    const std::array<bridge::WireParam, 1> slow{modeParam(8)}; // twice the block's time
    static_cast<void>(send(*effect, block, 1'000'000, slow));

    const std::uint64_t lateBefore = effect->health().lateBlocks.load();
    std::int64_t worstMicros = 0;
    int late = 0;
    for (int i = 0; i < 40; ++i) {
        const auto before = Clock::now();
        const bridge::BlockResult result = send(*effect, block, 1500);
        worstMicros = std::max<std::int64_t>(
            worstMicros,
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - before).count());
        if (result == bridge::BlockResult::Late) {
            ++late;
            for (const float s : block.outLeft) {
                REQUIRE(s == 0.0F);
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(5333)); // a block's time
    }
    CHECK(late >= 30);
    CHECK(effect->health().lateBlocks.load() - lateBefore == static_cast<std::uint64_t>(late));
    // Never a stall in the host: the deadline plus timer slack, whatever the child does.
    CHECK(worstMicros < 4000);

    // The rule that follows: fifty late in a second bypasses.
    bridge::LateCounter counter;
    counter.reset(48000);
    int crossedAt = -1;
    for (int i = 1; i <= 60 && crossedAt < 0; ++i) {
        if (counter.block(256, true)) {
            crossedAt = i;
        }
    }
    CHECK(std::cmp_equal(crossedAt, bridge::kLateLimit));
    bridge::LateCounter spread;
    spread.reset(48000);
    bool crossed = false;
    for (int i = 0; i < 1000; ++i) {
        crossed = crossed || spread.block(256, i % 5 == 0); // 20 % late: ~37 a second
    }
    CHECK_FALSE(crossed);
}

TEST_CASE("plugin_shared_child_isolation", "[plugin][watchdog]") {
    fx::useTestHost();
    PluginHost& plugins = PluginHost::instance();
    std::string error;
    auto first =
        plugins.create(fx::effectId(PluginFormat::Clap), nullptr, 48000, 256, false, error);
    auto second =
        plugins.create(fx::effectId(PluginFormat::Clap), nullptr, 48000, 256, false, error);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first->host() == second->host()); // one shared child

    Block block(256);
    const std::array<bridge::WireParam, 1> crash{modeParam(1)};
    static_cast<void>(send(*first, block, 2'000'000, crash));
    // One crashing takes down both.
    REQUIRE(
        waitFor([&] { return first->health().state.load() == bridge::LinkState::Crashed; }, 3.0));
    REQUIRE(
        waitFor([&] { return second->health().state.load() == bridge::LinkState::Crashed; }, 3.0));
    CHECK(send(*second, block, 100'000) == bridge::BlockResult::NotRunning);

    // Both recover; the crashing one into a process of its own.
    first->setIsolated(true);
    CHECK(plugins.restart(0, error) >= 1);
    REQUIRE(first->health().state.load() == bridge::LinkState::Running);
    REQUIRE(second->health().state.load() == bridge::LinkState::Running);
    CHECK(first->host() != second->host());
    CHECK(send(*second, block, 1'000'000) == bridge::BlockResult::Done);

    // From now on it crashes alone.
    static_cast<void>(send(*first, block, 2'000'000, crash));
    REQUIRE(
        waitFor([&] { return first->health().state.load() == bridge::LinkState::Crashed; }, 3.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(second->health().state.load() == bridge::LinkState::Running);
    CHECK(send(*second, block, 1'000'000) == bridge::BlockResult::Done);
    CHECK(block.outLeft.front() == 0.25F);
    static_cast<void>(plugins.takeNotices());
}

TEST_CASE("plugin_alloc_does_not_affect_host", "[plugin][watchdog][rt]") {
    fx::useTestHost();
    project::Project project = tests::toneProject({{.start = 0, .length = 3840, .pitch = 69}});
    fx::addFixtureEffect(project, fx::EffectSetup{.format = PluginFormat::Clap,
                                                  .state = {},
                                                  .params = {{.name = "mode", .value = 3.0}},
                                                  .mix = 1.0F}); // allocate
    render::OfflineRenderOptions options;
    options.frames = 48000;
    render::RenderStats stats;
    const std::vector<float> out = render::renderOffline(project, options, stats);
    REQUIRE(stats.error.empty());
    CHECK(stats.violations == 0);
    CHECK(fx::peakOf(out) > 0.01F); // and it still played
}

TEST_CASE("plugin_nan_is_contained", "[plugin][watchdog]") {
    fx::useTestHost();
    static_cast<void>(PluginHost::instance().takeNotices());
    core::SlotId slotId;
    project::Project project = crashProject(true, false, &slotId);
    project::Slot* slot = project.mixer.findSlot(slotId);
    REQUIRE(slot != nullptr);
    slot->find("mode")->value = 7.0; // NaN in its first sample
    const std::vector<float> out = fx::renderPlugin(project, 48000);
    CHECK(fx::allFinite(out));
    // Muted, not bypassed: the hosted path is silent; the other channel plays on.
    const std::vector<float> other = fx::renderPlugin(crashProject(false, true), 48000);
    CHECK(out == other);
    const auto notices = PluginHost::instance().takeNotices();
    CHECK(std::ranges::count(notices, bridge::Notice::Kind::NanMuted, &bridge::Notice::kind) >= 1);
}

TEST_CASE("plugin_malformed_state_handled", "[plugin][watchdog]") {
    fx::useTestHost();
    static_cast<void>(PluginHost::instance().takeNotices());
    // Bytes the plugin rejects: it loads with its defaults (unity gain), with a warning.
    core::SlotId slotId;
    project::Project project = crashProject(true, false, &slotId);
    project.mixer.findSlot(slotId)->plugin.state =
        encodeBase64(std::vector<std::uint8_t>{'g', 'a', 'r', 'b', 'a', 'g', 'e', '!'});
    const std::vector<float> out = fx::renderPlugin(project, 24000);
    const std::vector<float> dry = fx::renderPlugin(crashProject(false, false), 24000);
    CHECK(out == dry);
    auto notices = PluginHost::instance().takeNotices();
    CHECK(std::ranges::count(notices, bridge::Notice::Kind::StateRejected, &bridge::Notice::kind) ==
          1);

    // Text that is not base64 at all: the same, never a crash.
    project.mixer.findSlot(slotId)->plugin.state = "@@not base64@@";
    CHECK(fx::renderPlugin(project, 24000) == dry);
    notices = PluginHost::instance().takeNotices();
    CHECK(std::ranges::count(notices, bridge::Notice::Kind::StateRejected, &bridge::Notice::kind) ==
          1);

    // A plugin whose own save is garbage (mode 6): what it saved does not load again.
    std::string error;
    auto source = PluginHost::instance().create(fx::effectId(PluginFormat::Clap), nullptr, 48000,
                                                256, false, error);
    REQUIRE(source);
    Block block(64);
    const std::array<bridge::WireParam, 1> malformed{modeParam(6)};
    REQUIRE(send(*source, block, 1'000'000, malformed) == bridge::BlockResult::Done);
    StateBlob saved;
    REQUIRE(source->saveState(saved));
    auto reloaded = PluginHost::instance().create(fx::effectId(PluginFormat::Clap), &saved, 48000,
                                                  256, false, error);
    REQUIRE(reloaded);
    CHECK(reloaded->health().state.load() == bridge::LinkState::Running);
    notices = PluginHost::instance().takeNotices();
    CHECK(std::ranges::count(notices, bridge::Notice::Kind::StateRejected, &bridge::Notice::kind) ==
          1);
}

// Plugin parameters are parameters (phase_9.md §4.7, §5 plugin_params_automatable): laid
// out from the plugin's own list, named in the file, resolved by path, automated, and
// never overwritten by a default the project did not choose.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "engine/format/adx/Writer.h"
#include "engine/plugin/PluginHost.h"
#include "engine/project/ParamRegistry.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/plugin/PluginFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

using namespace adx;
namespace fx = adx::tests::plugins;

/// A tone through a hosted CLAP effect. `slotExtras` goes on the SLOT line, `stateLine`
/// (if any) under it, `automation` into the pattern.
std::string hostedText(const std::string& slotExtras, const std::string& stateLine = {},
                       const std::string& automation = {}) {
    std::string text =
        "[PROJECT]\nADX_VERSION=2\n"
        "\n[CHANNEL Tone]\nINSTRUMENT=testtone\nOUTPUT=insert.2\n"
        "\n[PATTERN P]\nLENGTH=4:0:0\nNOTES Tone\n  A4 0:0:0 4:0:0 100\n" +
        automation +
        "\n[PLAYLIST]\nTRACK 1 name=\"L\"\n  PATTERN P 0:0:0\n"
        "\n[MIXER]\nINSERT 1 name=\"Master\"\nINSERT 2 name=\"Hosted\"\n"
        "  SLOT 1 clap plugin=\"com.adx.test.effect\" name=\"adX Test Effect\" path=\"" +
        fx::clapPath() + "\"" + slotExtras + "\n";
    if (!stateLine.empty()) {
        text += "    STATE " + stateLine + "\n";
    }
    text += "ROUTE insert.2 -> insert.1\n";
    return text;
}

std::vector<float> renderHosted(const project::Project& project, std::uint64_t frames = 48000) {
    return fx::renderPlugin(project, frames);
}

double rms(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

} // namespace

TEST_CASE("plugin_slot_round_trips_through_text", "[plugin][format]") {
    fx::useTestHost();
    const std::string state = fx::fixtureState(0.5, 0, 0.75);
    const auto loaded =
        tests::loadText(hostedText(" gain=0.5 seed=0.75", "\"base64:" + state + "\""));
    const project::Slot* slot = loaded->project.mixer.findSlot(core::SlotId{1});
    REQUIRE(slot != nullptr);
    CHECK(slot->type == "clap");
    CHECK(slot->plugin.uid == "com.adx.test.effect");
    CHECK(slot->plugin.name == "adX Test Effect");
    CHECK(slot->plugin.path == fx::clapPath());
    CHECK(slot->plugin.state == state);
    REQUIRE(slot->params.size() == 2);
    // Plugin parameters are not the catalog's: no unknown-parameter warnings for them.
    for (const auto& diagnostic : loaded->diagnostics.all()) {
        CHECK(diagnostic.code != format::code::kUnknownParameter);
    }

    const std::string written = format::write(loaded->project);
    CHECK(written.find("  SLOT 1 clap plugin=\"com.adx.test.effect\"") != std::string::npos);
    CHECK(written.find("    STATE \"base64:" + state + "\"\n") != std::string::npos);
    const auto again = tests::loadText(written);
    CHECK(format::write(again->project) == written);
    CHECK(again->project.mixer == loaded->project.mixer);
}

TEST_CASE("plugin_params_absent_keep_the_plugins_own", "[plugin][parammap]") {
    fx::useTestHost();
    // The state says half gain and the slot names no gain: the plugin keeps its half.
    const auto fromState =
        tests::loadText(hostedText("", "\"base64:" + fx::fixtureState(0.5, 0, 0.0) + "\""));
    // The slot names a quarter: the project's value wins over the state's.
    const auto fromSlot = tests::loadText(
        hostedText(" gain=0.25", "\"base64:" + fx::fixtureState(0.5, 0, 0.0) + "\""));
    const auto unity = tests::loadText(hostedText(" gain=1"));
    const std::vector<float> half = renderHosted(fromState->project);
    const std::vector<float> quarter = renderHosted(fromSlot->project);
    const std::vector<float> full = renderHosted(unity->project);
    REQUIRE(fx::peakOf(full) > 0.05F);
    for (std::size_t i = 0; i < full.size(); ++i) {
        REQUIRE(half[i] == full[i] * 0.5F);
        REQUIRE(quarter[i] == full[i] * 0.25F);
    }
}

TEST_CASE("plugin_params_automatable", "[plugin][parammap]") {
    fx::useTestHost();
    // Text-addressable: the same path syntax as a native effect's.
    const auto loaded = tests::loadText(
        hostedText(" gain=0", {}, "AUTOMATION insert.2.slot.1.gain\n  0:0:0 0\n  2:0:0 2\n"));
    const project::ParamResolution path =
        project::ParamRegistry::resolve("insert.2.slot.1.gain", loaded->project);
    REQUIRE(path.ok());
    CHECK(path.ref.kind == project::ParamKind::SlotParam);
    CHECK(project::ParamRegistry::pathOf(path.ref, loaded->project) == "insert.2.slot.1.gain");

    // Automated: the ramp from 0 to 2 over two bars (4 s at 120 BPM) reaches the plugin.
    const std::vector<float> ramped = renderHosted(loaded->project, std::uint64_t{48000} * 4);
    const auto unity = tests::loadText(hostedText(" gain=1"));
    const std::vector<float> dry = renderHosted(unity->project, std::uint64_t{48000} * 4);
    double previous = -1.0;
    for (int quarterSecond = 1; quarterSecond < 16; ++quarterSecond) {
        const std::size_t from = static_cast<std::size_t>(quarterSecond) * 12000 * 2;
        const std::size_t to = from + (std::size_t{4800} * 2);
        const double ratio = rms(ramped, from, to) / rms(dry, from, to);
        // Two bars at 120 BPM are 4 s, so the gain at t is t / 2; the window's middle.
        const double expected = (quarterSecond * 0.25 + 0.05) / 2.0;
        INFO("t=" << quarterSecond * 0.25 << " s ratio " << ratio << " expected ~" << expected);
        CHECK(ratio > previous);
        CHECK(std::abs(ratio - expected) < 0.05);
        previous = ratio;
    }

    // Knob-turnable live, like a native parameter: a value command and no rebuild.
    tests::OfflineRig const rig(256);
    project::Project project = unity->project;
    project::CommandStack stack;
    REQUIRE(rig.engine->setProject(project, 0).rebuilt);
    rig.engine->play();
    const std::vector<float> before = rig.render(4800);
    const project::ParamResolution gain =
        project::ParamRegistry::resolve("insert.2.slot.1.gain", project);
    REQUIRE(rig.engine->setParam(project, stack, gain.ref, 0.5F));
    const std::vector<float> after = rig.render(4800);
    CHECK(rms(after, 960, after.size()) ==
          Catch::Approx(rms(before, 960, before.size()) * 0.5).epsilon(0.05));
}

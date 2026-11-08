// P9-7: a plugin editor's changes as edits and as automation, and plugin instruments
// following adX's pitch - slides, pitch curves and the channel's pitch offset - through
// CLAP and VST3 note tuning.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "engine/plugin/PluginCore.h"
#include "engine/plugin/ProjectSync.h"
#include "engine/project/AutomationWriter.h"
#include "engine/project/commands/CommandStack.h"
#include "tests/cpp/plugin/PluginFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace fx = adx::tests::plugins;
using adx::plugin::ParamEcho;
using adx::plugin::PluginFormat;

namespace {

constexpr double kNone = std::numeric_limits<double>::quiet_NaN();

/// Frequency of the left channel over [from, from + frames), by rising zero crossings.
double frequency(const std::vector<float>& stereo, std::size_t from, std::size_t frames) {
    std::size_t crossings = 0;
    std::size_t first = 0;
    std::size_t last = 0;
    for (std::size_t i = from + 1; i < from + frames; ++i) {
        if (stereo[(i - 1) * 2] < 0.0F && stereo[i * 2] >= 0.0F) {
            first = crossings == 0 ? i : first;
            last = i;
            ++crossings;
        }
    }
    return crossings < 2
               ? 0.0
               : static_cast<double>(crossings - 1) * 48000.0 / static_cast<double>(last - first);
}

} // namespace

TEST_CASE("plugin_echo_is_edit_only_when_it_is_one", "[plugin][automation]") {
    // A value the project already holds is never an edit.
    CHECK_FALSE(adx::plugin::echoIsEdit(
        ParamEcho{.index = 0, .value = 0.5F, .gesture = true, .settling = false}, 0.5));
    // A freshly loaded plugin reporting a value, without a gesture: not an edit (P10.5-2).
    CHECK_FALSE(adx::plugin::echoIsEdit(
        ParamEcho{.index = 0, .value = 0.7F, .gesture = false, .settling = true}, 0.5));
    CHECK_FALSE(adx::plugin::echoIsEdit(
        ParamEcho{.index = 0, .value = 0.7F, .gesture = false, .settling = true}, kNone));
    // The same report inside a gesture is the user's hand, settling or not.
    CHECK(adx::plugin::echoIsEdit(
        ParamEcho{.index = 0, .value = 0.7F, .gesture = true, .settling = true}, 0.5));
    // Later, a plugin that never sends gestures still has its changes kept.
    CHECK(adx::plugin::echoIsEdit(
        ParamEcho{.index = 0, .value = 0.7F, .gesture = false, .settling = false}, 0.5));
    CHECK(adx::plugin::echoIsEdit(
        ParamEcho{.index = 0, .value = 0.7F, .gesture = false, .settling = false}, kNone));
}

TEST_CASE("automation_writer_commits_lanes", "[plugin][automation]") {
    adx::project::Project project = adx::tests::toneProject({});
    adx::project::CommandStack stack;
    const adx::project::ParamRef volume{.owner = project.channels.front().id.value,
                                        .index = 0,
                                        .kind = adx::project::ParamKind::ChannelVolume};
    const adx::project::ParamRef pan{.owner = project.channels.front().id.value,
                                     .index = 0,
                                     .kind = adx::project::ParamKind::ChannelPan};
    adx::project::AutomationWriter writer;
    writer.add(volume, 100, 0.1F); // not writing: ignored
    CHECK(writer.lanes() == 0);

    const std::int64_t bar = adx::core::kPpq * 4;
    writer.begin(bar + 1000); // part way into bar 2
    writer.add(volume, bar + 1000, 0.2F);
    writer.add(volume, bar + 2000, 0.4F);
    writer.add(volume, bar + 2000, 0.5F); // same frame: the later value
    writer.add(volume, bar + 3000, 0.5F);
    writer.add(volume, bar + 4000, 0.5F); // a hold: the last point moves, no new one
    writer.add(pan, bar + 2500, -0.3F);
    CHECK(writer.lanes() == 2);
    const std::string name = writer.commit(project, stack, bar + 5000);
    REQUIRE(name == "Automation 1");
    CHECK_FALSE(writer.writing());

    const adx::project::Pattern* pattern = project.findPatternByName(name);
    REQUIRE(pattern != nullptr);
    REQUIRE(pattern->autoClips.size() == 2);
    const adx::project::AutomationClip& lane = pattern->autoClips[0];
    CHECK(lane.target == volume);
    CHECK_FALSE(lane.targetPath.empty());
    // Relative to the bar the pass began in.
    REQUIRE(lane.points.size() == 3);
    CHECK(lane.points[0].at.value == 1000);
    CHECK(lane.points[0].value == 0.2F);
    CHECK(lane.points[1].at.value == 2000);
    CHECK(lane.points[1].value == 0.5F);
    CHECK(lane.points[2].at.value == 4000);
    // Placed at that bar, on a track of its own; one undo takes it all back.
    bool placed = false;
    for (const adx::project::PlaylistTrack& track : project.playlist.tracks) {
        for (const adx::project::PlaylistItem& item : track.items) {
            placed = placed || (track.name == name && item.start.value == bar);
        }
    }
    CHECK(placed);
    REQUIRE(stack.undo(project));
    CHECK(project.findPatternByName(name) == nullptr);

    // A pass that caught nothing adds nothing.
    writer.begin(0);
    CHECK(writer.commit(project, stack, bar).empty());
}

TEST_CASE("plugin_instrument_follows_pitch", "[plugin][instrument][automation]") {
    fx::useTestHost();
    for (const PluginFormat format : {PluginFormat::Clap, PluginFormat::Vst3}) {
        INFO(toString(format));
        // One A4 a second long, sliding up an octave over its first quarter second.
        adx::project::Project project = fx::synthProject(format);
        adx::project::NoteClip& clip = project.patterns.front().noteClips.front();
        clip.notes.resize(1);
        clip.notes[0].length = adx::core::Ticks{adx::core::kPpq * 2};
        static_cast<void>(clip.setExtras(adx::project::NoteExtras{
            .note = clip.notes[0].id,
            .slide = adx::project::NoteSlide{.targetCents = 1200,
                                             .start = adx::core::Ticks{0},
                                             .length = adx::core::Ticks{adx::core::kPpq / 2},
                                             .curve = {}},
            .pitchCurve = {},
            .lyric = {}}));
        const std::vector<float> slid = fx::renderPlugin(project, 48000);
        const double start = frequency(slid, 200, 1200);
        const double end = frequency(slid, 24000, 9600);
        INFO("start " << start << " Hz, end " << end << " Hz");
        CHECK(std::abs(start - 440.0) < 40.0); // still near A4 in the first 30 ms
        CHECK(std::abs(end - 880.0) < 3.0);

        // And the channel's pitch offset moves every note: +700 cents on a plain A4.
        adx::project::Project offset = fx::synthProject(format);
        offset.channels.front().pitchOffsetCents = 700.0F;
        const std::vector<float> fifth = fx::renderPlugin(offset, 12000);
        const double heard = frequency(fifth, 1200, 4000);
        INFO("offset " << heard << " Hz");
        CHECK(std::abs(heard - (440.0 * std::pow(2.0, 7.0 / 12.0))) < 3.0);
    }
}

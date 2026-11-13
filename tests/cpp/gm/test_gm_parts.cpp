// The `gm` instrument's 16-part mode and "Play on one GM channel" MIDI import
// (phase_12.md §4.7.5, §5).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/format/midi/MidiImporter.h"
#include "engine/instruments/gm/GmSetup.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

namespace fs = std::filesystem;
using adx::tests::noteOff;

namespace {

/// Part `part` (0-15)'s parameter `name` in the harness's parameter vector, which puts
/// the channel's own parameters first.
std::uint32_t named(std::uint32_t part, std::string_view name) {
    return adx::tests::instrumentParamIndex("gm", "p" + std::to_string(part + 1) + "." +
                                                      std::string(name));
}

fs::path tinyBank() {
    return adx::tests::repoRoot() / "tests" / "data" / "banks" / "tiny_gm.sf2";
}

std::shared_ptr<adx::instruments::GmInstrument> gmNode(std::uint16_t polyphony = 16) {
    auto node = std::make_shared<adx::instruments::GmInstrument>(
        1, polyphony, adx::project::VoiceStealMode::OldestReleased);
    std::string error;
    adx::instruments::setGmBank(*node, adx::instruments::GmBankCache::get(tinyBank(), error),
                                tinyBank());
    node->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    return node;
}

adx::graph::BlockEvent noteOn(std::uint32_t offset, std::uint32_t id, std::uint8_t pitch,
                              std::uint8_t velocity, std::uint8_t part) {
    adx::graph::BlockEvent event = adx::tests::noteOn(offset, id, pitch, velocity);
    event.part = part;
    return event;
}

std::vector<float> partParams() {
    std::vector<float> params = adx::tests::instrumentParams("gm");
    params[adx::tests::instrumentParamIndex("gm", "part")] = 1.0F;
    return params;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

/// How sine-like a stretch is: a sine's samples swing smoothly, noise's jump. The mean
/// absolute first difference over the RMS is ~2 pi f / rate for a sine, ~1.4 for noise.
double roughness(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double diff = 0.0;
    for (std::size_t i = from + 1; i < to; ++i) {
        diff += std::abs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1]));
    }
    return (diff / static_cast<double>(to - from - 1)) / std::max(rms(x, from, to), 1e-12);
}

// --- a hand-built SMF: one track per MIDI channel ----------------------------------

void vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(value & 0x7FU)};
    while ((value >>= 7U) != 0) {
        bytes.push_back(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
    }
    out.insert(out.end(), bytes.rbegin(), bytes.rend());
}

void be(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

struct Event {
    std::uint32_t tick;
    std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t> smf(const std::vector<std::vector<Event>>& tracks) {
    std::vector<std::uint8_t> out{'M', 'T', 'h', 'd'};
    be(out, 6, 4);
    be(out, 1, 2);
    be(out, static_cast<std::uint32_t>(tracks.size()), 2);
    be(out, 480, 2);
    for (const auto& events : tracks) {
        std::vector<std::uint8_t> body;
        std::uint32_t last = 0;
        for (const Event& e : events) {
            vlq(body, e.tick - last);
            last = e.tick;
            body.insert(body.end(), e.bytes.begin(), e.bytes.end());
        }
        vlq(body, 0);
        body.insert(body.end(), {0xFF, 0x2F, 0x00});
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        be(out, static_cast<std::uint32_t>(body.size()), 4);
        out.insert(out.end(), body.begin(), body.end());
    }
    return out;
}

/// Sixteen channels, each a note at its own pitch and velocity; channel 10 the closed
/// hat. Channel 3 changes program halfway (to the same sine: bank 0 has one program).
std::vector<std::uint8_t> sixteenChannels() {
    std::vector<std::vector<Event>> tracks;
    for (std::uint8_t ch = 0; ch < 16; ++ch) {
        const auto pitch = static_cast<std::uint8_t>(ch == 9 ? 42 : 48 + (ch * 2));
        const auto velocity = static_cast<std::uint8_t>(60 + (ch * 4));
        std::vector<Event> events{
            {.tick = 0, .bytes = {static_cast<std::uint8_t>(0xC0U | ch), 0}},
            {.tick = 0, .bytes = {static_cast<std::uint8_t>(0x90U | ch), pitch, velocity}},
            {.tick = 480, .bytes = {static_cast<std::uint8_t>(0x80U | ch), pitch, 0}},
        };
        if (ch == 2) {
            events.push_back({.tick = 960, .bytes = {static_cast<std::uint8_t>(0xC0U | ch), 5}});
            events.push_back(
                {.tick = 960, .bytes = {static_cast<std::uint8_t>(0x90U | ch), 70, 90}});
            events.push_back(
                {.tick = 1440, .bytes = {static_cast<std::uint8_t>(0x80U | ch), 70, 0}});
        }
        tracks.push_back(std::move(events));
    }
    return smf(tracks);
}

/// An empty project with a master insert, as a new project has.
void blank(adx::project::Project& project, adx::project::CommandStack& stack) {
    adx::format::DiagnosticList diagnostics;
    adx::format::load("[PROJECT]\nADX_VERSION=2\n\n[MIXER]\nINSERT 1 name=\"Master\"\n", project,
                      stack, diagnostics);
    stack.clear();
}

std::vector<float> render(const adx::project::Project& project) {
    adx::render::OfflineRenderOptions options;
    options.frames = std::uint64_t{48000} * 2;
    adx::render::RenderStats stats;
    std::vector<float> out = adx::render::renderOffline(project, options, stats);
    CHECK(stats.violations == 0);
    return out;
}

} // namespace

TEST_CASE("gm_parts_follow_their_channel", "[gm]") {
    // In part mode a note plays its own part's program, drum switch, volume and pan.
    // Each run on its own node: the sine loops for as long as it is held.
    std::vector<float> params = partParams();
    // Part 1 the sine; part 10 the kit by default; part 3 told it is drums too.
    params[named(2, "drums")] = 1.0F;
    const auto melodic =
        adx::tests::runInstrument(*gmNode(), {noteOn(0, 1, 42, 100, 0)}, 4800, params);
    const auto kit = adx::tests::runInstrument(*gmNode(), {noteOn(0, 2, 42, 100, 9)}, 4800, params);
    const auto told =
        adx::tests::runInstrument(*gmNode(), {noteOn(0, 3, 42, 100, 2)}, 4800, params);
    CHECK(roughness(melodic.left, 480, 4800) < 0.2);
    CHECK(roughness(kit.left, 480, 4800) > 0.6);
    CHECK(roughness(told.left, 480, 4800) > 0.6);

    // Volume is GM's square law, unity at 100; pan is equal-power, unity at the centre.
    params[named(4, "volume")] = 50.0F;
    params[named(5, "pan")] = 1.0F;
    const auto unity =
        adx::tests::runInstrument(*gmNode(), {noteOn(0, 4, 60, 100, 0)}, 4800, params);
    const auto quiet =
        adx::tests::runInstrument(*gmNode(), {noteOn(0, 5, 60, 100, 4)}, 4800, params);
    const auto right =
        adx::tests::runInstrument(*gmNode(), {noteOn(0, 6, 60, 100, 5)}, 4800, params);
    CHECK(std::abs((rms(quiet.left, 2400, 4800) / rms(unity.left, 2400, 4800)) - 0.25) < 1e-3);
    CHECK(rms(right.left, 2400, 4800) < 1e-6);
    CHECK(std::abs((rms(right.right, 2400, 4800) / rms(unity.right, 2400, 4800)) - std::sqrt(2.0)) <
          1e-3);
}

TEST_CASE("gm_drums_channel10", "[gm]") {
    // Part mode: MIDI channel 10 plays the kit without being told; single mode ignores
    // the channel a note carries.
    const auto parts =
        adx::tests::runInstrument(*gmNode(), {noteOn(0, 1, 42, 100, 9)}, 4800, partParams());
    CHECK(roughness(parts.left, 480, 4800) > 0.6);
    const auto single = adx::tests::runInstrument(*gmNode(), {noteOn(0, 2, 42, 100, 9)}, 4800,
                                                  adx::tests::instrumentParams("gm"));
    CHECK(roughness(single.left, 480, 4800) < 0.2);
}

TEST_CASE("gm_exclusive_class_is_per_part", "[gm]") {
    // A closed hat on part 10 chokes part 10's open hat, not another kit part's.
    std::vector<float> params = partParams();
    params[named(10, "drums")] = 1.0F;
    const auto run = adx::tests::runInstrument(
        *gmNode(),
        {noteOn(0, 1, 46, 120, 10), noteOn(0, 2, 46, 120, 9), noteOn(2400, 3, 42, 120, 9)}, 9600,
        params);
    const auto open =
        adx::tests::runInstrument(*gmNode(), {noteOn(0, 4, 46, 120, 10)}, 9600, params);
    // After the choke only part 11's open hat rings: the same as it alone, give or take
    // the closed hat's own tail.
    const double after = rms(run.left, 7200, 9600);
    const double alone = rms(open.left, 7200, 9600);
    CHECK(alone > 1e-4);
    CHECK(std::abs(after - alone) / alone < 0.35);
}

TEST_CASE("gm_16part_midi_import", "[gm][midi]") {
    // A 16-channel .mid on one 16-part gm channel renders the same as the per-channel
    // import on 16 single-part gm channels set the same way.
    std::string error;
    const auto song = adx::format::midi::parseMidi(sixteenChannels(), error);
    REQUIRE(song.has_value());
    if (!song) {
        return;
    }
    REQUIRE(song->tracks.size() == 16);
    const std::string bank = tinyBank().generic_string();

    adx::project::Project parts;
    adx::project::CommandStack partsStack;
    blank(parts, partsStack);
    adx::format::midi::importSong(*song, parts, partsStack, "Song",
                                  adx::format::midi::ImportLayout::GmParts);
    REQUIRE(parts.channels.size() == 1);
    adx::project::Channel& gm = parts.channels.front();
    CHECK(gm.instrument.type == "gm");
    gm.instrument.voicebank = bank;
    // Channel 3's program change is a stepped lane on p3.program.
    const adx::project::Pattern& pattern = parts.patterns.front();
    REQUIRE(pattern.autoClips.size() == 1);
    CHECK(pattern.autoClips.front().targetPath == "channel.GM.p3.program");
    CHECK(pattern.autoClips.front().target.valid());
    CHECK(pattern.autoClips.front().points.size() == 2);
    // Every note kept its channel.
    const adx::project::NoteClip* clip = pattern.clipFor(gm.id);
    REQUIRE(clip != nullptr);
    CHECK(clip->notes.size() == 17);
    CHECK(std::ranges::count_if(clip->notes, [](const adx::project::Note& n) {
              return n.part == 9 && n.pitch == 42;
          }) == 1);

    adx::project::Project single;
    adx::project::CommandStack singleStack;
    blank(single, singleStack);
    adx::format::midi::importSong(*song, single, singleStack, "Song");
    REQUIRE(single.channels.size() == 16);
    for (std::size_t i = 0; i < single.channels.size(); ++i) {
        adx::project::Channel& channel = single.channels[i];
        channel.instrument.type = "gm";
        channel.instrument.voicebank = bank;
        channel.maxPolyphony = 64;
        if (i == 9) {
            adx::project::ParamValue drums;
            drums.name = "drums";
            drums.value = 1.0;
            channel.instrument.params.push_back(drums);
        }
    }
    const std::vector<float> a = render(parts);
    const std::vector<float> b = render(single);
    REQUIRE(a.size() == b.size());
    double peak = 0.0;
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        peak = std::max(peak, static_cast<double>(std::abs(a[i])));
        worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
    }
    CHECK(peak > 0.01);
    CHECK(worst < 1e-5);
}

TEST_CASE("gm_parts_import_controllers", "[gm][midi]") {
    // Volume, pan and expression before the first note set the part; a later volume move
    // is a lane; the sustain pedal holds a note until it lifts.
    const std::vector<std::vector<Event>> tracks{{
        {.tick = 0, .bytes = {0xB1, 7, 80}},
        {.tick = 0, .bytes = {0xB1, 10, 127}},
        {.tick = 0, .bytes = {0xB1, 11, 90}},
        {.tick = 0, .bytes = {0xB1, 64, 127}},
        {.tick = 0, .bytes = {0x91, 60, 100}},
        {.tick = 240, .bytes = {0x81, 60, 0}},
        {.tick = 960, .bytes = {0xB1, 64, 0}},
        {.tick = 1920, .bytes = {0xB1, 7, 40}},
        {.tick = 1920, .bytes = {0x91, 62, 100}},
        {.tick = 2400, .bytes = {0x81, 62, 0}},
    }};
    std::string error;
    const auto song = adx::format::midi::parseMidi(smf(tracks), error);
    REQUIRE(song.has_value());
    if (!song) {
        return;
    }
    adx::project::Project project;
    adx::project::CommandStack stack;
    blank(project, stack);
    adx::format::midi::importSong(*song, project, stack, "Song",
                                  adx::format::midi::ImportLayout::GmParts);
    REQUIRE(project.channels.size() == 1);
    const adx::project::InstrumentSpec& spec = project.channels.front().instrument;
    const auto value = [&](std::string_view name) {
        const adx::project::ParamValue* p = spec.find(name);
        return p == nullptr ? -99.0 : p->value;
    };
    CHECK(value("part") == 1.0);
    CHECK(value("p2.pan") == 1.0);
    CHECK(value("p2.expression") == 90.0);
    CHECK(value("p2.volume") == 80.0); // listed at its first value; the lane moves it
    const adx::project::Pattern& pattern = project.patterns.front();
    REQUIRE(pattern.autoClips.size() == 1);
    CHECK(pattern.autoClips.front().targetPath == "channel.GM.p2.volume");
    CHECK(pattern.autoClips.front().points.front().value == 80.0F);
    CHECK(pattern.autoClips.front().points.back().value == 40.0F);
    const adx::project::NoteClip* clip = pattern.clipFor(project.channels.front().id);
    REQUIRE(clip != nullptr);
    const auto held = std::ranges::find(clip->notes, std::uint8_t{60}, &adx::project::Note::pitch);
    REQUIRE(held != clip->notes.end());
    // 960 file ticks at 480 to the quarter is two quarters: 7680 adX ticks.
    CHECK(held->length.value == 7680);
    CHECK(held->part == 1);
}

TEST_CASE("note_part_roundtrips", "[gm][format]") {
    // A note's MIDI channel is `ch=1..16` in the file, omitted on channel 1.
    const std::string text = "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL Lead]\nINSTRUMENT=gm\n"
                             "OUTPUT=insert.1\n\n[PATTERN P]\nLENGTH=1:0:0\nNOTES Lead\n"
                             "  C4 0:0:0 0:1:0 100\n  D4 0:1:0 0:1:0 100 ch=10\n\n"
                             "[MIXER]\nINSERT 1 name=\"Master\"\n";
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(text, project, stack, diagnostics);
    INFO(diagnostics.size());
    const adx::project::NoteClip* clip =
        project.patterns.front().clipFor(project.channels.front().id);
    REQUIRE(clip != nullptr);
    REQUIRE(clip->notes.size() == 2);
    const auto d4 = std::ranges::find(clip->notes, std::uint8_t{62}, &adx::project::Note::pitch);
    REQUIRE(d4 != clip->notes.end());
    CHECK(d4->part == 9);
    const std::string saved = adx::format::write(project);
    CHECK(saved.find("ch=10") != std::string::npos);
    CHECK(saved.find("ch=1 ") == std::string::npos);
}

TEST_CASE("gm_no_alloc", "[gm][rt]") {
    // Sixteen parts with program and volume moves, under the allocator hook: no
    // allocation (runInstrument runs inside a ScopedRtSection).
    const auto node = gmNode(64);
    std::vector<float> params = partParams();
    std::uint64_t violations = 0;
    for (int round = 0; round < 20; ++round) {
        std::vector<adx::graph::BlockEvent> events;
        for (std::uint8_t part = 0; part < 16; ++part) {
            const auto id = static_cast<std::uint32_t>((round * 16) + part + 1);
            const auto pitch = static_cast<std::uint8_t>(part == 9 ? 42 + (round % 5) : 40 + part);
            events.push_back(noteOn(part * 10U, id, pitch, 100, part));
            events.push_back(noteOff(2400 + (part * 10U), id, pitch));
            params[named(part, "program")] = static_cast<float>(round % 8);
            params[named(part, "volume")] = static_cast<float>(40 + round);
        }
        const auto run = adx::tests::runInstrument(*node, events, 4800, params);
        violations += run.violations;
    }
    CHECK(violations == 0);
}

TEST_CASE("gm_shipped_bank", "[gm][libraries]") {
    // The GM bank in the library store: a lib:// BANK= resolves to it, it holds the 128
    // GM programs and a kit, and C4 on each program sounds.
    const fs::path path =
        adx::instruments::gmBankPath(std::string(adx::instruments::kShippedGmBank), fs::path{});
    if (path.empty()) {
        SKIP("generaluser is not in the library store (fetch_libraries.py fetch generaluser)");
    }
    std::string error;
    const auto bank = adx::instruments::GmBankCache::get(path, error);
    INFO(error);
    REQUIRE(bank != nullptr);
    std::size_t melodic = 0;
    for (const auto& preset : bank->presets) {
        melodic += preset.bank == 0 ? 1 : 0;
    }
    CHECK(melodic == 128);
    auto node = std::make_shared<adx::instruments::GmInstrument>(
        1, 4, adx::project::VoiceStealMode::OldestReleased);
    adx::instruments::setGmBank(*node, bank, path);
    node->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    std::vector<float> params = adx::tests::instrumentParams("gm");
    const std::uint32_t program = adx::tests::instrumentParamIndex("gm", "program");
    for (int p = 0; p < 128; ++p) {
        INFO("program " << p);
        params[program] = static_cast<float>(p);
        const auto id = static_cast<std::uint32_t>(p + 1);
        const auto run = adx::tests::runInstrument(
            *node, {adx::tests::noteOn(0, id, 60, 100), noteOff(9600, id, 60)}, 12000, params);
        CHECK(rms(run.left, 0, 9600) > 3e-4);
    }
}

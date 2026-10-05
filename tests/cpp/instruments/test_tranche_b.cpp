// Tranche B's instruments (phase_4.md §4.6-§4.8, §6): Slicer, sample-pool channel,
// DrumSynth and the hardstyle kick lineage, Granular, FM, Wavetable. The contract
// every instrument shares - lifecycle, no allocation, silence when idle, golden hashes
// - is test_instrument_base.cpp and test_golden_units.cpp, which pick these up from the
// catalog. These are what each one is *for*.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "engine/format/audio/SamplePool.h"
#include "engine/instruments/Factory.h"
#include "engine/instruments/drumsynth/DrumSynth.h"
#include "engine/instruments/drumsynth/HardstyleKick.h"
#include "engine/instruments/fm/FmInstrument.h"
#include "engine/instruments/granular/GranularInstrument.h"
#include "engine/instruments/pool/PoolZones.h"
#include "engine/instruments/pool/SamplePoolChannel.h"
#include "engine/instruments/slicer/SliceLayout.h"
#include "engine/instruments/slicer/Slicer.h"
#include "engine/instruments/wavetable/WavetableSetup.h"
#include "engine/project/Project.h"
#include "engine/project/TypeCatalog.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/dsp/DspTestUtil.h"
#include "tests/cpp/instruments/InstrumentHarness.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace fs = std::filesystem;
using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;
using adx::tests::InstrumentRun;
using adx::tests::noteOff;
using adx::tests::noteOn;
using adx::tests::preparedInstrument;
using adx::tests::runInstrument;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

fs::path scratch() {
    const fs::path dir = fs::temp_directory_path() / "adx_tranche_b_tests";
    fs::create_directories(dir);
    return dir;
}

fs::path writeMono(const std::string& name, const std::vector<float>& samples) {
    const fs::path path = scratch() / name;
    REQUIRE(adx::render::writeWavFloat32(path.string(), samples, 1, 48000));
    return path;
}

void set(std::vector<float>& params, std::string_view type, std::string_view name, float value) {
    const std::uint32_t index = instrumentParamIndex(type, name);
    REQUIRE(index != adx::project::kNoParam);
    params[index] = value;
}

/// A sample-playing channel of `type` with `zones`, all on one file.
struct ZoneRig {
    adx::project::Project project;
    adx::format::SamplePool pool{1};
    std::shared_ptr<adx::graph::ChannelNode> node;

    ZoneRig(std::string_view type, const fs::path& file,
            std::vector<adx::project::SampleZone> zones) {
        project.resources.baseDirectory = scratch().string();
        adx::project::SampleRef ref;
        ref.id = project.newSampleId();
        ref.path = file.filename().string();
        project.resources.samples.push_back(ref);
        adx::project::Channel channel;
        channel.id = project.newChannelId();
        channel.instrument.type = std::string(type);
        for (adx::project::SampleZone& zone : zones) {
            zone.sample = ref.id;
        }
        channel.instrument.zones = std::move(zones);
        project.channels.push_back(channel);
        node = adx::instruments::makeInstrument(
            project.channels.front(),
            adx::instruments::InstrumentContext{.resources = &project.resources, .pool = &pool});
        node->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
        pool.waitAll();
    }
};

/// Sample-player parameters with nothing in the way: full velocity sensitivity off.
std::vector<float> flat(std::string_view type) {
    std::vector<float> params = instrumentParams(type);
    set(params, type, "velocity", 0.0F);
    return params;
}

/// Zero crossings per second over [from, from + length) of the left channel.
double crossingRate(const InstrumentRun& run, std::size_t from, std::size_t length) {
    std::size_t crossings = 0;
    for (std::size_t i = from + 1; i < from + length; ++i) {
        if ((run.left[i - 1] < 0.0F) != (run.left[i] < 0.0F)) {
            ++crossings;
        }
    }
    return static_cast<double>(crossings) * 48000.0 / static_cast<double>(length);
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

std::vector<float> window(const InstrumentRun& run, std::size_t from, std::size_t length) {
    return {run.left.begin() + static_cast<std::ptrdiff_t>(from),
            run.left.begin() + static_cast<std::ptrdiff_t>(from + length)};
}

} // namespace

// --- Slicer ---------------------------------------------------------------------

TEST_CASE("slicer_even_slices_cover_the_sample", "[instruments][slicer]") {
    const auto zones = adx::instruments::evenSlices(adx::core::SampleId{7}, 48000, 4, 36);
    REQUIRE(zones.size() == 4);
    for (std::size_t s = 0; s < zones.size(); ++s) {
        CHECK(zones[s].keyLow == 36 + s);
        CHECK(zones[s].keyHigh == zones[s].keyLow);
        CHECK(zones[s].rootKey == zones[s].keyLow);
        CHECK(zones[s].start == 12000 * s);
        CHECK(zones[s].end == 12000 * (s + 1));
    }
}

TEST_CASE("slicer_plays_only_its_slice", "[instruments][slicer]") {
    // A ramp names the frame it came from: key 37 plays frames 1200..2399 and stops.
    std::vector<float> ramp(4800);
    for (std::size_t k = 0; k < ramp.size(); ++k) {
        ramp[k] = 0.1F + (static_cast<float>(k) / 10000.0F);
    }
    const ZoneRig rig("slicer", writeMono("slicer_ramp.wav", ramp),
                      adx::instruments::evenSlices({}, 4800, 4, 36));
    const InstrumentRun run =
        runInstrument(*rig.node, {noteOn(0, 1, 37, 127)}, 4800, flat("slicer"));
    CHECK(run.left[0] == ramp[1200]);
    CHECK(run.left[500] == ramp[1700]);
    CHECK(run.left[1199] < ramp[2399] * 0.05F); // faded at the slice's end
    for (std::size_t i = 1200; i < 4800; ++i) {
        REQUIRE(run.left[i] == 0.0F);
    }
    CHECK(run.sounding.back() == 0);
}

TEST_CASE("slicer_new_slice_chokes_the_last", "[instruments][slicer]") {
    const ZoneRig rig("slicer", writeMono("slicer_flat.wav", std::vector<float>(48000, 0.25F)),
                      adx::instruments::evenSlices({}, 48000, 2, 36));
    // Slice 0 at 0, slice 1 at 4800: by 4800 + 2 ms only slice 1 is sounding.
    const InstrumentRun run = runInstrument(
        *rig.node, {noteOn(0, 1, 36, 127), noteOn(4800, 2, 37, 127)}, 9600, flat("slicer"));
    CHECK(run.left[4700] == 0.25F);
    CHECK(run.left[4800 + 200] == 0.25F); // the choked voice is gone, the new one alone
    CHECK(run.left[4801] > 0.25F);        // both, briefly, while the first fades
}

TEST_CASE("slicer_tempo_change", "[instruments][slicer][render]") {
    // phase_4.md §6: changing project tempo respaces slices without re-slicing or
    // re-pitching. Four slices of one second recorded at 120 BPM, laid out as notes;
    // rendered at 120 and at 150 BPM, each slice starts where its beat now falls and
    // plays the same samples.
    std::vector<float> source(48000);
    for (std::size_t k = 0; k < source.size(); ++k) {
        source[k] = 0.2F + (0.1F * static_cast<float>(std::sin(static_cast<double>(k) * 0.01)));
    }
    const fs::path file = writeMono("slicer_tempo.wav", source);
    const auto warm = adx::format::SamplePool::global().request(file);
    adx::format::SamplePool::global().waitAll();

    std::vector<std::vector<float>> renders;
    for (const double bpm : {120.0, 150.0}) {
        const auto zones = adx::instruments::evenSlices(adx::core::SampleId{}, 48000, 4, 36);
        const auto notes = adx::instruments::slicePattern(zones, 48000, 120.0);
        std::vector<adx::tests::NoteSpec> specs;
        specs.reserve(notes.size());
        for (const adx::instruments::SliceNote& note : notes) {
            specs.push_back({.start = note.start.value,
                             .length = note.length.value,
                             .pitch = note.pitch,
                             .velocity = 127});
        }
        adx::project::Project project = adx::tests::toneProject(specs, bpm);
        project.resources.baseDirectory = scratch().string();
        adx::project::SampleRef ref;
        ref.id = project.newSampleId();
        ref.path = file.filename().string();
        project.resources.samples.push_back(ref);
        adx::project::Channel& channel = project.channels.front();
        channel.instrument.type = "slicer";
        channel.instrument.zones = zones;
        for (adx::project::SampleZone& zone : channel.instrument.zones) {
            zone.sample = ref.id;
        }
        adx::project::ParamValue velocity;
        velocity.name = "velocity";
        velocity.value = 0.0;
        channel.instrument.params.push_back(velocity);
        adx::render::RenderStats stats;
        renders.push_back(adx::tests::renderFrames(project, 256, 48000, stats));
    }

    // Slice s starts at s * 0.25 s at 120 BPM and s * 0.2 s at 150; at 150 the next slice
    // chokes it after 9600 frames, so compare up to 9000. At 150 the slice before is still
    // fading out (the 2 ms choke) for the first 96 frames, so start past that.
    for (std::size_t s = 0; s < 4; ++s) {
        INFO("slice " << s);
        const std::size_t at120 = s * 12000;
        const std::size_t at150 = s * 9600;
        CHECK(adx::tests::firstSoundFrom(renders[0], at120) == at120);
        CHECK(adx::tests::firstSoundFrom(renders[1], at150) == at150);
        for (std::size_t i = 200; i < 9000; ++i) {
            REQUIRE(renders[0][(at120 + i) * 2] == renders[1][(at150 + i) * 2]);
        }
    }
}

// --- Sample-pool channel ------------------------------------------------------------

TEST_CASE("pool_plays_its_sample_at_recorded_pitch_on_any_key", "[instruments][pool]") {
    std::vector<float> ramp(2400);
    for (std::size_t k = 0; k < ramp.size(); ++k) {
        ramp[k] = static_cast<float>(k) / 4000.0F;
    }
    const adx::project::SampleZone zone; // every key, root 60
    const ZoneRig rig("pool", writeMono("pool_ramp.wav", ramp), {zone});
    const InstrumentRun run =
        runInstrument(*rig.node, {noteOn(0, 1, 79, 127), noteOff(10, 1, 79)}, 2400, flat("pool"));
    // A one-shot, bit for bit, two octaves above its root: no repitch, no envelope.
    for (std::size_t i = 0; i < 2390; ++i) {
        REQUIRE(run.left[i] == ramp[i]);
    }
}

TEST_CASE("pool_zones_are_one_key_each", "[instruments][pool]") {
    const std::vector<adx::core::SampleId> samples{adx::core::SampleId{3}, adx::core::SampleId{4},
                                                   adx::core::SampleId{9}};
    const auto zones = adx::instruments::poolZones(samples, 36);
    REQUIRE(zones.size() == 3);
    CHECK(zones[2].sample == adx::core::SampleId{9});
    CHECK(zones[2].keyLow == 38);
    CHECK(zones[2].keyHigh == 38);
}

// --- DrumSynth ------------------------------------------------------------------------

TEST_CASE("drumsynth_kick_sweeps_down_to_its_pitch", "[instruments][drumsynth]") {
    std::vector<float> params = instrumentParams("drumsynth");
    set(params, "drumsynth", "click", 0.0F);
    set(params, "drumsynth", "decay", 1.0F);
    const InstrumentRun run =
        runInstrument(*preparedInstrument("drumsynth"), {noteOn(0, 1, 60, 127)}, 24000, params);
    // The first 20 ms sweeps down from 200 Hz (two octaves up) and is well above the
    // resting pitch; 200-400 ms is the resting pitch, 50 Hz: 100 zero crossings a second.
    CHECK(crossingRate(run, 0, 960) > 250.0);
    CHECK_THAT(crossingRate(run, 9600, 9600), WithinAbs(100.0, 6.0));
}

TEST_CASE("drumsynth_models_have_their_own_spectra", "[instruments][drumsynth]") {
    // Brightness by zero crossings over the first 50 ms: the hat is the brightest and
    // the kick the darkest; every model sounds and ends.
    std::vector<double> rates;
    for (int model = 0; model < 5; ++model) {
        INFO("model " << model);
        std::vector<float> params = instrumentParams("drumsynth");
        set(params, "drumsynth", "model", static_cast<float>(model));
        set(params, "drumsynth", "click", 0.0F);
        const InstrumentRun run = runInstrument(*preparedInstrument("drumsynth"),
                                                {noteOn(0, 1, 60, 127)}, 48000 * 3, params);
        CHECK(rms(run.left, 0, 2400) > 0.01);
        CHECK(run.sounding.back() == 0);
        rates.push_back(crossingRate(run, 0, 2400));
    }
    CHECK(rates[2] == *std::ranges::max_element(rates)); // hat
    CHECK(rates[0] == *std::ranges::min_element(rates)); // kick
}

TEST_CASE("drumsynth_kit_maps_general_midi", "[instruments][drumsynth]") {
    using adx::instruments::DrumModel;
    using adx::instruments::kitModel;
    CHECK(kitModel(36) == DrumModel::Kick);
    CHECK(kitModel(38) == DrumModel::Snare);
    CHECK(kitModel(39) == DrumModel::Clap);
    CHECK(kitModel(42) == DrumModel::Hat);
    CHECK(kitModel(46) == DrumModel::Hat);
    CHECK(kitModel(45) == DrumModel::Tom);
}

TEST_CASE("hardstyle_kick_follows_the_melody_root", "[instruments][drumsynth]") {
    using adx::core::kPpq;
    using adx::core::Ticks;
    // C4 for two beats, then E4, then G#4 (8 above C: wraps to -4).
    const std::vector<adx::instruments::MelodyNote> melody{
        {.start = Ticks{0}, .length = Ticks{2 * kPpq}, .pitch = 60},
        {.start = Ticks{2 * kPpq}, .length = Ticks{2 * kPpq}, .pitch = 64},
        {.start = Ticks{4 * kPpq}, .length = Ticks{kPpq}, .pitch = 68},
    };
    const auto kicks = adx::instruments::hardstyleKickNotes(melody, Ticks{kPpq / 2}, 8, 36);
    REQUIRE(kicks.size() == 10);
    CHECK(kicks[0].pitch == 36);
    CHECK(kicks[3].pitch == 36);
    CHECK(kicks[4].pitch == 40);
    CHECK(kicks[7].pitch == 40);
    CHECK(kicks[8].pitch == 32);
    CHECK(kicks[1].start == Ticks{kPpq / 2});
    CHECK(adx::instruments::wrapSemitones(7) == -5);
    CHECK(adx::instruments::wrapSemitones(6) == 6);
    CHECK(adx::instruments::wrapSemitones(-1) == -1);
    // No melody: the fallback bars, flat.
    CHECK(adx::instruments::hardstyleKickNotes({}, Ticks{kPpq}, 2, 36).size() == 8);

    // The patch is the DrumSynth's, and it is the shipped preset.
    std::vector<float> params = instrumentParams("drumsynth");
    for (const auto& [name, value] : adx::instruments::hardstyleKickParams()) {
        set(params, "drumsynth", name, value);
    }
    const InstrumentRun run =
        runInstrument(*preparedInstrument("drumsynth"), {noteOn(0, 1, 36, 127)}, 24000, params);
    float peak = 0.0F;
    for (const float s : run.left) {
        peak = std::max(peak, std::abs(s));
    }
    CHECK(peak > 0.2F);
    CHECK(peak <= 1.0F);
}

// --- Granular -------------------------------------------------------------------------

TEST_CASE("granular_density_sets_the_grain_rate", "[instruments][granular]") {
    // Sparse, short grains of a sine: count the bursts in one second.
    std::vector<float> params = instrumentParams("granular");
    set(params, "granular", "source", 1.0F);
    set(params, "granular", "density", 10.0F);
    set(params, "granular", "size", 20.0F);
    set(params, "granular", "env.attack", 0.0F);
    const InstrumentRun run =
        runInstrument(*preparedInstrument("granular"), {noteOn(0, 1, 69, 127)}, 48000, params);
    int bursts = 0;
    bool inBurst = false;
    std::size_t quiet = 0;
    for (const float s : run.left) {
        if (std::abs(s) > 1e-6F) {
            if (!inBurst) {
                ++bursts;
            }
            inBurst = true;
            quiet = 0;
        } else if (++quiet > 48) {
            inBurst = false;
        }
    }
    CHECK(bursts == 10);
}

TEST_CASE("granular_block_size_independent", "[instruments][granular][render]") {
    std::vector<float> params = instrumentParams("granular");
    set(params, "granular", "source", 2.0F);
    set(params, "granular", "pitchJitter", 300.0F);
    const std::vector<adx::graph::BlockEvent> events{noteOn(0, 1, 57, 100), noteOn(700, 2, 64, 90),
                                                     noteOff(30000, 1, 57), noteOff(30000, 2, 64)};
    const InstrumentRun a =
        runInstrument(*preparedInstrument("granular"), events, 48000, params, 64);
    const InstrumentRun b =
        runInstrument(*preparedInstrument("granular"), events, 48000, params, 1024);
    CHECK(a.left == b.left);
    CHECK(a.right == b.right);
}

TEST_CASE("granular_reads_the_sample_at_its_position", "[instruments][granular]") {
    // The first half of the sample at 0.1, the second at 0.3: grains from 0.75 are
    // three times as loud as grains from 0.25.
    std::vector<float> two(96000, 0.1F);
    std::fill(two.begin() + 48000, two.end(), 0.3F);
    adx::project::SampleZone zone;
    zone.rootKey = 60;
    const fs::path file = writeMono("granular_two.wav", two);
    std::array<double, 2> level{};
    for (const int which : {0, 1}) {
        const ZoneRig rig("granular", file, {zone});
        std::vector<float> params = instrumentParams("granular");
        set(params, "granular", "position", which == 0 ? 0.25F : 0.75F);
        set(params, "granular", "spray", 0.0F);
        set(params, "granular", "spread", 0.0F);
        const InstrumentRun run = runInstrument(*rig.node, {noteOn(0, 1, 60, 127)}, 24000, params);
        level[static_cast<std::size_t>(which)] = rms(run.left, 4800, 24000);
    }
    CHECK_THAT(level[1] / level[0], WithinRel(3.0, 0.01));
}

// --- FM -------------------------------------------------------------------------------

TEST_CASE("fm_algorithms_are_the_dx7s", "[instruments][fm]") {
    // Carriers per algorithm, from the DX7's charts.
    constexpr std::array<int, 32> kCarriers{2, 2, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
                                            1, 1, 3, 3, 4, 4, 4, 5, 5, 3, 3, 3, 4, 4, 5, 6};
    for (int a = 1; a <= 32; ++a) {
        INFO("algorithm " << a);
        const adx::instruments::FmAlgorithm& algo = adx::instruments::fmAlgorithm(a);
        int carriers = 0;
        for (std::uint32_t op = 0; op < 6; ++op) {
            carriers += static_cast<int>((algo.carriers >> op) & 1U);
            // Modulation runs from higher operators to lower: evaluation order 6..1 works.
            CHECK((algo.modulators[op] & ((1U << (op + 1)) - 1U)) == 0);
        }
        CHECK(carriers == kCarriers[static_cast<std::size_t>(a - 1)]);
        CHECK(algo.carriers & 1U); // operator 1 is a carrier in every one
    }
    CHECK(adx::instruments::fmAlgorithm(1).modulators[0] == 0b10);
    CHECK(adx::instruments::fmAlgorithm(32).modulators == std::array<std::uint8_t, 6>{});
}

namespace {

std::vector<float> fmFlat() {
    std::vector<float> params = instrumentParams("fm");
    set(params, "fm", "level", 0.0F);
    for (int op = 1; op <= 6; ++op) {
        const std::string p = "op" + std::to_string(op) + ".";
        set(params, "fm", p + "attack", 0.0F);
        set(params, "fm", p + "sustain", 1.0F);
        set(params, "fm", p + "level", 0.0F);
        set(params, "fm", p + "velocity", 0.0F);
    }
    return params;
}

} // namespace

TEST_CASE("fm_modulation_follows_bessel", "[instruments][fm]") {
    // Operator 2 at ratio 3 modulating operator 1: the carrier at f carries J0(b), the
    // first upper sideband at 4f carries J1(b), b in radians = 2 pi x index x level.
    std::vector<float> params = fmFlat();
    set(params, "fm", "op1.level", 1.0F);
    set(params, "fm", "op2.ratio", 3.0F);
    constexpr double kBeta = 0.3;
    set(params, "fm", "op2.level",
        static_cast<float>(kBeta / (2.0 * std::numbers::pi * adx::instruments::kFmModIndex)));
    const InstrumentRun run =
        runInstrument(*preparedInstrument("fm"), {noteOn(0, 1, 45, 127)}, 48000, params);
    const std::vector<float> steady = window(run, 4800, 32768);
    const double carrier = adx::tests::toneAmplitude(steady, 110.0, 48000.0);
    const double sideband = adx::tests::toneAmplitude(steady, 440.0, 48000.0);
    // J1(0.3) / J0(0.3); algorithm 1 has two carriers, each scaled by 1/sqrt(2).
    CHECK_THAT(sideband / carrier, WithinRel(0.148319 / 0.977626, 0.01));
    CHECK_THAT(carrier, WithinRel(0.977626 / std::sqrt(2.0), 0.01));
}

TEST_CASE("fm_single_carrier_is_a_sine", "[instruments][fm]") {
    std::vector<float> params = fmFlat();
    set(params, "fm", "algorithm", 32.0F);
    set(params, "fm", "op1.level", 1.0F);
    const InstrumentRun run =
        runInstrument(*preparedInstrument("fm"), {noteOn(0, 1, 69, 127)}, 48000, params);
    const std::vector<float> steady = window(run, 4800, 32768);
    // Algorithm 32 has six carriers, each scaled by 1/sqrt(6).
    CHECK_THAT(adx::tests::toneAmplitude(steady, 440.0, 48000.0),
               WithinRel(1.0 / std::sqrt(6.0), 0.01));
    CHECK(adx::tests::toneAmplitude(steady, 880.0, 48000.0) < 1e-4);
}

TEST_CASE("fm_feedback_is_stable", "[instruments][fm]") {
    std::vector<float> params = fmFlat();
    set(params, "fm", "feedback", 1.0F);
    for (int op = 1; op <= 6; ++op) {
        set(params, "fm", "op" + std::to_string(op) + ".level", 1.0F);
    }
    for (int a = 1; a <= 32; ++a) {
        INFO("algorithm " << a);
        set(params, "fm", "algorithm", static_cast<float>(a));
        const InstrumentRun run =
            runInstrument(*preparedInstrument("fm"), {noteOn(0, 1, 60, 127)}, 9600, params);
        for (const float s : run.left) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::abs(s) <= 4.0F);
        }
    }
}

// --- Wavetable --------------------------------------------------------------------------

namespace {

std::vector<float> wtFlat() {
    std::vector<float> params = instrumentParams("wavetable");
    set(params, "wavetable", "env.attack", 0.0F);
    set(params, "wavetable", "env.sustain", 1.0F);
    set(params, "wavetable", "level", 0.0F);
    return params;
}

std::vector<float> wtSteady(std::vector<float> params, std::uint8_t note = 69) {
    const InstrumentRun run = runInstrument(*preparedInstrument("wavetable"),
                                            {noteOn(0, 1, note, 127)}, 48000, std::move(params));
    return window(run, 4800, 32768);
}

} // namespace

TEST_CASE("wavetable_position_moves_through_the_frames", "[instruments][wavetable]") {
    // The Classic table: sine, triangle, saw, square. Position 0 is a sine; position 1
    // is a square, whose third harmonic is a third of its first.
    std::vector<float> params = wtFlat();
    const std::vector<float> sine = wtSteady(params);
    CHECK(adx::tests::toneAmplitude(sine, 880.0, 48000.0) <
          1e-3 * adx::tests::toneAmplitude(sine, 440.0, 48000.0));
    set(params, "wavetable", "position", 1.0F);
    const std::vector<float> square = wtSteady(params);
    CHECK_THAT(adx::tests::toneAmplitude(square, 1320.0, 48000.0) /
                   adx::tests::toneAmplitude(square, 440.0, 48000.0),
               WithinRel(1.0 / 3.0, 0.01));
}

TEST_CASE("wavetable_is_band_limited", "[instruments][wavetable]") {
    // The saw frame at 8.4 kHz (note 120): -60 dBc, as phase_4.md §4.1 asks of every
    // oscillator.
    std::vector<float> params = wtFlat();
    set(params, "wavetable", "position", 2.0F / 3.0F);
    const std::vector<float> saw = wtSteady(params, 120);
    CHECK(adx::tests::aliasingDbc(saw, 8372.018, 48000.0) < -60.0);
}

TEST_CASE("wavetable_spectral_morph_meets_linear_at_the_frames", "[instruments][wavetable]") {
    // At a source frame the two morphs read the same cycle; between frames they differ:
    // the spectral one keeps the harmonics both frames share at full strength.
    for (const float position : {0.0F, 1.0F / 3.0F, 1.0F}) {
        INFO("position " << position);
        std::vector<float> params = wtFlat();
        set(params, "wavetable", "position", position);
        const std::vector<float> linear = wtSteady(params);
        set(params, "wavetable", "morph", 1.0F);
        const std::vector<float> spectral = wtSteady(params);
        for (std::size_t i = 0; i < linear.size(); i += 97) {
            REQUIRE_THAT(spectral[i], WithinAbs(linear[i], 2e-3));
        }
    }
    std::vector<float> params = wtFlat();
    set(params, "wavetable", "position", 1.0F / 6.0F); // halfway, sine to triangle
    const std::vector<float> linear = wtSteady(params);
    set(params, "wavetable", "morph", 1.0F);
    const std::vector<float> spectral = wtSteady(params);
    CHECK(std::abs(rms(linear, 0, linear.size()) - rms(spectral, 0, spectral.size())) > 1e-4);
}

TEST_CASE("wavetable_imports_a_wav_table", "[instruments][wavetable]") {
    // Two 2048-sample frames: a sine, then a sine at three times the frequency.
    std::vector<float> cycles(4096);
    for (std::size_t i = 0; i < 2048; ++i) {
        const double t = static_cast<double>(i) / 2048.0;
        cycles[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * t));
        cycles[2048 + i] = static_cast<float>(std::sin(6.0 * std::numbers::pi * t));
    }
    const fs::path file = writeMono("wt_import.wav", cycles);
    adx::project::Project project;
    project.resources.baseDirectory = scratch().string();
    adx::project::SampleRef ref;
    ref.id = project.newSampleId();
    ref.path = file.filename().string();
    project.resources.samples.push_back(ref);
    adx::project::Channel channel;
    channel.id = project.newChannelId();
    channel.instrument.type = "wavetable";
    adx::project::SampleZone zone;
    zone.sample = ref.id;
    channel.instrument.zones.push_back(zone);
    const auto node = adx::instruments::makeInstrument(
        channel, adx::instruments::InstrumentContext{.resources = &project.resources});
    node->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    CHECK(adx::instruments::configMatches(
        *node, channel, adx::instruments::InstrumentContext{.resources = &project.resources}));

    std::vector<float> params = wtFlat();
    set(params, "wavetable", "table", 4.0F);
    set(params, "wavetable", "position", 1.0F);
    const InstrumentRun run = runInstrument(*node, {noteOn(0, 1, 57, 127)}, 48000, params);
    const std::vector<float> steady = window(run, 4800, 32768);
    // Frame 2 is the third harmonic alone: 660 Hz at note A3.
    CHECK(adx::tests::toneAmplitude(steady, 660.0, 48000.0) > 0.5);
    CHECK(adx::tests::toneAmplitude(steady, 220.0, 48000.0) < 1e-3);
}

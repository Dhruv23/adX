// SFZ import and library paths (phase_12.md §4.7.2, §4.7.3, §5).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/format/audio/LibraryPaths.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/format/sfz/SfzParser.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/preset/SfzImport.h"
#include "engine/project/Resources.h"
#include "tests/cpp/Corpus.h"

namespace fs = std::filesystem;
using adx::format::sfz::Import;
using adx::format::sfz::NoteKind;
using adx::format::sfz::parseSfz;
using adx::project::LoopMode;
using adx::project::ZoneTrigger;

namespace {

constexpr const char* kText = R"(// A comment
#define $EXT wav
<control>
default_path=Samples\Violin\ note_offset=0
<global> ampeg_release=0.4 volume=-3
<group> sw_lokey=c1 sw_hikey=d1 sw_last=c1 sw_label=Sustain
#include "notes.txt"
<group> sw_lokey=c1 sw_hikey=d1 sw_last=d1 trigger=release rt_decay=6
<region> sample=rel A4.$EXT key=a4 volume=2
<group> seq_length=2 loop_mode=loop_continuous loop_start=100 loop_end=999
<region> sample=rr1.$EXT lokey=60 hikey=61 pitch_keycenter=60 seq_position=1 group=1 off_by=2
<region> sample=rr2.$EXT lokey=60 hikey=61 pitch_keycenter=60 seq_position=2 pan=-50
<region> sample=odd.$EXT key=72 fil_type=lpf_2p cutoff=900 transpose=1 tune=-12
)";

constexpr const char* kNotes =
    R"(<region> sample=sus pp C4.$EXT lokey=58 hikey=62 pitch_keycenter=c4 lovel=0 hivel=80 xfout_lovel=60 xfout_hivel=80
<region> sample=sus ff C4.$EXT lokey=58 hikey=62 pitch_keycenter=c4 lovel=60 hivel=127 xfin_lovel=60 xfin_hivel=80 tune=7
)";

Import parsed() {
    return parseSfz(kText, [](std::string_view name, std::string& text) {
        if (name != "notes.txt") {
            return false;
        }
        text = kNotes;
        return true;
    });
}

} // namespace

TEST_CASE("sfz_parse_maps_regions", "[sfz]") {
    const Import import = parsed();
    REQUIRE(import.regions.size() == 6);

    // The included regions: default_path joined, a path with spaces kept whole, the
    // group's keyswitch and the global's volume and release inherited.
    const auto& soft = import.regions[0];
    CHECK(soft.sample == "Samples/Violin/sus pp C4.wav");
    CHECK(soft.zone.keyLow == 58);
    CHECK(soft.zone.keyHigh == 62);
    CHECK(soft.zone.rootKey == 60);
    CHECK(soft.zone.velocityLow == 0);
    CHECK(soft.zone.velocityHigh == 80);
    CHECK(soft.zone.xfadeOutLow == 60);
    CHECK(soft.zone.xfadeOutHigh == 80);
    CHECK(soft.zone.switchLow == 24);
    CHECK(soft.zone.switchHigh == 26);
    CHECK(soft.zone.switchKey == 24);
    CHECK(soft.zone.gainDb == -3.0F);
    CHECK(soft.release == 0.4F);
    const auto& loud = import.regions[1];
    CHECK(loud.zone.xfadeInLow == 60);
    CHECK(loud.zone.xfadeInHigh == 80);
    CHECK(loud.zone.tuneCents == 7.0F);

    // The release group: the second articulation, a region's own volume over the
    // global's, key= setting the range and the root.
    const auto& release = import.regions[2];
    CHECK(release.sample == "Samples/Violin/rel A4.wav");
    CHECK(release.zone.trigger == ZoneTrigger::Release);
    CHECK(release.zone.rtDecay == 6.0F);
    CHECK(release.zone.switchKey == 26);
    CHECK(release.zone.keyLow == 69);
    CHECK(release.zone.rootKey == 69);
    CHECK(release.zone.gainDb == 2.0F);

    // A new <group> drops the previous group's opcodes: no keyswitch here. The two
    // seq_ regions share one round-robin group, in order; SFZ's inclusive loop end
    // becomes exclusive.
    const auto& first = import.regions[3];
    const auto& second = import.regions[4];
    CHECK_FALSE(first.zone.hasSwitch());
    CHECK(first.zone.roundRobinGroup != 0);
    CHECK(first.zone.roundRobinGroup == second.zone.roundRobinGroup);
    CHECK(first.zone.roundRobinIndex == 0);
    CHECK(second.zone.roundRobinIndex == 1);
    CHECK(first.zone.loop == LoopMode::Forward);
    CHECK(first.zone.loopStart == 100);
    CHECK(first.zone.loopEnd == 1000);
    CHECK(first.zone.group == 1);
    CHECK(first.zone.offBy == 2);
    CHECK(second.zone.pan == -0.5F);
    // transpose and tune add.
    CHECK(import.regions[5].zone.tuneCents == 88.0F);
}

TEST_CASE("sfz_unknown_opcodes_are_reported", "[sfz]") {
    const Import import = parsed();
    // fil_type and cutoff are not read: listed with where they are, never dropped.
    REQUIRE(import.count(NoteKind::Unknown) == 2);
    const auto unknown =
        std::ranges::find(import.notes, NoteKind::Unknown, &adx::format::sfz::Note::kind);
    CHECK(unknown->opcode == "fil_type");
    CHECK(unknown->line == 13);
    CHECK(unknown->column == 32);
    // The label is understood and ignored, and says so.
    CHECK(import.count(NoteKind::Ignored) == 1);
    const std::string report = adx::format::sfz::reportOf(import);
    CHECK(report.find("13:32 unknown fil_type") != std::string::npos);
    CHECK(report.find("ignored sw_label") != std::string::npos);

    // An include that cannot be read is reported too.
    const Import missing = parseSfz("#include \"nowhere.sfz\"\n<region> sample=a.wav key=60\n");
    CHECK(missing.regions.size() == 1);
    REQUIRE(missing.notes.size() == 1);
    CHECK(missing.notes.front().opcode == "#include");
}

TEST_CASE("sfz_becomes_a_sampler_preset", "[sfz]") {
    Import import = parsed();
    const adx::preset::Preset preset = adx::preset::presetFromSfz(
        import, {.name = "Violin KS", .pack = "classical", .pathPrefix = "lib://demo/"});
    CHECK(preset.type == "sampler");
    REQUIRE(preset.zones.size() == 6);
    CHECK(preset.zones[0].path == "lib://demo/Samples/Violin/sus pp C4.wav");
    const auto release =
        std::ranges::find(preset.params, "env.release", &adx::project::ParamValue::name);
    REQUIRE(release != preset.params.end());
    CHECK(release->value == 0.4);

    // The text round-trips, zones and all.
    adx::format::DiagnosticList diagnostics;
    const std::string text = adx::preset::writePreset(preset);
    const adx::preset::Preset again = adx::preset::parsePreset(text, diagnostics);
    CHECK_FALSE(diagnostics.hasErrors());
    CHECK(again == preset);
}

TEST_CASE("library_paths_resolve_in_the_store", "[sfz][library]") {
    const fs::path store = fs::temp_directory_path() / "adx_library_store_test";
    fs::remove_all(store);
    fs::create_directories(store / "demo@0123456789ab" / "Strings");
    {
        std::ofstream(store / "demo@0123456789ab" / "Strings" / "a.wav") << "x";
    }

    CHECK(adx::format::libraryOf("lib://demo/Strings/a.wav") == "demo");
    CHECK(adx::format::libraryOf("Strings/a.wav").empty());
    CHECK(adx::format::resolveLibraryPath(store, "lib://demo/Strings/a.wav") ==
          store / "demo@0123456789ab" / "Strings" / "a.wav");
    // Not installed: nothing, not a guess.
    CHECK(adx::format::resolveLibraryPath(store, "lib://other/a.wav").empty());
    fs::remove_all(store);
}

TEST_CASE("lib_path_fallback", "[sfz][library]") {
    // A preset whose library is not installed still loads; what plays is its declared
    // fallback, and there is exactly one diagnostic saying why.
    const fs::path directory = fs::temp_directory_path() / "adx_fallback_test";
    fs::remove_all(directory);
    fs::create_directories(directory / "demo");
    {
        std::ofstream(directory / "demo" / "strings.adxpreset")
            << "[PRESET]\nNAME=\"Sampled Strings\"\nTYPE=sampler\nPACK=demo\n"
               "FALLBACK=\"demo/Synth Strings\"\n\n[ZONES]\n"
               "ZONE \"lib://adx-test-not-installed/Strings/a.wav\" root=60\n"
               "ZONE \"lib://adx-test-not-installed/Strings/b.wav\" root=72\n";
        std::ofstream(directory / "demo" / "synth.adxpreset")
            << "[PRESET]\nNAME=\"Synth Strings\"\nTYPE=va\nPACK=demo\n";
    }
    adx::format::DiagnosticList scan;
    adx::preset::PresetLibrary library;
    REQUIRE(library.scan(directory, scan) == 2);
    const adx::preset::Preset* sampled = library.find("Sampled Strings", "demo");
    REQUIRE(sampled != nullptr);
    CHECK(sampled->fallback == "demo/Synth Strings");
    CHECK(adx::preset::missingLibraries(*sampled) ==
          std::vector<std::string>{"adx-test-not-installed"});

    adx::format::DiagnosticList diagnostics;
    const adx::preset::Preset* plays = library.playable(*sampled, diagnostics);
    REQUIRE(plays != nullptr);
    CHECK(plays->name == "Synth Strings");
    CHECK(plays->type == "va");
    REQUIRE(diagnostics.all().size() == 1);
    CHECK(diagnostics.all().front().message.find("adx-test-not-installed") != std::string::npos);

    // A preset with nothing missing is itself, with no diagnostic.
    adx::format::DiagnosticList quiet;
    const adx::preset::Preset* synth = library.find("Synth Strings", "demo");
    REQUIRE(synth != nullptr);
    CHECK(library.playable(*synth, quiet) == synth);
    CHECK(quiet.all().empty());

    // And the zone itself resolves to no file: silence with a reason, not a wrong file.
    const adx::project::Resources resources;
    CHECK(adx::format::resolveSamplePath(resources, {}, sampled->zones.front().path).empty());
    fs::remove_all(directory);
}

TEST_CASE("sfz_import_vsco_vcsl", "[sfz][libraries]") {
    // Every SFZ the fetched libraries ship imports, every sample it names exists, and
    // nothing is unknown. Skipped where the library store is not populated (CI).
    const fs::path store = adx::format::libraryStore();
    std::size_t files = 0;
    std::size_t regions = 0;
    std::size_t missingFiles = 0;
    for (const char* id : {"vsco2ce", "vcsl", "salamander"}) {
        const fs::path folder = adx::format::libraryFolder(store, id);
        if (folder.empty()) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            if (entry.path().extension() != ".sfz") {
                continue;
            }
            INFO(entry.path().string());
            Import import;
            REQUIRE(adx::format::sfz::loadSfz(entry.path(), import));
            CHECK_FALSE(import.regions.empty());
            INFO(adx::format::sfz::reportOf(import));
            CHECK(import.count(NoteKind::Unknown) == 0);
            std::size_t missing = 0;
            for (const auto& region : import.regions) {
                missing += fs::is_regular_file(entry.path().parent_path() / region.sample) ? 0 : 1;
            }
            // VCSL at the pinned commit keeps the Psaltery's samples inside the Italian
            // harpsichord's folder, so its four mappings name files that are not there.
            // That is the library's, and the import tool reports it; anything else
            // missing is the parser's.
            if (entry.path().filename().string().starts_with("Psaltery")) {
                missingFiles += missing;
            } else {
                CHECK(missing == 0);
            }
            ++files;
            regions += import.regions.size();
        }
    }
    if (files == 0) {
        SKIP("no sound libraries in " << store.string());
    }
    WARN("imported " << files << " SFZ files, " << regions << " regions; " << missingFiles
                     << " samples missing upstream (VCSL Psaltery)");
}

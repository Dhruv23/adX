// Presets and the shipped packs (phase_4.md §4.12, §6).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/preset/Preset.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/project/TypeCatalog.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/effects/EffectHarness.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::format::DiagnosticList;
using adx::preset::Preset;
using adx::preset::PresetLibrary;

namespace {

std::string describe(const DiagnosticList& list) {
    std::string out;
    for (const adx::format::Diagnostic& d : list.all()) {
        out += d.codeString() + " " + d.message + '\n';
    }
    return out;
}

PresetLibrary shippedPacks(DiagnosticList& diagnostics) {
    PresetLibrary library;
    library.scan(adx::preset::packsDirectory(adx::tests::repoRoot()), diagnostics);
    return library;
}

} // namespace

TEST_CASE("preset_packs_load", "[preset]") {
    // phase_4.md §6: every C418 and STAKILLAZ preset loads with zero diagnostics - which,
    // because parameters are checked against each type's descriptor table, also means
    // every parameter name exists and every value is in range.
    DiagnosticList diagnostics;
    const PresetLibrary library = shippedPacks(diagnostics);
    INFO(describe(diagnostics));
    CHECK(diagnostics.empty());

    // The sounds docs/C418.md and docs/STAKILLAZ.md name, each a preset.
    for (const char* name : {"Kalimba", "Marimba", "Giant Piano", "Warm Pad", "Pizzicato"}) {
        INFO(name);
        CHECK(library.find(name, "c418") != nullptr);
    }
    for (const char* name : {"Hardstyle Kick", "808 Sub", "Screech Lead", "Phonk Bass"}) {
        INFO(name);
        CHECK(library.find(name, "stakillaz") != nullptr);
    }
    CHECK(library.inPack("c418").size() >= 7);
    CHECK(library.inPack("stakillaz").size() >= 7);

    // And each one plays: an instrument preset makes a finite, audible note; an effect
    // preset processes noise without blowing up.
    for (const Preset& preset : library.all()) {
        INFO(preset.name);
        if (preset.kind() == adx::project::TypeKind::Instrument) {
            std::vector<float> params = adx::tests::instrumentParams(preset.type);
            for (const adx::project::ParamValue& param : preset.params) {
                const std::uint32_t index =
                    adx::tests::instrumentParamIndex(preset.type, param.name);
                REQUIRE(index != adx::project::kNoParam);
                params[index] = static_cast<float>(param.value);
            }
            const auto node = adx::tests::preparedInstrument(preset.type);
            const auto run = adx::tests::runInstrument(
                *node, {adx::tests::noteOn(0, 1, 48), adx::tests::noteOff(12000, 1, 48)}, 24000,
                params);
            float peak = 0.0F;
            for (const float s : run.left) {
                REQUIRE(std::isfinite(s));
                peak = std::max(peak, std::abs(s));
            }
            CHECK(peak > 0.001F);
        } else {
            std::vector<float> params =
                adx::tests::slotParams(preset.type, static_cast<float>(preset.mix.value_or(1.0)));
            for (const adx::project::ParamValue& param : preset.params) {
                const std::uint32_t index = adx::tests::slotParamIndex(preset.type, param.name);
                REQUIRE(index != adx::project::kNoParam);
                params[index] = static_cast<float>(param.value);
            }
            const auto run = adx::tests::runEffect(*adx::tests::preparedEffect(preset.type),
                                                   adx::tests::noise(9U, 0.5F, 24000), params);
            for (const float s : run.out.left) {
                REQUIRE(std::isfinite(s));
                REQUIRE(std::abs(s) < 4.0F);
            }
        }
    }
}

TEST_CASE("preset_roundtrip", "[preset]") {
    // phase_4.md §6: every shipped preset loads, saves and reloads identically, and the
    // writer is idempotent.
    DiagnosticList diagnostics;
    const PresetLibrary library = shippedPacks(diagnostics);
    REQUIRE_FALSE(library.all().empty());
    for (const Preset& preset : library.all()) {
        INFO(preset.name);
        const std::string text = adx::preset::writePreset(preset);
        DiagnosticList again;
        const Preset reloaded = adx::preset::parsePreset(text, again);
        CHECK(again.empty());
        CHECK(reloaded == preset);
        CHECK(adx::preset::writePreset(reloaded) == text);
    }
}

TEST_CASE("preset_parse_reports_problems", "[preset]") {
    const std::string text = "[PRESET]\nNAME=\"Odd\"\nTYPE=additive\nCOLOUR=red\n\n"
                             "[PARAMS]\nenv.attack=99\nnot.a.param=1\nharmonic.2=0.5\n\n[EXTRA]\n";
    DiagnosticList diagnostics;
    const Preset preset = adx::preset::parsePreset(text, diagnostics);
    // Still a preset, with every parameter kept: unknown is a warning, not a refusal.
    CHECK(preset.name == "Odd");
    CHECK(preset.params.size() == 3);
    std::vector<std::uint16_t> codes;
    for (const adx::format::Diagnostic& d : diagnostics.all()) {
        codes.push_back(d.code);
    }
    CHECK(std::ranges::count(codes, adx::format::code::kUnknownKey) == 1);       // COLOUR
    CHECK(std::ranges::count(codes, adx::format::code::kValueOutOfRange) == 1);  // env.attack=99
    CHECK(std::ranges::count(codes, adx::format::code::kUnknownParameter) == 1); // not.a.param
    CHECK(std::ranges::count(codes, adx::format::code::kUnknownSection) == 1);   // [EXTRA]
}

TEST_CASE("preset_library_search_and_user_presets", "[preset]") {
    DiagnosticList diagnostics;
    PresetLibrary library = shippedPacks(diagnostics);

    CHECK(library.withTag("pluck").size() >= 3);
    // Every word must match some field: "808 Sub" is tagged phonk and bass, so it is a
    // hit too, and results come in name order.
    const auto bass = library.search("phonk bass");
    CHECK(std::ranges::any_of(bass, [](const Preset* p) { return p->name == "Phonk Bass"; }));
    CHECK(std::ranges::none_of(bass, [](const Preset* p) { return p->name == "Kalimba"; }));
    CHECK(!library.search("REVERB").empty()); // case-insensitive, matches the type
    const std::vector<std::string> tags = library.tags();
    CHECK(std::ranges::is_sorted(tags));
    CHECK(std::ranges::adjacent_find(tags) == tags.end());

    // A user preset is saved apart from the packs and shadows nothing it should not.
    const std::filesystem::path user = std::filesystem::temp_directory_path() / "adx_user_presets";
    std::filesystem::remove_all(user);
    library.setUserDirectory(user);
    Preset mine = *library.find("Kalimba", "c418");
    mine.params.push_back(
        adx::project::ParamValue{.name = "drive", .value = 3.0, .hasCurve = false, .curve = {}});
    REQUIRE(library.save(mine));
    CHECK(std::filesystem::exists(user / "c418" / "Kalimba.adxpreset"));
    // The library now answers with the user's version...
    CHECK(library.find("Kalimba", "c418")->params.size() == mine.params.size());
    // ...and a fresh library that scans packs then the user directory agrees.
    PresetLibrary fresh = shippedPacks(diagnostics);
    fresh.scan(user, diagnostics, true);
    CHECK(fresh.find("Kalimba", "c418")->params.size() == mine.params.size());
    // A pack rescan does not undo the user's edit.
    fresh.scan(adx::preset::packsDirectory(adx::tests::repoRoot()), diagnostics);
    CHECK(fresh.find("Kalimba", "c418")->params.size() == mine.params.size());
}

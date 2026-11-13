// SF2 and DLS banks and the `gm` player (phase_12.md §4.7.4, §4.7.5, §5).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/format/soundfont/Generators.h"
#include "engine/format/soundfont/SoundFont.h"
#include "engine/instruments/gm/GmSetup.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

namespace fs = std::filesystem;
namespace sf = adx::format::soundfont;
using adx::tests::noteOff;
using adx::tests::noteOn;

namespace {

fs::path banks() {
    return adx::tests::repoRoot() / "tests" / "data" / "banks";
}

sf::Bank load(const fs::path& path) {
    sf::Bank bank;
    std::string error;
    INFO(error);
    REQUIRE(sf::loadBank(path, bank, error));
    return bank;
}

std::vector<std::byte> bytesOf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

/// A gm node on `bank`, prepared at 48 kHz.
std::shared_ptr<adx::instruments::GmInstrument> gmNode(const fs::path& bank,
                                                       std::uint16_t polyphony = 8) {
    auto node = std::make_shared<adx::instruments::GmInstrument>(
        1, polyphony, adx::project::VoiceStealMode::OldestReleased);
    std::string error;
    adx::instruments::setGmBank(*node, adx::instruments::GmBankCache::get(bank, error), bank);
    node->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    return node;
}

std::vector<float> gmParams(std::initializer_list<std::pair<std::string_view, float>> values) {
    std::vector<float> params = adx::tests::instrumentParams("gm");
    for (const auto& [name, value] : values) {
        params[adx::tests::instrumentParamIndex("gm", name)] = value;
    }
    return params;
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return 10.0 * std::log10((sum / static_cast<double>(to - from)) + 1e-30);
}

double frequency(const std::vector<float>& x, std::size_t from, std::size_t to) {
    std::vector<double> crossings;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0F && x[i] >= 0.0F) {
            crossings.push_back(static_cast<double>(i - 1) +
                                (static_cast<double>(-x[i - 1]) / (x[i] - x[i - 1])));
        }
    }
    REQUIRE(crossings.size() > 2);
    return 48000.0 * static_cast<double>(crossings.size() - 1) /
           (crossings.back() - crossings.front());
}

} // namespace

TEST_CASE("sf2_generator_units", "[soundfont]") {
    CHECK(sf::timecentsToSeconds(0) == 1.0);
    CHECK(std::abs(sf::timecentsToSeconds(1200) - 2.0) < 1e-12);
    CHECK(std::abs(sf::timecentsToSeconds(-1200) - 0.5) < 1e-12);
    CHECK(sf::timecentsToSeconds(-12000) == 0.0);
    CHECK(sf::timecentsToSeconds(-32768) == 0.0);
    CHECK(std::abs(sf::absoluteCentsToHz(6900) - 440.0) < 0.01);
    CHECK(std::abs(sf::absoluteCentsToHz(13500) - 19912.1) < 0.5);
    CHECK(sf::centibelsToDb(120) == 12.0);
    CHECK(sf::generatorDefault(static_cast<std::uint16_t>(sf::Gen::InitialFilterFc)) == 13500);
    CHECK(sf::generatorDefault(static_cast<std::uint16_t>(sf::Gen::ScaleTuning)) == 100);
    CHECK_FALSE(sf::presetAdditive(static_cast<std::uint16_t>(sf::Gen::SampleId)));
    CHECK(sf::presetAdditive(static_cast<std::uint16_t>(sf::Gen::AttackVolEnv)));
}

TEST_CASE("sf2_preset_adds_instrument", "[soundfont]") {
    const sf::Bank bank = load(banks() / "tiny_gm.sf2");
    CHECK(bank.name == "adX tiny GM");
    REQUIRE(bank.presets.size() == 2);
    const sf::BankPreset* sine = bank.find(0, 0);
    REQUIRE(sine != nullptr);
    CHECK(sine->name == "Sine");
    REQUIRE(sine->zones.size() == 1);
    const sf::BankZone& z = sine->zones.front();
    CHECK(z.rootKey == 69);
    CHECK(std::abs(z.volume.attack - 0.05F) < 1e-3F);
    CHECK(std::abs(z.volume.hold - 0.1F) < 1e-3F);
    CHECK(std::abs(z.volume.decay - 0.4F) < 1e-3F);
    CHECK(std::abs(z.volume.release - 0.3F) < 1e-3F);
    CHECK(std::abs(z.volume.sustain - static_cast<float>(std::pow(10.0, -12.0 / 20.0))) < 1e-4F);
    CHECK(std::abs(z.vibrato.hz - 5.0F) < 0.01F);
    CHECK(z.vibrato.toPitch == 20.0F);
    CHECK(std::abs(z.filterHz - 8000.0F) < 5.0F);

    const sf::BankPreset* kit = bank.find(sf::Bank::kDrumBank, 0);
    REQUIRE(kit != nullptr);
    REQUIRE(kit->zones.size() == 2);
    CHECK(kit->zones[0].keyLow == 42);
    CHECK(kit->zones[0].exclusiveClass == 1);
    CHECK(kit->zones[1].keyLow == 46);
    CHECK(std::abs(kit->zones[1].volume.release - 0.5F) < 1e-3F);
    // An unknown program falls back to bank 0; a missing kit to the first kit.
    CHECK(bank.find(0, 77) == sine);
    CHECK(bank.find(3, 0) == sine);
}

TEST_CASE("sf2_loop_modes", "[soundfont]") {
    const sf::Bank bank = load(banks() / "tiny_gm.sf2");
    const sf::BankZone& z = bank.find(0, 0)->zones.front();
    CHECK(z.loop == sf::BankLoop::Continuous);
    CHECK(z.loopStart == 0);
    CHECK(z.loopEnd == 10000);
    CHECK(z.end == 10000);
    CHECK(bank.find(sf::Bank::kDrumBank, 0)->zones.front().loop == sf::BankLoop::None);
}

TEST_CASE("dls_parse_and_map", "[soundfont]") {
    // The DLS fixture describes what the SF2 one does; it maps to the same articulation.
    const sf::Bank sf2 = load(banks() / "tiny_gm.sf2");
    const sf::Bank dls = load(banks() / "tiny_gm.dls");
    REQUIRE(dls.presets.size() == 2);
    const sf::BankZone& a = sf2.find(0, 0)->zones.front();
    const sf::BankZone& b = dls.find(0, 0)->zones.front();
    CHECK(b.rootKey == a.rootKey);
    CHECK(std::abs(b.volume.attack - a.volume.attack) < 1e-3F);
    CHECK(std::abs(b.volume.hold - a.volume.hold) < 1e-3F);
    CHECK(std::abs(b.volume.decay - a.volume.decay) < 1e-3F);
    CHECK(std::abs(b.volume.sustain - a.volume.sustain) < 1e-3F);
    CHECK(std::abs(b.volume.release - a.volume.release) < 1e-3F);
    CHECK(std::abs(b.vibrato.hz - a.vibrato.hz) < 0.01F);
    CHECK(std::abs(b.vibrato.delay - a.vibrato.delay) < 1e-3F);
    CHECK(b.vibrato.toPitch == a.vibrato.toPitch);
    CHECK(std::abs(b.filterHz - a.filterHz) < 5.0F);
    CHECK(b.loop == sf::BankLoop::Continuous);
    CHECK(b.loopEnd == a.loopEnd);
    const sf::BankPreset* kit = dls.find(sf::Bank::kDrumBank, 0);
    REQUIRE(kit != nullptr);
    REQUIRE(kit->zones.size() == 2);
    CHECK(kit->zones[0].exclusiveClass == 1);
    CHECK(std::abs(kit->zones[1].volume.release - 0.5F) < 1e-3F);
    CHECK(sf2.samples[a.sample].data == dls.samples[b.sample].data);
}

TEST_CASE("soundfont_fuzz", "[soundfont]") {
    // Mutated and truncated banks: never a crash, never a read past the end, and every
    // rejection says why. (10k under ASan in the full check; 500 here per format.)
    for (const char* name : {"tiny_gm.sf2", "tiny_gm.dls"}) {
        const std::vector<std::byte> original = bytesOf(banks() / name);
        std::mt19937 random(12345U);
        for (int round = 0; round < 500; ++round) {
            std::vector<std::byte> mutated = original;
            const int edits = 1 + static_cast<int>(random() % 8U);
            for (int e = 0; e < edits; ++e) {
                const std::size_t at = random() % mutated.size();
                mutated[at] = static_cast<std::byte>(random() & 0xFFU);
            }
            if (round % 5 == 0) {
                mutated.resize(random() % mutated.size());
            }
            sf::Bank bank;
            std::string error;
            const bool ok = std::string_view(name).ends_with(".sf2")
                                ? sf::parseSf2(mutated, bank, error)
                                : sf::parseDls(mutated, bank, error);
            if (!ok) {
                CHECK_FALSE(error.empty());
            }
            for (const sf::BankPreset& preset : bank.presets) {
                for (const sf::BankZone& zone : preset.zones) {
                    REQUIRE(zone.sample < bank.samples.size());
                    CHECK(zone.end <= bank.samples[zone.sample].data.size());
                }
            }
        }
    }
}

TEST_CASE("soundfont_unknown_reported", "[soundfont]") {
    // An SF3 (compressed) sample is named in the report, not silently missing.
    std::vector<std::byte> bytes = bytesOf(banks() / "tiny_gm.sf2");
    // shdr's first record's sampleType is its last two bytes: set the compressed bit.
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::size_t shdr = text.find("shdr");
    REQUIRE(shdr != std::string_view::npos);
    bytes[shdr + 8 + 44] = static_cast<std::byte>(0x11);
    sf::Bank bank;
    std::string error;
    REQUIRE(sf::parseSf2(bytes, bank, error));
    CHECK(sf::reportOf(bank).find("SF3") != std::string::npos);
}

TEST_CASE("gm_plays_in_tune_with_its_envelope", "[soundfont][gm]") {
    for (const char* name : {"tiny_gm.sf2", "tiny_gm.dls"}) {
        INFO(name);
        const auto node = gmNode(banks() / name);
        const auto run = adx::tests::runInstrument(
            *node, {noteOn(0, 1, 69, 127), noteOff(48000, 1, 69)}, 72000, gmParams({}));
        CHECK(run.violations == 0);
        // Before the vibrato's 0.2 s delay: exactly the sample's 441 Hz, at 48 kHz output.
        CHECK(std::abs(1200.0 * std::log2(frequency(run.left, 1000, 9000) / 441.0)) < 1.0);
        // The sustain sits 12 dB under the hold's peak.
        const double hold = rmsDb(run.left, 2400 + 1200, 2400 + 4800);
        const double sustain = rmsDb(run.left, 36000, 46000);
        CHECK(std::abs((hold - sustain) - 12.0) < 1.0);
        // Released at 1 s with 0.3 s of release: 96 dB down by 1.3 s.
        CHECK(rmsDb(run.left, 64000, 72000) < hold - 80.0);
        CHECK(run.sounding.back() == 0);
    }
}

TEST_CASE("sf2_exclusive_class", "[soundfont][gm]") {
    // The closed hat (42) cuts the ringing open hat (46) within a block.
    const auto node = gmNode(banks() / "tiny_gm.sf2");
    const auto params = gmParams({{"drums", 1.0F}});
    const auto both = adx::tests::runInstrument(
        *node, {noteOn(0, 1, 46, 120), noteOn(9600, 2, 42, 120)}, 19200, params);
    const auto open = adx::tests::runInstrument(*gmNode(banks() / "tiny_gm.sf2"),
                                                {noteOn(0, 1, 46, 120)}, 19200, params);
    const auto closed = adx::tests::runInstrument(*gmNode(banks() / "tiny_gm.sf2"),
                                                  {noteOn(9600, 2, 42, 120)}, 19200, params);
    // From 2 ms after the closed hat, the mix is the closed hat alone.
    double worst = 0.0;
    for (std::size_t i = 9600 + 200; i < 19200; ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(both.left[i] - closed.left[i])));
    }
    CHECK(worst < 1e-6);
    CHECK(rmsDb(open.left, 12000, 19000) > -60.0); // left alone, it was still ringing
}

TEST_CASE("gm_program_change", "[soundfont][gm]") {
    // A program change between notes changes the next note; one ringing keeps its zones.
    const auto node = gmNode(banks() / "tiny_gm.sf2");
    const std::uint32_t drums = adx::tests::instrumentParamIndex("gm", "drums");
    std::vector<float> params = gmParams({});
    // Note 1 on the melodic bank; then drums on; note 2 on the kit.
    const auto run = adx::tests::runInstrument(*node, {noteOn(0, 1, 46, 120)}, 4800, params);
    params[drums] = 1.0F;
    const auto second = adx::tests::runInstrument(*node, {noteOn(0, 2, 46, 120)}, 9600, params);
    // The first note is the sine (441 Hz pitched to 46: ~116.5 Hz); still the sine while
    // the second plays, which is noise.
    CHECK(std::abs(1200.0 * std::log2(frequency(run.left, 0, 4800) /
                                      (441.0 * std::pow(2.0, (46 - 69) / 12.0)))) < 2.0);
    CHECK(rmsDb(second.left, 0, 4800) > -40.0);
}

TEST_CASE("gm_bank_shared", "[soundfont][gm]") {
    // Twenty gm channels on one bank hold it once.
    std::vector<std::shared_ptr<adx::instruments::GmInstrument>> nodes;
    nodes.reserve(20);
    for (int i = 0; i < 20; ++i) {
        nodes.push_back(gmNode(banks() / "tiny_gm.sf2"));
    }
    for (const auto& node : nodes) {
        CHECK(adx::instruments::gmBankOf(*node) == adx::instruments::gmBankOf(*nodes.front()));
    }
}

TEST_CASE("dls_system_gm", "[soundfont][gm][libraries]") {
    // The Windows GS bank: 128 programs and a kit, C4 on each one sounding.
    const fs::path path = sf::systemGmBank();
    if (path.empty()) {
        SKIP("no gm.dls on this machine");
    }
    const sf::Bank bank = load(path);
    INFO(sf::reportOf(bank));
    std::size_t melodic = 0;
    for (const sf::BankPreset& preset : bank.presets) {
        melodic += preset.bank == 0 ? 1 : 0;
    }
    CHECK(melodic == 128);
    CHECK(bank.find(sf::Bank::kDrumBank, 0) != nullptr);
    const auto node = gmNode(path, 4);
    for (int program = 0; program < 128; program += 1) {
        INFO("program " << program);
        const auto run =
            adx::tests::runInstrument(*node,
                                      {noteOn(0, static_cast<std::uint32_t>(program + 1), 60, 100),
                                       noteOff(9600, static_cast<std::uint32_t>(program + 1), 60)},
                                      12000, gmParams({{"program", static_cast<float>(program)}}));
        CHECK(rmsDb(run.left, 0, 9600) > -70.0);
    }
}

TEST_CASE("gm_dls_never_copied", "[soundfont][gm]") {
    // A project on the Windows bank names it; saving writes the name, never the bytes.
    const std::string text =
        "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL Piano]\nINSTRUMENT=gm\nBANK=\"system:gm.dls\"\n"
        "OUTPUT=insert.1\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(text, project, stack, diagnostics);
    REQUIRE(project.channels.front().instrument.voicebank == "system:gm.dls");
    const std::string saved = adx::format::write(project);
    CHECK(saved.find("BANK=\"system:gm.dls\"") != std::string::npos);
    CHECK(saved.size() < 1000);
    CHECK(project.resources.samples.empty());
}

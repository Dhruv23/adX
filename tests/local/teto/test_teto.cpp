// Local acceptance tests against Kasane Teto (phase_4.md §4.13). The bank's licence
// forbids redistributing it, so it is never in the repo or in CI: these run only where
// ADX_TETO_DIR points at the extracted bank, and are skipped - not failed - elsewhere.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/instruments/voice/UtauResampler.h"
#include "engine/instruments/voice/VoiceSetup.h"
#include "engine/instruments/voice/Voicebank.h"
#include "engine/instruments/voice/WorldAnalysis.h"
#include "tests/cpp/Env.h"

namespace {

std::filesystem::path tetoDir() {
    const std::string dir = adx::tests::environment("ADX_TETO_DIR").value_or("");
    if (dir.empty()) {
        return {};
    }
    std::u8string u8;
    for (const char c : dir) {
        u8.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path{u8};
}

#define REQUIRE_TETO()                                                                             \
    const std::filesystem::path teto = tetoDir();                                                  \
    if (teto.empty()) {                                                                            \
        SKIP("ADX_TETO_DIR is not set");                                                           \
    }

/// F0 from the autocorrelation peak within half an octave of `expected`.
double pitchOf(const std::vector<float>& x, std::size_t from, std::size_t length, double rate,
               double expected) {
    double best = 0.0;
    std::size_t bestLag = 1;
    const auto at = [&](std::size_t lag) {
        double sum = 0.0;
        for (std::size_t i = from; i < from + length; ++i) {
            sum += static_cast<double>(x[i]) * x[i + lag];
        }
        return sum;
    };
    for (auto lag = static_cast<std::size_t>(rate / (expected * 1.41));
         lag < static_cast<std::size_t>(rate / (expected / 1.41)); ++lag) {
        const double sum = at(lag);
        if (sum > best) {
            best = sum;
            bestLag = lag;
        }
    }
    const double a = at(bestLag - 1);
    const double b = at(bestLag);
    const double c = at(bestLag + 1);
    return rate / (static_cast<double>(bestLag) + (0.5 * (a - c) / (a - (2.0 * b) + c)));
}

} // namespace

TEST_CASE("teto_loads_all_three_subbanks", "[voice][teto]") {
    REQUIRE_TETO();
    adx::instruments::Voicebank bank;
    std::vector<std::string> warnings;
    REQUIRE(bank.load(teto, warnings));
    for (const std::string& w : warnings) {
        UNSCOPED_INFO(w);
    }
    CHECK(warnings.empty()); // every line parses, every referenced WAV exists
    std::vector<std::size_t> lines;
    for (const adx::instruments::SubBank& sub : bank.subBanks()) {
        lines.push_back(sub.lines);
    }
    std::ranges::sort(lines);
    CHECK(lines == std::vector<std::size_t>{39, 319, 887});
    CHECK(bank.name().find("重音テト") != std::string::npos);
}

TEST_CASE("teto_vcv_phrase_resolves", "[voice][teto]") {
    REQUIRE_TETO();
    const auto bank = adx::instruments::loadVoicebank(teto);
    REQUIRE(bank != nullptr);
    const std::vector<std::string> lyrics{"あ", "い", "う", "え", "お"};
    const std::vector<std::string> expected{"- あ", "a い", "i う", "u え", "e お"};
    std::string previous;
    for (std::size_t n = 0; n < lyrics.size(); ++n) {
        const auto* entry = adx::instruments::resolveAlias(*bank, lyrics[n], previous);
        REQUIRE(entry != nullptr);
        CHECK(entry->alias == expected[n]);
        previous = lyrics[n];
    }
}

TEST_CASE("teto_frq_matches_analysis", "[voice][teto]") {
    REQUIRE_TETO();
    const auto bank = adx::instruments::loadVoicebank(teto);
    REQUIRE(bank != nullptr);
    for (const char* alias : {"あ", "か", "a い"}) {
        INFO(alias);
        const auto* entry = bank->find(alias);
        REQUIRE(entry != nullptr);
        const auto analysis = adx::instruments::analyseWav(entry->wav);
        REQUIRE(analysis != nullptr);
        CHECK(analysis->fromFrq);
        const std::vector<double> harvest =
            adx::instruments::harvestF0(analysis->samples, analysis->sampleRate);
        std::size_t voiced = 0;
        std::size_t agreed = 0;
        for (std::size_t i = 0; i < harvest.size() && i < analysis->f0.size(); ++i) {
            if (harvest[i] > 0.0 && analysis->f0[i] > 0.0) {
                ++voiced;
                agreed += std::abs(1200.0 * std::log2(harvest[i] / analysis->f0[i])) < 30.0 ? 1 : 0;
            }
        }
        REQUIRE(voiced > 20);
        CHECK(agreed >= voiced * 9 / 10);
    }
}

TEST_CASE("teto_pitch_sweep_no_artifacts", "[voice][teto]") {
    REQUIRE_TETO();
    const auto bank = adx::instruments::loadVoicebank(teto);
    REQUIRE(bank != nullptr);
    const auto* entry = adx::instruments::resolveAlias(*bank, "あ", "");
    REQUIRE(entry != nullptr);
    // D#4 +- 12 semitones, every third.
    for (int semitones = -12; semitones <= 12; semitones += 3) {
        INFO(semitones << " semitones from D#4");
        adx::instruments::NoteRenderRequest request;
        request.bank = bank;
        request.oto = *entry;
        request.noteHz = 311.127 * std::exp2(semitones / 12.0);
        request.lengthMs = 600.0;
        request.cutMs = 600.0;
        request.cents.assign(130, 0.0F);
        const auto render = adx::instruments::renderNote(request);
        REQUIRE(render.ok);
        float peak = 0.0F;
        for (const float s : render.samples) {
            REQUIRE(std::isfinite(s));
            peak = std::max(peak, std::abs(s));
        }
        CHECK(peak <= 1.0F);
        const double measured =
            pitchOf(render.samples, render.lead + 14400, 4800, adx::instruments::kVoiceRenderRate,
                    request.noteHz);
        CHECK(std::abs(1200.0 * std::log2(measured / request.noteHz)) < 10.0);
    }
}

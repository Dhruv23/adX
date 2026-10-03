// What each instrument costs per voice (hidden: run it on purpose, in Release).
//
//   adx_tests "[.perf]"
//
// Not a gate - a measurement for the record (phase_4.md §11) and for Phase 5's CPU
// budget. Sixteen held voices of each type at its defaults, ten seconds of audio in
// 256-frame blocks; reported as the percentage of one core one voice takes at 48 kHz.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

#include "engine/project/TypeCatalog.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

TEST_CASE("instrument_cpu_cost", "[.perf]") {
    constexpr std::uint32_t kVoices = 16;
    constexpr std::uint32_t kFrames = 48000 * 10;
    for (const adx::project::TypeInfo& type : adx::project::instrumentTypes()) {
        std::vector<adx::graph::BlockEvent> events;
        events.reserve(kVoices);
        for (std::uint32_t v = 0; v < kVoices; ++v) {
            events.push_back(adx::tests::noteOn(0, v + 1, static_cast<std::uint8_t>(40 + (v * 2))));
        }
        const auto node = adx::tests::preparedInstrument(type.name, kVoices);
        const auto start = std::chrono::steady_clock::now();
        const adx::tests::InstrumentRun run = adx::tests::runInstrument(
            *node, events, kFrames, adx::tests::instrumentParams(type.name), 256);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double audioSeconds = static_cast<double>(kFrames) / 48000.0;
        const double percentPerVoice = 100.0 * seconds / audioSeconds / kVoices;
        WARN(type.name << ": " << percentPerVoice << " % of a core per voice ("
                       << run.sounding.back() << " voices sounding at the end)");
        CHECK(run.violations == 0);
    }
}

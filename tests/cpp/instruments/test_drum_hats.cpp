// DrumSynth's hat pair (phase_13.md §4.2): the open hat's own decay, and a closed hit
// choking an open one, so a kit's hats can live on one layer.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string_view>
#include <vector>

#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::tests::noteOn;

namespace {

std::vector<float> hats(std::initializer_list<std::pair<std::string_view, float>> values,
                        std::vector<adx::graph::BlockEvent> events) {
    std::vector<float> params = adx::tests::instrumentParams("drumsynth");
    params[adx::tests::instrumentParamIndex("drumsynth", "model")] = 5.0F; // Kit
    for (const auto& [name, value] : values) {
        params[adx::tests::instrumentParamIndex("drumsynth", name)] = value;
    }
    const auto run = adx::tests::runInstrument(*adx::tests::preparedInstrument("drumsynth"),
                                               std::move(events), 48000, params);
    CHECK(run.violations == 0);
    return run.left;
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return 10.0 * std::log10((s / static_cast<double>(to - from)) + 1e-24);
}

} // namespace

TEST_CASE("drum_hat_pair", "[drumsynth]") {
    // decay 0.05 for the closed hat; the open hat rings 0.5 s when told to.
    const auto shared = hats({{"decay", 0.05F}}, {noteOn(0, 1, 46, 110)});
    const auto open = hats({{"decay", 0.05F}, {"hat.openDecay", 0.5F}}, {noteOn(0, 1, 46, 110)});
    CHECK(rmsDb(open, 9600, 14400) > rmsDb(shared, 9600, 14400) + 30.0);
    // The closed hat itself is unchanged by the open hat's decay.
    const auto closedA = hats({{"decay", 0.05F}}, {noteOn(0, 1, 42, 110)});
    const auto closedB = hats({{"decay", 0.05F}, {"hat.openDecay", 0.5F}}, {noteOn(0, 1, 42, 110)});
    CHECK(closedA == closedB);
    // Choke: a closed hit at 100 ms cuts the ringing open hat; without it, it rings on.
    const std::vector<adx::graph::BlockEvent> pair{noteOn(0, 1, 46, 110), noteOn(4800, 2, 42, 110)};
    const auto ringing = hats({{"decay", 0.05F}, {"hat.openDecay", 0.5F}}, pair);
    const auto choked =
        hats({{"decay", 0.05F}, {"hat.openDecay", 0.5F}, {"hat.choke", 1.0F}}, pair);
    CHECK(rmsDb(choked, 14400, 19200) < rmsDb(ringing, 14400, 19200) - 30.0);
}

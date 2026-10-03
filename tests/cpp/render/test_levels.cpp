// P3-7: one call returns every strip's latest meter reading (phase_5.md §4.9 counts
// FFI calls per meter frame, and the answer must be exactly one).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "engine/audio/NullBackend.h"
#include "engine/render/RenderEngine.h"
#include "tests/cpp/render/RenderFixtures.h"

TEST_CASE("levels_reads_every_strip_in_one_call", "[render][meter]") {
    // Three inserts: the master, and two that feed it.
    adx::project::Project project = adx::tests::toneProject(
        {{.start = 0, .length = adx::core::kPpq * 16, .pitch = 69, .velocity = 100}});
    const adx::core::InsertId master = project.mixer.master;
    for (int i = 0; i < 2; ++i) {
        adx::project::Insert insert;
        insert.id = project.newInsertId();
        insert.name = "Bus" + std::to_string(i);
        project.mixer.inserts.push_back(insert);
        project.mixer.routes.push_back(
            adx::project::Route{.id = project.newRouteId(), .from = insert.id, .to = master});
    }
    project.channels.front().output = project.mixer.inserts[1].id;

    adx::render::RenderEngine engine{std::make_unique<adx::audio::NullBackend>(),
                                     adx::render::EngineOptions{}};
    REQUIRE(engine.open().code == adx::audio::Error::Code::None);
    REQUIRE(engine.setProject(project, 1).rebuilt);
    engine.setLoop(adx::transport::LoopRegion{});
    engine.seek(adx::core::Ticks{0});
    engine.play();
    REQUIRE(engine.start().code == adx::audio::Error::Code::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    engine.pump();

    std::vector<adx::render::RenderEngine::StripLevel> strips;
    engine.levels(strips);
    engine.stop();

    REQUIRE(strips.size() == 3);
    for (const auto& strip : strips) {
        INFO("insert " << strip.insert.value);
        const bool isMaster = strip.insert == master;
        const bool hasSignal = isMaster || strip.insert == project.mixer.inserts[1].id;
        if (hasSignal) {
            CHECK(strip.frame.peakLeft > 0.0F);
            CHECK(strip.frame.momentary > -60.0F); // 600 ms of tone: a momentary reading
        } else {
            CHECK(strip.frame.peakLeft == 0.0F);
            CHECK(strip.frame.momentary == -200.0F);
        }
        // True peak is the master's alone.
        CHECK((strip.frame.truePeak > 0.0F) == isMaster);
    }

    // A second read reuses the caller's storage: a steady 60 Hz reader does not allocate.
    const auto* before = strips.data();
    engine.levels(strips);
    CHECK(strips.data() == before);
}

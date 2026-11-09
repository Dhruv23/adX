// dx7_engine_matches_msfa (phase_11.5.md §5): the port is bit-identical to unmodified
// upstream msfa, block by block, for every fixture voice: all 32 algorithms, every
// field at its extremes, random voices; single notes at three velocities, a chord with
// sustain, pitch bend, the LFO on and off; at the engine rate and at the FM-1's.
//
// Upstream has no amplitude modulation, applies no transpose and bends a fixed three
// semitones. The comparison therefore runs both at those settings: AMD 0, the bend
// range 3, and the transpose applied to the note handed to upstream. adX's additions
// are tested on their own (test_dx7_instrument.cpp).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/instruments/dx7/Dx7Engine.h"
#include "engine/midi/dx7/Dx7Codec.h"
#include "tests/cpp/dx7/MsfaOracle.h"

using adx::dx7::Dx7Engine;
using adx::dx7::Dx7Function;
using adx::dx7::Dx7Voice;
using adx::test::OracleEvent;

namespace {

OracleEvent ev(int block, OracleEvent::Kind kind, int note, int velocity, int value) {
    return OracleEvent{
        .block = block, .kind = kind, .note = note, .velocity = velocity, .value = value};
}

std::vector<std::uint8_t> fixture(const char* name) {
    const std::filesystem::path path =
        std::filesystem::path(ADX_REPO_ROOT) / "tests" / "fixtures" / "dx7" / name;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<Dx7Voice> fixtureVoices() {
    std::vector<Dx7Voice> all;
    for (const char* name : {"algorithms.syx", "extremes.syx", "random.syx"}) {
        const auto result = adx::dx7::readVoices(fixture(name));
        REQUIRE(result.voices.size() == 32);
        all.insert(all.end(), result.voices.begin(), result.voices.end());
    }
    return all;
}

/// The port, driven as the oracle is.
std::vector<std::int32_t> portRender(double rate, const Dx7Voice& voice,
                                     const std::vector<OracleEvent>& events, int blocks) {
    Dx7Engine engine;
    engine.prepare(rate);
    Dx7Function function;
    function.bendRange = 3; // upstream's fixed range
    function.polyphony = Dx7Engine::kMaxVoices;
    engine.setFunction(function);
    engine.setVoice(voice);
    engine.reset();
    std::vector<std::int32_t> out(static_cast<std::size_t>(blocks) * 64);
    std::size_t next = 0;
    for (int block = 0; block < blocks; ++block) {
        while (next < events.size() && events[next].block <= block) {
            const OracleEvent& e = events[next++];
            switch (e.kind) {
            case OracleEvent::Kind::On:
                engine.noteOn(e.note, e.velocity, static_cast<std::uint64_t>(e.note));
                break;
            case OracleEvent::Kind::Off:
                engine.noteOff(static_cast<std::uint64_t>(e.note));
                break;
            case OracleEvent::Kind::Sustain:
                engine.setSustain(e.value != 0);
                break;
            case OracleEvent::Kind::Bend:
                engine.setPitchBend(e.value);
                break;
            }
        }
        engine.render(out.data() + (static_cast<std::size_t>(block) * 64));
    }
    return out;
}

/// Upstream sees the note the port plays after its transpose.
std::vector<OracleEvent> forUpstream(std::vector<OracleEvent> events, const Dx7Voice& voice) {
    const int shift = static_cast<int>(voice.bytes[adx::dx7::kTranspose]) - 24;
    for (OracleEvent& e : events) {
        if (e.kind == OracleEvent::Kind::On || e.kind == OracleEvent::Kind::Off) {
            e.note = std::clamp(e.note + shift, 0, 127);
        }
    }
    return events;
}

struct Scenario {
    std::string name;
    std::vector<OracleEvent> events;
    int blocks;
    bool lfo;
};

std::vector<Scenario> scenarios() {
    using K = OracleEvent::Kind;
    std::vector<Scenario> out;
    for (const int velocity : {1, 64, 127}) {
        out.push_back({.name = "note v" + std::to_string(velocity),
                       .events = {ev(0, K::On, 60, velocity, 0), ev(50, K::Off, 60, 0, 0)},
                       .blocks = 140,
                       .lfo = true});
    }
    out.push_back(
        {.name = "chord + sustain",
         .events = {ev(0, K::On, 48, 90, 0), ev(0, K::On, 55, 80, 0), ev(2, K::On, 64, 100, 0),
                    ev(3, K::On, 67, 70, 0), ev(10, K::Sustain, 0, 0, 1), ev(20, K::Off, 48, 0, 0),
                    ev(20, K::Off, 55, 0, 0), ev(25, K::Off, 64, 0, 0), ev(30, K::Off, 67, 0, 0),
                    ev(60, K::Sustain, 0, 0, 0), ev(70, K::On, 72, 110, 0),
                    ev(90, K::Off, 72, 0, 0)},
         .blocks = 160,
         .lfo = true});
    out.push_back({.name = "bend",
                   .events = {ev(0, K::On, 57, 100, 0), ev(8, K::Bend, 0, 0, 0x3000),
                              ev(20, K::Bend, 0, 0, 0x0400), ev(30, K::Bend, 0, 0, 0x2000),
                              ev(40, K::Off, 57, 0, 0)},
                   .blocks = 100,
                   .lfo = true});
    out.push_back({.name = "lfo off",
                   .events = {ev(0, K::On, 64, 100, 0), ev(60, K::Off, 64, 0, 0)},
                   .blocks = 120,
                   .lfo = false});
    return out;
}

void compare(double rate, const std::vector<Dx7Voice>& voices) {
    std::size_t compared = 0;
    for (std::size_t v = 0; v < voices.size(); ++v) {
        for (const Scenario& s : scenarios()) {
            Dx7Voice voice = voices[v];
            voice.bytes[adx::dx7::kLfoAmd] = 0; // upstream has no amplitude modulation
            voice.bytes[adx::dx7::kOpEnable] = 0x3f;
            if (!s.lfo) {
                voice.bytes[adx::dx7::kLfoPmd] = 0;
            }
            const auto upstream = adx::test::msfaRender(rate, voice.bytes.data(),
                                                        forUpstream(s.events, voice), s.blocks);
            const auto port = portRender(rate, voice, s.events, s.blocks);
            REQUIRE(upstream.size() == port.size());
            for (std::size_t i = 0; i < port.size(); ++i) {
                if (port[i] != upstream[i]) {
                    INFO("voice " << v << " (" << adx::dx7::nameOf(voice).data() << "), " << s.name
                                  << ", rate " << rate << ", block " << (i / 64) << " sample "
                                  << (i % 64));
                    REQUIRE(port[i] == upstream[i]);
                }
            }
            ++compared;
        }
    }
    REQUIRE(compared == voices.size() * scenarios().size());
}

} // namespace

TEST_CASE("dx7_engine_matches_msfa", "[dx7]") {
    const std::vector<Dx7Voice> voices = fixtureVoices();
    // Not silence: a comparison of two zero buffers would prove nothing.
    {
        Dx7Voice voice = voices[0];
        voice.bytes[adx::dx7::kLfoAmd] = 0;
        const auto out = portRender(48000.0, voice, {ev(0, OracleEvent::Kind::On, 60, 100, 0)}, 20);
        REQUIRE(std::ranges::any_of(out, [](std::int32_t s) { return s != 0; }));
    }
    SECTION("engine rate, 48 kHz") {
        compare(48000.0, voices);
    }
    SECTION("FM-1 rate, 44 118 Hz") {
        compare(44118.0, voices);
    }
}

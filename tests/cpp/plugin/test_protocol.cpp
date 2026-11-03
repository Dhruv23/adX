// The sandbox's wire formats (phase_9.md §4.3, §5 protocol_roundtrip): the control
// protocol's encoding, the scan result's JSON, state as base64, and audio and events
// through a real child and back.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <random>
#include <vector>

#include "engine/plugin/Bridge/AudioLink.h"
#include "engine/plugin/Bridge/HostProcess.h"
#include "engine/plugin/Bridge/Protocol.h"
#include "engine/plugin/PluginDescriptor.h"
#include "engine/plugin/StateBlob.h"
#include "tests/cpp/plugin/PluginFixtures.h"

namespace {

using namespace adx::plugin;
namespace fx = adx::tests::plugins;

PluginDescriptor sampleDescriptor() {
    PluginDescriptor d;
    d.id = PluginId{.format = PluginFormat::Vst3,
                    .uid = "0123456789ABCDEF0123456789ABCDEF",
                    .path = "C:/Program Files/Common Files/VST3/Thing \"Pro\".vst3",
                    .name = "Thing \u00e9",
                    .version = "1.2.3"};
    d.vendor = "Vendor\\Name";
    d.instrument = true;
    d.audioInputs = 0;
    d.audioOutputs = 2;
    d.hasEditor = true;
    d.latency = 128;
    d.params.push_back(ParamInfo{.id = 7,
                                 .key = "cutoff",
                                 .name = "Cutoff",
                                 .minimum = 20.0,
                                 .maximum = 20000.0,
                                 .defaultValue = 1000.0,
                                 .steps = 0,
                                 .automatable = true,
                                 .hidden = false});
    d.params.push_back(ParamInfo{.id = 0xFFFFFFF0U,
                                 .key = "mode",
                                 .name = "Mode",
                                 .minimum = 0.0,
                                 .maximum = 3.0,
                                 .defaultValue = 1.0,
                                 .steps = 3,
                                 .automatable = false,
                                 .hidden = true});
    return d;
}

/// One block through the link with a generous deadline.
bridge::BlockResult run(bridge::RemoteInstance& instance, std::vector<float>& left,
                        std::vector<float>& right, std::vector<float>& outLeft,
                        std::vector<float>& outRight,
                        std::span<const bridge::WireEvent> events = {},
                        std::span<const bridge::WireParam> params = {}) {
    const std::array<const float*, 2> inputs{left.data(), right.data()};
    const std::array<float*, 2> outputs{outLeft.data(), outRight.data()};
    std::vector<bridge::WireParam> echoes(bridge::kMaxOutParams);
    bridge::BlockRequest request;
    request.inputs = inputs;
    request.frames = static_cast<std::uint32_t>(left.size());
    request.events = events;
    request.params = params;
    request.transport.tempo = 120.0;
    request.deadlineMicros = 5'000'000;
    std::uint32_t count = 0;
    return bridge::runBlock(instance.link(), request, outputs, echoes, count);
}

} // namespace

TEST_CASE("protocol_byte_roundtrip", "[plugin][protocol]") {
    bridge::ByteWriter out;
    out.u32(0xDEADBEEF);
    out.u64(0x0123456789ABCDEFULL);
    out.f64(-3.25);
    out.str("hello");
    out.bytes(std::vector<std::uint8_t>{1, 2, 3});
    bridge::LoadRequest load;
    load.instance = 42;
    load.format = PluginFormat::Vst3;
    load.path = "C:/x.vst3";
    load.uid = "abc";
    load.sampleRate = 44100;
    load.maxFrames = 1024;
    load.region = "Local\\r";
    load.goEvent = "Local\\g";
    load.doneEvent = "Local\\d";
    load.write(out);
    bridge::LoadReply reply;
    reply.error = "none";
    reply.descriptor = sampleDescriptor();
    reply.write(out);

    bridge::ByteReader in(out.data());
    CHECK(in.u32() == 0xDEADBEEF);
    CHECK(in.u64() == 0x0123456789ABCDEFULL);
    CHECK(in.f64() == -3.25);
    CHECK(in.str() == "hello");
    CHECK(in.bytes() == std::vector<std::uint8_t>{1, 2, 3});
    bridge::LoadRequest loadBack;
    REQUIRE(loadBack.read(in));
    CHECK(loadBack.instance == 42);
    CHECK(loadBack.format == PluginFormat::Vst3);
    CHECK(loadBack.doneEvent == "Local\\d");
    bridge::LoadReply replyBack;
    REQUIRE(replyBack.read(in));
    CHECK(replyBack.descriptor == sampleDescriptor());
    CHECK(in.ok());

    // A short read fails once, at the end, and never reads past the buffer.
    bridge::ByteReader shortRead(std::span<const std::uint8_t>(out.data().data(), 6));
    static_cast<void>(shortRead.u32());
    static_cast<void>(shortRead.u64());
    static_cast<void>(shortRead.str());
    CHECK_FALSE(shortRead.ok());
}

TEST_CASE("plugin_scan_json_roundtrip", "[plugin][protocol]") {
    ScanResult result;
    result.path = "C:/Plugins/Thing.vst3";
    result.format = PluginFormat::Vst3;
    result.mtime = 134378119093917856LL;
    result.size = 834560;
    result.ok = true;
    result.plugins.push_back(sampleDescriptor());
    ScanResult back;
    std::string error;
    REQUIRE(fromJson(toJson(result), back, error));
    CHECK(back == result);

    CHECK_FALSE(fromJson("{\"path\": 3", back, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(fromJson("[]", back, error));
}

TEST_CASE("plugin_state_base64", "[plugin][protocol]") {
    std::mt19937 random(7);
    for (std::size_t size = 0; size < 70; ++size) {
        StateBlob bytes(size);
        for (auto& b : bytes) {
            b = static_cast<std::uint8_t>(random());
        }
        StateBlob back;
        REQUIRE(decodeBase64(encodeBase64(bytes), back));
        CHECK(back == bytes);
    }
    StateBlob out;
    CHECK(encodeBase64(std::vector<std::uint8_t>{'M', 'a', 'n'}) == "TWFu");
    CHECK_FALSE(decodeBase64("TWF", out));  // not a multiple of four
    CHECK_FALSE(decodeBase64("TW=u", out)); // padding in the middle
    CHECK_FALSE(decodeBase64("TWF!", out)); // not the alphabet
}

TEST_CASE("plugin_param_keys", "[plugin][protocol]") {
    std::vector<ParamInfo> taken;
    CHECK(paramKey("Band 1 Freq", 1, taken) == "band_1_freq");
    CHECK(paramKey("  Gain (dB) ", 2, taken) == "gain_db");
    CHECK(paramKey("3D Width", 3, taken) == "p3_3d_width");
    CHECK(paramKey("", 9, taken) == "p9");
    CHECK(paramKey("Mix", 4, taken) == "mix_"); // the slot's own key
    ParamInfo gain;
    gain.id = 5;
    gain.key = "gain";
    taken.push_back(gain);
    CHECK(paramKey("GAIN", 6, taken) == "gain_2");
}

TEST_CASE("protocol_roundtrip", "[plugin][protocol]") {
    fx::useTestHost();
    auto host = std::make_shared<bridge::HostProcess>(fx::hostExe(), nullptr);
    std::string error;
    REQUIRE(host->start(error));

    SECTION("audio comes back bit-identical through a unity-gain effect") {
        auto effect = host->load(fx::effectId(PluginFormat::Clap), nullptr, 48000, 512, error);
        INFO(error);
        REQUIRE(effect);
        CHECK(effect->descriptor().params.size() == 4);
        std::mt19937 random(11);
        std::uniform_real_distribution<float> sample(-1.0F, 1.0F);
        for (int block = 0; block < 50; ++block) {
            const std::size_t frames = 64 + ((static_cast<std::size_t>(block) * 37) % 449);
            std::vector<float> left(frames);
            std::vector<float> right(frames);
            for (std::size_t i = 0; i < frames; ++i) {
                left[i] = sample(random);
                right[i] = sample(random);
            }
            std::vector<float> outLeft(frames, 9.0F);
            std::vector<float> outRight(frames, 9.0F);
            REQUIRE(run(*effect, left, right, outLeft, outRight) == bridge::BlockResult::Done);
            CHECK(outLeft == left);
            CHECK(outRight == right);
        }
    }

    SECTION("events land at their offsets, the same in two instances") {
        const auto renderNotes = [&](bridge::RemoteInstance& synth) {
            std::vector<float> rendered;
            for (int block = 0; block < 8; ++block) {
                std::vector<float> silent(256, 0.0F);
                std::vector<float> silentRight(256, 0.0F);
                std::vector<float> outLeft(256);
                std::vector<float> outRight(256);
                std::vector<bridge::WireEvent> events;
                if (block == 1) {
                    bridge::WireEvent on{};
                    on.offset = 100;
                    on.kind = bridge::WireEventKind::NoteOn;
                    on.key = 69;
                    on.velocity = 100;
                    events.push_back(on);
                }
                REQUIRE(run(synth, silent, silentRight, outLeft, outRight, events) ==
                        bridge::BlockResult::Done);
                rendered.insert(rendered.end(), outLeft.begin(), outLeft.end());
            }
            return rendered;
        };
        auto first = host->load(fx::synthId(PluginFormat::Clap), nullptr, 48000, 512, error);
        auto second = host->load(fx::synthId(PluginFormat::Clap), nullptr, 48000, 512, error);
        REQUIRE(first);
        REQUIRE(second);
        const std::vector<float> a = renderNotes(*first);
        const std::vector<float> b = renderNotes(*second);
        CHECK(a == b);
        // Silent up to block 1's offset 100, sounding from there.
        for (std::size_t i = 0; i < 256 + 100; ++i) {
            REQUIRE(a[i] == 0.0F);
        }
        CHECK(std::abs(a[256 + 101]) > 0.0F);
    }
}

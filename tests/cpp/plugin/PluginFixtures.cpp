#include "tests/cpp/plugin/PluginFixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "engine/plugin/PluginHost.h"
#include "engine/plugin/StateBlob.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace adx::tests::plugins {

std::string hostExe() {
    return ADX_PLUGIN_HOST_EXE;
}

std::string clapPath() {
    return ADX_TEST_PLUGIN_CLAP;
}

std::string vst3Path() {
    return ADX_TEST_PLUGIN_VST3;
}

std::string freshDirectory(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "adx-tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

void useTestHost() {
    plugin::PluginHost& host = plugin::PluginHost::instance();
    host.setExecutable(hostExe());
    static const std::string kCache =
        (std::filesystem::path(freshDirectory("plugin-cache")) / "cache.json").string();
    std::string error;
    static_cast<void>(host.loadCache(kCache, error));
}

ScopedEnv::ScopedEnv(std::string name, const std::string& value) : m_name(std::move(name)) {
    REQUIRE(_putenv_s(m_name.c_str(), value.c_str()) == 0);
}

ScopedEnv::~ScopedEnv() {
    static_cast<void>(_putenv_s(m_name.c_str(), ""));
}

plugin::PluginId effectId(plugin::PluginFormat format) {
    if (format == plugin::PluginFormat::Clap) {
        return plugin::PluginId{.format = format,
                                .uid = "com.adx.test.effect",
                                .path = clapPath(),
                                .name = "adX Test Effect",
                                .version = {}};
    }
    return plugin::PluginId{.format = format,
                            .uid = "54584441464558460000000100000001",
                            .path = vst3Path(),
                            .name = "adX Test Effect",
                            .version = {}};
}

plugin::PluginId synthId(plugin::PluginFormat format) {
    if (format == plugin::PluginFormat::Clap) {
        return plugin::PluginId{.format = format,
                                .uid = "com.adx.test.synth",
                                .path = clapPath(),
                                .name = "adX Test Synth",
                                .version = {}};
    }
    return plugin::PluginId{.format = format,
                            .uid = "545844415953544E0000000100000002",
                            .path = vst3Path(),
                            .name = "adX Test Synth",
                            .version = {}};
}

std::string fixtureState(double gain, std::uint32_t latency, double seed,
                         plugin::PluginFormat format) {
    std::vector<std::uint8_t> bytes{'A', 'D', 'X', 'T'};
    const auto append = [&bytes](double value) {
        std::array<std::uint8_t, sizeof(double)> raw{};
        std::memcpy(raw.data(), &value, sizeof value);
        bytes.insert(bytes.end(), raw.begin(), raw.end());
    };
    append(gain);
    append(static_cast<double>(latency));
    append(seed);
    if (format == plugin::PluginFormat::Vst3) {
        std::vector<std::uint8_t> wrapped{'A', 'V', '3', 'S'};
        const auto size = static_cast<std::uint32_t>(bytes.size());
        for (unsigned shift = 0; shift < 32; shift += 8) {
            wrapped.push_back(static_cast<std::uint8_t>((size >> shift) & 0xFFU));
        }
        wrapped.insert(wrapped.end(), bytes.begin(), bytes.end());
        wrapped.insert(wrapped.end(), {0, 0, 0, 0});
        bytes = std::move(wrapped);
    }
    return plugin::encodeBase64(bytes);
}

core::SlotId addFixtureEffect(project::Project& project, const EffectSetup& setup) {
    const plugin::PluginId id = effectId(setup.format);
    project::Insert insert;
    insert.id = project.newInsertId();
    insert.name = "Hosted";
    project::Slot slot;
    slot.id = project.newSlotId();
    slot.type = plugin::toString(setup.format);
    slot.mix = setup.mix;
    slot.params = setup.params;
    slot.plugin = project::PluginRef{
        .uid = id.uid, .path = id.path, .name = id.name, .state = setup.state, .isolated = false};
    insert.slots.push_back(slot);
    project.mixer.inserts.push_back(insert);
    project.mixer.routes.push_back(
        project::Route{.id = project.newRouteId(), .from = insert.id, .to = project.mixer.master});
    project.channels.front().output = insert.id;
    return slot.id;
}

project::Project synthProject(plugin::PluginFormat format) {
    project::Project project =
        toneProject({{.start = 0, .length = 960, .pitch = 69, .velocity = 100},
                     {.start = 1920, .length = 960, .pitch = 72, .velocity = 80}});
    const plugin::PluginId id = synthId(format);
    project::Channel& channel = project.channels.front();
    channel.instrument = project::InstrumentSpec{};
    channel.instrument.type = plugin::toString(format);
    channel.instrument.plugin = project::PluginRef{
        .uid = id.uid, .path = id.path, .name = id.name, .state = {}, .isolated = false};
    return project;
}

std::vector<float> renderPlugin(const project::Project& project, std::uint64_t frames,
                                std::uint32_t block) {
    render::OfflineRenderOptions options;
    options.frames = frames;
    options.blockFrames = block;
    render::RenderStats stats;
    std::vector<float> out = render::renderOffline(project, options, stats);
    REQUIRE(stats.error.empty());
    return out;
}

float peakOf(const std::vector<float>& samples) {
    float peak = 0.0F;
    for (const float s : samples) {
        peak = std::max(peak, std::abs(s));
    }
    return peak;
}

bool allFinite(const std::vector<float>& samples) {
    return std::ranges::all_of(samples, [](float s) { return std::isfinite(s); });
}

} // namespace adx::tests::plugins

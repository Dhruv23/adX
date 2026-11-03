// What the plugin tests share: where the sandbox child and adx_test_plugin are, a
// clean plugin cache, and projects that host the fixture.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/plugin/PluginId.h"
#include "engine/project/Project.h"

namespace adx::tests::plugins {

[[nodiscard]] std::string hostExe();
[[nodiscard]] std::string clapPath();
[[nodiscard]] std::string vst3Path();

/// Points PluginHost at the built child and a cache file of its own under the temp
/// directory, so no test sees the user's cache or another test's.
void useTestHost();

/// A directory under %TEMP% for one test, emptied first.
[[nodiscard]] std::string freshDirectory(const std::string& name);

/// Sets an environment variable for the scope (the child inherits it).
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value);
    ~ScopedEnv();
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

private:
    std::string m_name;
};

/// The fixture's identities.
[[nodiscard]] plugin::PluginId effectId(plugin::PluginFormat format);
[[nodiscard]] plugin::PluginId synthId(plugin::PluginFormat format);

/// The fixture's own state format (TestCore.h): gain, latency, seed. Base64, ready for a
/// PluginRef. VST3 wraps it as the host does (Vst3Module.cpp): "AV3S", the component's
/// bytes and an empty controller's, each length-prefixed.
[[nodiscard]] std::string fixtureState(double gain, std::uint32_t latency, double seed,
                                       plugin::PluginFormat format = plugin::PluginFormat::Clap);

/// toneProject's channel, through an insert holding the fixture effect, to the master.
/// Returns the slot id. `params` are written on the slot.
struct EffectSetup {
    plugin::PluginFormat format{plugin::PluginFormat::Clap};
    std::string state;
    std::vector<project::SlotParam> params;
    float mix{1.0F};
};
core::SlotId addFixtureEffect(project::Project& project, const EffectSetup& setup);

/// A project whose one channel plays the fixture synth.
[[nodiscard]] project::Project synthProject(plugin::PluginFormat format);

/// Offline render of `frames` at `block`. REQUIREs that it rendered.
[[nodiscard]] std::vector<float> renderPlugin(const project::Project& project, std::uint64_t frames,
                                              std::uint32_t block = 256);

[[nodiscard]] float peakOf(const std::vector<float>& samples);
[[nodiscard]] bool allFinite(const std::vector<float>& samples);

} // namespace adx::tests::plugins

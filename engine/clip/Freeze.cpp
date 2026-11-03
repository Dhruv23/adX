// adx-thread: main
#include "engine/clip/Freeze.h"

#include <algorithm>

#include "engine/graph/GraphBuilder.h"
#include "engine/project/Project.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/OfflineRender.h"

namespace adx::clip {

FreezeResult renderFreeze(const project::Project& project, core::ChannelId channel,
                          const std::filesystem::path& file, std::uint32_t sampleRate,
                          std::uint32_t blockFrames, double maxTailSeconds) {
    FreezeResult result;
    const project::Channel* source = project.find(channel);
    if (source == nullptr) {
        result.error = "no such channel";
        return result;
    }
    // The channel as it would sound alone and live: unmuted, nothing soloed, and not
    // frozen already - a re-freeze renders the instrument, not its own last render.
    project::Project live = project;
    for (project::Channel& each : live.channels) {
        each.soloed = false;
        if (each.id == channel) {
            each.muted = false;
            each.frozen.clear();
        }
    }
    graph::StemSpec tap;
    tap.tap = channel;
    render::OfflineRenderOptions options;
    options.sampleRate = sampleRate;
    options.blockFrames = blockFrames;
    options.tailSeconds = maxTailSeconds;
    options.stem = &tap;
    render::RenderStats stats;
    std::vector<float> samples = render::renderOffline(live, options, stats);
    if (!stats.error.empty()) {
        result.error = stats.error;
        return result;
    }
    // Ends at the last sample that is not exactly zero: past it the live channel is
    // exactly silent too, so the frozen one may stop there and still match it.
    const auto last =
        std::ranges::find_if(samples.rbegin(), samples.rend(), [](float s) { return s != 0.0F; });
    samples.resize(static_cast<std::size_t>(samples.rend() - last));
    samples.resize(samples.size() + (samples.size() % 2));
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    if (!render::writeWavFloat32(file.string(), samples, 2, sampleRate)) {
        result.error = "could not write " + file.string();
        return result;
    }
    result.ok = true;
    result.file = file;
    result.frames = samples.size() / 2;
    result.hash = render::hashSamples(samples).hex();
    return result;
}

FreezeResult freezeChannel(project::Project& project, project::CommandStack& stack,
                           core::ChannelId channel) {
    const project::Channel* source = project.find(channel);
    if (source == nullptr) {
        FreezeResult failed;
        failed.error = "no such channel";
        return failed;
    }
    // Named after the channel, in the project's folder, relative like every sample.
    std::string name;
    for (const char c : source->name) {
        name += std::string_view{"<>:\"/\\|?* "}.find(c) == std::string_view::npos ? c : '_';
    }
    const std::filesystem::path folder =
        std::filesystem::path{project.resources.baseDirectory} / "freeze";
    FreezeResult result = renderFreeze(project, channel, folder / (name + ".rendering.wav"));
    if (!result.ok) {
        return result;
    }
    const std::string file = name + "-" + result.hash.substr(0, 8) + ".wav";
    std::error_code error;
    std::filesystem::rename(result.file, folder / file, error);
    if (error) {
        result.ok = false;
        result.error = "could not rename the render: " + error.message();
        return result;
    }
    result.file = folder / file;
    stack.execute(std::make_unique<project::SetChannelFrozen>(channel, "freeze/" + file), project);
    return result;
}

void unfreezeChannel(project::Project& project, project::CommandStack& stack,
                     core::ChannelId channel) {
    stack.execute(std::make_unique<project::SetChannelFrozen>(channel, std::string{}), project);
}

} // namespace adx::clip

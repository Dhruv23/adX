// adx-thread: main
#include "engine/plugin/ProjectSync.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/plugin/PluginBinding.h"
#include "engine/plugin/PluginCore.h"
#include "engine/plugin/PluginInstrument.h"
#include "engine/plugin/PluginNode.h"
#include "engine/plugin/StateBlob.h"
#include "engine/project/AutomationWriter.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/PluginCommands.h"
#include "engine/render/RenderEngine.h"

namespace adx::plugin {

const PluginCore* pluginOfSlot(render::RenderEngine& engine, core::SlotId slot) {
    const auto node = engine.builder().nodes().findSlot(slot);
    const auto* effect = dynamic_cast<const PluginEffect*>(node.get());
    return effect != nullptr ? &effect->core() : nullptr;
}

const PluginCore* pluginOfChannel(render::RenderEngine& engine, core::ChannelId channel) {
    const auto node = engine.builder().nodes().findChannel(channel);
    const auto* instrument = dynamic_cast<const PluginInstrument*>(node.get());
    return instrument != nullptr ? &instrument->core() : nullptr;
}

namespace {

/// The live state of `core` as the file spells it, or false when there is none.
bool liveState(const PluginCore* core, std::string& out) {
    if (core == nullptr || core->binding() == nullptr || !core->binding()->instance) {
        return false;
    }
    StateBlob state;
    if (!core->binding()->instance->saveState(state)) {
        return false;
    }
    out = encodeBase64(state);
    return true;
}

} // namespace

std::size_t syncPluginStates(render::RenderEngine& engine, project::Project& project,
                             project::CommandStack& stack) {
    std::size_t changed = 0;
    for (const project::Insert& insert : project.mixer.inserts) {
        for (const project::Slot& slot : insert.slots) {
            if (slot.plugin.empty()) {
                continue;
            }
            std::string state;
            if (liveState(pluginOfSlot(engine, slot.id), state) && state != slot.plugin.state) {
                stack.execute(std::make_unique<project::SetPluginState>(slot.id, state), project);
                ++changed;
            }
        }
    }
    for (const project::Channel& channel : project.channels) {
        if (channel.instrument.plugin.empty()) {
            continue;
        }
        std::string state;
        if (liveState(pluginOfChannel(engine, channel.id), state) &&
            state != channel.instrument.plugin.state) {
            stack.execute(std::make_unique<project::SetPluginState>(channel.id, state), project);
            ++changed;
        }
    }
    return changed;
}

bool echoIsEdit(const ParamEcho& echo, double stored) noexcept {
    if (!std::isnan(stored) && std::abs(static_cast<double>(echo.value) - stored) <=
                                   1e-6 * std::max(1.0, std::abs(stored))) {
        return false; // what the project already says
    }
    return echo.gesture || !echo.settling;
}

std::size_t applyPluginEchoes(render::RenderEngine& engine, project::Project& project,
                              project::CommandStack& stack, project::AutomationWriter* writer,
                              std::int64_t tick) {
    std::size_t applied = 0;
    // Slot ids and channel ids first: the commands below may move the vectors.
    std::vector<core::SlotId> slots;
    for (const project::Insert& insert : project.mixer.inserts) {
        for (const project::Slot& slot : insert.slots) {
            if (!slot.plugin.empty()) {
                slots.push_back(slot.id);
            }
        }
    }
    std::vector<core::ChannelId> channels;
    for (const project::Channel& channel : project.channels) {
        if (!channel.instrument.plugin.empty()) {
            channels.push_back(channel.id);
        }
    }
    for (const core::SlotId id : slots) {
        const PluginCore* core = pluginOfSlot(engine, id);
        ParamEcho echo{};
        while (core != nullptr && core->takeEcho(echo)) {
            const std::string key(core->paramKey(echo.index));
            const project::Slot* slot = project.mixer.findSlot(id);
            if (key.empty() || slot == nullptr) {
                continue;
            }
            const auto found = std::ranges::find(slot->params, key, &project::SlotParam::name);
            if (!echoIsEdit(echo, found != slot->params.end()
                                      ? found->value
                                      : std::numeric_limits<double>::quiet_NaN())) {
                continue;
            }
            if (found == slot->params.end()) {
                stack.execute(std::make_unique<project::SetSlotValue>(id, key, echo.value),
                              project);
            }
            slot = project.mixer.findSlot(id);
            const auto match = std::ranges::find(slot->params, key, &project::SlotParam::name);
            if (match != slot->params.end()) {
                const project::ParamRef ref{
                    .owner = id.value,
                    .index = static_cast<std::uint16_t>(match - slot->params.begin()),
                    .kind = project::ParamKind::SlotParam};
                if (found != slot->params.end()) {
                    static_cast<void>(engine.setParam(project, stack, ref, echo.value));
                }
                if (writer != nullptr && echo.gesture) {
                    writer->add(ref, tick, echo.value);
                }
            }
            ++applied;
        }
    }
    for (const core::ChannelId id : channels) {
        const PluginCore* core = pluginOfChannel(engine, id);
        ParamEcho echo{};
        while (core != nullptr && core->takeEcho(echo)) {
            const std::string key(core->paramKey(echo.index));
            const project::Channel* channel = project.find(id);
            if (key.empty() || channel == nullptr) {
                continue;
            }
            const auto found =
                std::ranges::find(channel->instrument.params, key, &project::ParamValue::name);
            if (!echoIsEdit(echo, found != channel->instrument.params.end()
                                      ? static_cast<double>(found->value)
                                      : std::numeric_limits<double>::quiet_NaN())) {
                continue;
            }
            const bool known = found != channel->instrument.params.end();
            if (!known) {
                stack.execute(std::make_unique<project::SetChannelParam>(id, key, echo.value),
                              project);
            }
            channel = project.find(id);
            const auto& values = channel->instrument.params;
            const auto match = std::ranges::find(values, key, &project::ParamValue::name);
            if (match != values.end()) {
                const project::ParamRef ref{.owner = id.value,
                                            .index =
                                                static_cast<std::uint16_t>(match - values.begin()),
                                            .kind = project::ParamKind::ChannelInstrumentParam};
                if (known) {
                    static_cast<void>(engine.setParam(project, stack, ref, echo.value));
                }
                if (writer != nullptr && echo.gesture) {
                    writer->add(ref, tick, echo.value);
                }
            }
            ++applied;
        }
    }
    return applied;
}

} // namespace adx::plugin

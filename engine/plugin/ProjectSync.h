// adx-thread: main
//
// What flows from live plugins back into the project (phase_9.md §4.7): their state,
// before a save or an export, and the parameter changes a plugin's own editor made.
// Both go through commands, so they are in the file and in undo like any other edit.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/Ids.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project
namespace adx::render {
class RenderEngine;
}

namespace adx::plugin {

class PluginCore;

/// The live plugin behind a slot, or a channel's plugin instrument; null for none.
[[nodiscard]] const PluginCore* pluginOfSlot(render::RenderEngine& engine, core::SlotId slot);
[[nodiscard]] const PluginCore* pluginOfChannel(render::RenderEngine& engine,
                                                core::ChannelId channel);

/// Asks every live plugin in the project for its state and stores the ones that
/// changed (SetPluginState). Returns how many did.
std::size_t syncPluginStates(render::RenderEngine& engine, project::Project& project,
                             project::CommandStack& stack);

/// Puts the parameter values plugins changed themselves into the project. A
/// parameter the project already names is a knob turn (heard now, no rebuild); a new
/// one is appended to the slot or instrument. Returns how many.
std::size_t applyPluginEchoes(render::RenderEngine& engine, project::Project& project,
                              project::CommandStack& stack);

} // namespace adx::plugin

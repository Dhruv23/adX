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
class AutomationWriter;
} // namespace adx::project
namespace adx::render {
class RenderEngine;
}

namespace adx::plugin {

class PluginCore;
struct ParamEcho;

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
///
/// Not every echo is an edit (P10.5-2, P9-7): one equal to what the project holds is
/// dropped, and so is a value a freshly loaded plugin reports without a gesture. While
/// `writer` is writing, each gesture also becomes a breakpoint at `tick`.
std::size_t applyPluginEchoes(render::RenderEngine& engine, project::Project& project,
                              project::CommandStack& stack,
                              project::AutomationWriter* writer = nullptr, std::int64_t tick = 0);

/// The rule above, for one echo against the value the project stores (NaN: none).
[[nodiscard]] bool echoIsEdit(const ParamEcho& echo, double stored) noexcept;

} // namespace adx::plugin

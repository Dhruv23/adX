// The one Python-visible owner of a render engine.
//
// Separate from ProjectHandle on purpose: a project can exist without being played,
// and one engine plays whichever project it was last given. Python holds the two
// independently and hands the project to the engine.
#pragma once

#include <memory>
#include <vector>

#include "engine/render/RenderEngine.h"

namespace adx::bindings {

struct EngineHandle {
    std::unique_ptr<render::RenderEngine> engine;
    bool opened{false};
    bool running{false};
    /// Reused by Engine.frame() so the 60 Hz read allocates nothing once warm (P4-1).
    std::vector<render::RenderEngine::StripLevel> levelScratch;
};

/// The arrangement transport, as its own Python object: `engine.transport.play()`.
/// Holds the engine it drives; the binding keeps the engine alive while it exists.
struct TransportHandle {
    EngineHandle* engine{nullptr};
};

} // namespace adx::bindings

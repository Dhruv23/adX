// The one Python-visible owner of a render engine.
//
// Separate from ProjectHandle on purpose: a project can exist without being played,
// and one engine plays whichever project it was last given. Python holds the two
// independently and hands the project to the engine.
#pragma once

#include <memory>
#include <vector>

#include "engine/audio/CaptureTap.h"
#include "engine/audio/OfflineBackend.h"
#include "engine/audio/WasapiExclusiveBackend.h"
#include "engine/midi/MidiBackend.h"
#include "engine/render/RenderEngine.h"

namespace adx::bindings {

struct EngineHandle {
    std::unique_ptr<render::RenderEngine> engine;
    /// Set when the engine runs on an offline backend - a test's, rendered by
    /// Engine.render_offline() rather than by a device clock.
    audio::OfflineBackend* offline{nullptr};
    /// Set when the engine plays the default output in WASAPI exclusive mode.
    audio::WasapiExclusiveBackend* exclusive{nullptr};
    bool opened{false};
    bool running{false};
    /// Reused by Engine.frame() so the 60 Hz read allocates nothing once warm (P4-1).
    std::vector<render::RenderEngine::StripLevel> levelScratch;
    /// The same strips as the meter builder takes them (frame_with_meters).
    std::vector<rt::LevelFrame> meterScratch;
    /// The MIDI inputs, made on first use (bindings/midi.cpp).
    std::unique_ptr<midi::MidiDevices> midi;
    /// The last control message Engine.midi_last_control() reported (MidiLearn.h).
    std::uint64_t learnSequence{0};
    /// Engine.capture_start's tap (capture.cpp). Declared after `engine`, so it goes
    /// first: its worker reads the engine's scope tap.
    std::unique_ptr<audio::CaptureTap> capture;

    EngineHandle() = default;
    EngineHandle(const EngineHandle&) = delete;
    EngineHandle& operator=(const EngineHandle&) = delete;
    EngineHandle(EngineHandle&&) = delete;
    EngineHandle& operator=(EngineHandle&&) = delete;
    /// The stream stops before the MIDI inputs go: the audio thread may hold one of
    /// them until its block ends (EngineCore::setMidiInput).
    ~EngineHandle() {
        if (midi && engine) {
            engine->stop();
        }
        midi.reset();
    }
};

/// The arrangement transport, as its own Python object: `engine.transport.play()`.
/// Holds the engine it drives; the binding keeps the engine alive while it exists.
struct TransportHandle {
    EngineHandle* engine{nullptr};
};

} // namespace adx::bindings

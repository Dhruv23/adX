// The one Python-visible owner of a render engine.
//
// Separate from ProjectHandle on purpose: a project can exist without being played,
// and one engine plays whichever project it was last given. Python holds the two
// independently and hands the project to the engine.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "engine/audio/CaptureTap.h"
#include "engine/audio/OfflineBackend.h"
#include "engine/audio/WasapiExclusiveBackend.h"
#include "engine/midi/MidiBackend.h"
#include "engine/midi/PadFeedback.h"
#include "engine/project/AutomationWriter.h"
#include "engine/record/PerformanceRecorder.h"
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
    /// A pad controller's lights (bindings/pads.cpp). After `midi`, so it goes first:
    /// its thread sends through the outputs.
    std::unique_ptr<midi::PadFeedback> feedback;
    /// Which session cells hold a clip, for the lights' "empty" (64 x 64, track-major).
    std::unique_ptr<std::array<std::atomic<std::uint8_t>, 4096>> padFilled =
        std::make_unique<std::array<std::atomic<std::uint8_t>, 4096>>();
    /// The performance log, drained from the audio thread at UI rate (phase_11.md §4.6).
    record::PerformanceRecorder performance;
    /// A MIDI take's messages, drained from the audio thread at UI rate (P9-6).
    std::vector<midi::TakeEvent> take;
    /// Automation being written from plugin editors' gestures (P9-7).
    project::AutomationWriter automation;
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
        feedback.reset();
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

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <type_traits>

// Forward declarations
struct EnvelopeData;
struct WavetableData;

// --- Data Structures ---

// Envelope States
enum class EnvState {
    Idle,
    Attack,
    Decay,
    Sustain,
    Release
};

// Represents a Synth configuration
struct Patch {
    std::string name;

    // Core parameters (example for a basic additive synth or simple subtractive)
    float attackTime = 0.1f;
    float decayTime = 0.1f;
    float sustainLevel = 0.7f;
    float releaseTime = 0.5f;

    // Pointers to pre-calculated tables or read-only resources
    const EnvelopeData* envTable = nullptr;
    const WavetableData* waveTable = nullptr;
};

// Represents a note played in a sequence
struct Note {
    float startBeat = 0.0f; // Position in beats
    float lengthBeats = 1.0f; // Duration in beats
    uint8_t pitch = 60; // MIDI Note Number (Middle C)
    uint8_t velocity = 100; // MIDI Velocity (0-127)
};

// A Sequence of notes tied to a patch
struct Track {
    std::string patchName; // Reference to the Patch
    std::vector<Note> notes;
};

// Core state owned and mutated by the Main Thread
struct SequencerState {
    std::unordered_map<std::string, Patch> patches;
    std::vector<Track> tracks;

    // Shared between Main and Audio threads
    // The main thread might reset it, the audio thread increments it
    std::atomic<float> playheadPositionBeats{0.0f};
};

// --- Thread Synchronization ---

// Event types passed to the audio thread
enum class AudioEventType : uint8_t {
    NoteOn,
    NoteOff,
    ParameterChange
};

// Represents a single event passed from Main -> Audio Thread via lock-free queue
// It must be trivially copyable and contain NO allocating types (like std::string)
struct AudioEvent {
    AudioEventType type;

    // Basic note info
    uint8_t pitch;     // 0-127
    uint8_t velocity;  // 0-127

    // If it's a ParameterChange, this could hold an ID and a new float value.
    // If NoteOn, it might need pointers to the read-only tables for the allocated voice.
    union Data {
        struct {
            const EnvelopeData* envTable;
            const WavetableData* waveTable;
            float attackTime;
            float decayTime;
            float sustainLevel;
            float releaseTime;
        } noteData;

        // Simplified patch data passed in NoteOn
        struct {
            float attackTime;
            float decayTime;
            float sustainLevel;
            float releaseTime;
        } patch;

        struct {
            uint32_t paramId;
            float value;
        } paramData;
    } data;
};

static_assert(std::is_trivially_copyable_v<AudioEvent>, "AudioEvent must be trivially copyable for lock-free passing");

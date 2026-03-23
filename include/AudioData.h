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

// Represents the harmonic profile for a specific MIDI note
struct TimbreKeyframe {
    uint8_t midiNote = 60;
    std::vector<float> harmonics; // Up to 16 harmonics
};

// Represents a Synth configuration
struct Patch {
    std::string name;

    // Raw envelope ms (for saving/loading .adx)
    float attackMs = 100.0f;
    float decayMs = 100.0f;
    float releaseMs = 500.0f;

    // Pre-calculated tables for envelope phases
    std::vector<float> attackTable;
    std::vector<float> decayTable;
    std::vector<float> releaseTable;

    // Level to hold at during sustain
    float sustainLevel = 0.7f;

    // Keyframes for interpolating harmonic amplitudes across the keyboard
    std::vector<TimbreKeyframe> timbreKeyframes;

    // Pointers to pre-calculated tables or read-only resources (kept for legacy/future use)
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
    std::atomic<bool> isPlaying{false};
    std::atomic<float> bpm{120.0f};

    // Global parameters
    std::atomic<float> masterVolume{0.75f};
    std::atomic<float> tuning{440.0f};
};

// --- Thread Synchronization ---

// Event types passed to the audio thread
enum class AudioEventType : uint8_t {
    NoteOn,
    NoteOff,
    ParameterChange,
    PatchUpdate,
    PlayStateChange,
    BpmChange,
    SequenceUpdate,
    MasterVolChange,
    GlobalTuningChange
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
    // If PatchUpdate, it holds a pointer to a new Patch object for the audio thread to take ownership of.
    // If SequenceUpdate, it holds a pointer to a new Track object.
    union Data {
        // Pointer to the thread-safe, read-only Patch object
        const Patch* patch;

        // Pointer to the thread-safe, read-only Track object for sequence updates
        const Track* track;

        struct {
            uint32_t paramId;
            float value;
        } paramData;

        struct {
            bool isPlaying;
        } playState;

        struct {
            float bpm;
        } bpmState;

        struct {
            float volume;
        } masterVol;

        struct {
            float tuning;
        } globalTuning;
    } data;
};

static_assert(std::is_trivially_copyable_v<AudioEvent>, "AudioEvent must be trivially copyable for lock-free passing");

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <memory>

// Forward declarations
struct EnvelopeData;
struct WavetableData;

// Sample rate the audio engine and all decoded audio is normalized to.
constexpr unsigned int kEngineSampleRate = 44100;

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

    bool operator==(const TimbreKeyframe&) const = default;
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

    // --- C418 suite: per-voice one-pole low-pass filter with LFO cutoff
    // modulation (warm, slowly-evolving "Dreiton" pads). Defaults = bypass.
    float filterCutoffHz = 20000.0f; // >= kFilterBypassHz means no filtering
    float filterLfoRateHz = 0.0f;    // cutoff modulation speed
    float filterLfoDepth = 0.0f;     // 0..1, fraction of cutoff swept by the LFO

    // --- STAKILLAZ suite: waveshaper drive and 808-style sub oscillator with
    // a pitch-drop transient (hardstyle kick "tok"). Defaults = clean/off.
    float drive = 0.0f;              // 0 = bypass; ~30 = blown out
    float subOscLevel = 0.0f;        // 0..1, summed after the harmonic stack
    int subOscWave = 0;              // 0 = sine, 1 = triangle
    float pitchDropSemitones = 0.0f; // 0 = off; e.g. 36 = start 3 octaves up
    float pitchDropMs = 50.0f;       // exponential decay time constant

    // Compares only the fields the .adx format actually stores — NOT
    // attackTable/decayTable/releaseTable, which are rebuilt from ImGui
    // Bezier-curve control points that have no on-disk representation.
    bool operator==(const Patch& other) const {
        return attackMs == other.attackMs && decayMs == other.decayMs &&
               releaseMs == other.releaseMs && sustainLevel == other.sustainLevel &&
               timbreKeyframes == other.timbreKeyframes &&
               filterCutoffHz == other.filterCutoffHz && filterLfoRateHz == other.filterLfoRateHz &&
               filterLfoDepth == other.filterLfoDepth && drive == other.drive &&
               subOscLevel == other.subOscLevel && subOscWave == other.subOscWave &&
               pitchDropSemitones == other.pitchDropSemitones && pitchDropMs == other.pitchDropMs;
    }
};

// Cutoffs at/above this are treated as "no filter" by the engine.
constexpr float kFilterBypassHz = 19000.0f;

// Factory library of pre-configured patches (C418 + STAKILLAZ suites).
// Returned patches are fully playable: envelope tables are already built at
// kEngineSampleRate. Implemented in src/PatchLibrary.cpp.
namespace PatchLibrary {
    const std::vector<std::string>& Names();
    Patch Create(const std::string& name); // falls back to a plain sine patch for unknown names
}

// Represents a note played in a sequence
struct Note {
    float startBeat = 0.0f; // Position in beats
    float lengthBeats = 1.0f; // Duration in beats
    uint8_t pitch = 60; // MIDI Note Number (Middle C)
    uint8_t velocity = 100; // MIDI Velocity (0-127)

    bool operator==(const Note&) const = default;
};

// A decoded audio file placed on the timeline. Position is stored in absolute
// seconds (not beats) since m_currentSamplePosition in AudioEngine is a
// sample-accurate wall clock independent of BPM; PCM data is shared (not
// deep-copied) so hot-swapping the owning Track on every UI edit stays cheap.
struct AudioClip {
    std::string filePath; // Display/serialization only, never touched on the audio thread
    float startTimeSeconds = 0.0f;
    std::shared_ptr<const std::vector<float>> pcmData; // What AudioEngine mixes — processed output @ kEngineSampleRate
    std::shared_ptr<const std::vector<float>> originalPcmData; // Raw decode, immutable; source AudioClipProcessor reprocesses from
    unsigned int sampleRate = kEngineSampleRate;
    unsigned int channels = 2;
    float pitchShiftSemitones = 0.0f; // 0 = unshifted
    float timeStretchFactor = 1.0f;   // 1.0 = original speed
    bool reversed = false;            // STAKILLAZ suite: play the source backwards

    // pcmData/originalPcmData/sampleRate/channels deliberately excluded —
    // deterministic from filePath+pitch+stretch, so comparing by pointer would
    // always report "changed" (every reload/reparse decodes fresh shared_ptrs,
    // even for an unchanged file). pitchShiftSemitones/timeStretchFactor ARE
    // compared since they're real, file-stored parameters a hot-reloaded .adx
    // edit can change.
    bool operator==(const AudioClip& other) const {
        return filePath == other.filePath && startTimeSeconds == other.startTimeSeconds &&
               pitchShiftSemitones == other.pitchShiftSemitones && timeStretchFactor == other.timeStretchFactor &&
               reversed == other.reversed;
    }
};

// Forward declaration (full definition in AudioEffect.h — kept out of this
// header so AudioData.h stays a lightweight POD-ish data header).
class AudioEffect;

// C418 suite: track-level arpeggiator. Held/overlapping notes on an
// arp-enabled track are not played directly — the engine turns the chord
// sounding at each rateBeats grid step into cascading staccato notes.
struct ArpSettings {
    int mode = 0;            // 0 = off, 1 = up, 2 = down, 3 = up-down
    float rateBeats = 0.25f; // 0.25 = 16th notes
    int octaves = 1;         // pattern octave span, 1..4
    float gate = 0.8f;       // note length as a fraction of the step

    bool operator==(const ArpSettings&) const = default;
};

// A Sequence of notes tied to a patch
struct Track {
    std::string patchName; // Reference to the Patch
    std::vector<Note> notes;
    std::vector<AudioClip> audioClips;
    ArpSettings arp;

    // Per-track insert chain (Phase 5), run by AudioEngine::process() on this
    // track's bus before mixing into the master. shared_ptr on purpose: the
    // Track copies shipped to the audio thread alias the SAME effect instances
    // (reverb tails survive sequence hot-swaps; UI atomic-parameter tweaks are
    // heard live). Instances are only ever created/destroyed on the Main
    // Thread; only the audio thread calls processSample().
    std::vector<std::shared_ptr<AudioEffect>> effects;

    // Effect equivalence (type + parameters, not pointer identity) is what
    // hot-reload diffing needs — a reparse always builds fresh instances.
    bool effectsEquivalent(const Track& other) const;

    bool operator==(const Track& other) const {
        return patchName == other.patchName && notes == other.notes && audioClips == other.audioClips &&
               arp == other.arp && effectsEquivalent(other);
    }
};

// Master-bus FX parameters (C418 delay/reverb, STAKILLAZ sidechain/drive).
// This is the Main Thread's authoritative copy for UI + .adx save/load; each
// field is mirrored to the audio thread via AudioEventType::ParameterChange
// events carrying an EngineParam id (see below).
struct MasterFxSettings {
    // Stereo ping-pong delay
    float delayTimeMs = 350.0f;
    float delayFeedback = 0.35f;
    float delayMix = 0.0f; // 0 = bypass

    // Algorithmic (Freeverb) master reverb
    float reverbRoom = 0.8f;
    float reverbDamp = 0.5f;
    float reverbMix = 0.0f; // 0 = bypass

    // Sidechain pump: Track 1's bus ducks the master
    float sidechainEnabled = 0.0f; // 0/1
    float sidechainAmount = 0.6f;  // 0..1 depth
    float sidechainReleaseMs = 120.0f;

    // Final tanh saturation stage before the compressor
    float masterDrive = 0.0f; // 0 = bypass

    bool operator==(const MasterFxSettings&) const = default;
};

// Core state owned and mutated by the Main Thread
struct SequencerState {
    std::unordered_map<std::string, Patch> patches;
    std::vector<Track> tracks;
    MasterFxSettings masterFx;

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

// Parameter ids carried by AudioEventType::ParameterChange (paramData.paramId)
// to address individual master-FX fields on the audio thread.
enum class EngineParam : uint32_t {
    DelayTimeMs = 0,
    DelayFeedback,
    DelayMix,
    ReverbRoom,
    ReverbDamp,
    ReverbMix,
    SidechainEnabled,
    SidechainAmount,
    SidechainReleaseMs,
    MasterDrive,
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
    // If SequenceUpdate, it holds a pointer to a new vector<Track> (the whole track list) object.
    union Data {
        // Pointer to the thread-safe, read-only Patch object
        const Patch* patch;

        // Pointer to the thread-safe, read-only Track list for sequence updates
        const std::vector<Track>* tracks;

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

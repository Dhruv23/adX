#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <memory>
#include <cmath>

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

    // --- Phase 3: noise oscillator, summed alongside the harmonic stack and
    // sub-oscillator. 0 level = off (no RNG work done for that voice).
    float noiseLevel = 0.0f;
    int noiseType = 0; // 0 = white, 1 = pink

    // --- Phase 3: resonant multimode filter (TPT state-variable), applied
    // after the C418 one-pole LPF stage above. Same bypass convention as
    // filterCutoffHz: cutoff >= kFilterBypassHz means "no filtering".
    int resFilterType = 0;            // 0 = LP, 1 = BP, 2 = HP
    float resFilterCutoff = 20000.0f; // Hz
    float resFilterResonance = 0.0f;  // 0..1, approaches self-oscillation near 1
    float filterEnvAmount = 0.0f;     // octaves of cutoff sweep at full env level (signed)
    float keyTrack = 0.0f;            // 0..1, fraction of 1:1 keyboard tracking

    // Dedicated filter envelope (FILTERENV), independent of the amplitude
    // ADSR above — same raw-ms-plus-precalculated-table shape.
    float filterEnvAttackMs = 0.0f;
    float filterEnvDecayMs = 0.0f;
    float filterEnvSustainLevel = 1.0f;
    float filterEnvReleaseMs = 0.0f;
    std::vector<float> filterEnvAttackTable;
    std::vector<float> filterEnvDecayTable;
    std::vector<float> filterEnvReleaseTable;

    // --- Phase 4: vocal formant bank. A parallel 3-band resonant bandpass
    // filter set — fixed per-vowel center frequencies (see kVowelFormants in
    // AudioEngine.cpp) crossfaded between two vowel presets by formantMorph,
    // then blended with the dry oscillator signal by formantAmount (0 =
    // bypass). formantMorph doubles as a Phase-2 automation target
    // (patch.formantMorph) — this value is just the seed.
    std::string formantVowelA = "Ah";
    std::string formantVowelB = "Ah";
    float formantMorph = 0.0f;
    float formantAmount = 0.0f;

    // --- Phase 4: pitch-LFO vibrato, applied to the fundamental before the
    // harmonic stack/sub-osc/formant bank consume it — so all three move
    // together. 0 rate or depth = off.
    float vibratoRateHz = 0.0f;
    float vibratoDepthCents = 0.0f;
    float vibratoDelayMs = 0.0f;

    // --- Phase 4: glide/portamento. On NoteOn, if the track already has a
    // remembered frequency (AudioEngine::m_lastTrackFreq) the new voice
    // slides from it to this note's frequency over glideMs instead of
    // jumping. 0 = off (instant retrigger, pre-Phase-4 behavior).
    float glideMs = 0.0f;

    // --- Phase 5: band-limited (polyBLEP) virtual-analog oscillator, summed
    // alongside the additive harmonic stack (see AudioEngine.cpp's unison
    // generation stage) rather than replacing it — oscWave == 0 is "off",
    // reproducing pre-Phase-5 behavior exactly. unisonVoices copies are
    // detuned symmetrically across +/-detuneCents/2 and spread across the
    // stereo field for the classic supersaw width.
    int oscWave = 0;              // 0 = off, 1 = Saw, 2 = Square/Pulse, 3 = Triangle
    int oscUnisonVoices = 1;      // 1..kMaxUnisonVoices (see AudioEngine.h)
    float oscDetuneCents = 0.0f;  // total spread across all unison voices
    float oscPulseWidth = 0.5f;   // 0..1, Square/Pulse wave only

    // Compares only the fields the .adx format actually stores — NOT
    // attackTable/decayTable/releaseTable (or the filter-envelope tables
    // below), which are rebuilt deterministically from their ms/level
    // scalars and have no independent on-disk representation.
    bool operator==(const Patch& other) const {
        return attackMs == other.attackMs && decayMs == other.decayMs &&
               releaseMs == other.releaseMs && sustainLevel == other.sustainLevel &&
               timbreKeyframes == other.timbreKeyframes &&
               filterCutoffHz == other.filterCutoffHz && filterLfoRateHz == other.filterLfoRateHz &&
               filterLfoDepth == other.filterLfoDepth && drive == other.drive &&
               subOscLevel == other.subOscLevel && subOscWave == other.subOscWave &&
               pitchDropSemitones == other.pitchDropSemitones && pitchDropMs == other.pitchDropMs &&
               noiseLevel == other.noiseLevel && noiseType == other.noiseType &&
               resFilterType == other.resFilterType && resFilterCutoff == other.resFilterCutoff &&
               resFilterResonance == other.resFilterResonance && filterEnvAmount == other.filterEnvAmount &&
               keyTrack == other.keyTrack && filterEnvAttackMs == other.filterEnvAttackMs &&
               filterEnvDecayMs == other.filterEnvDecayMs && filterEnvSustainLevel == other.filterEnvSustainLevel &&
               filterEnvReleaseMs == other.filterEnvReleaseMs &&
               formantVowelA == other.formantVowelA && formantVowelB == other.formantVowelB &&
               formantMorph == other.formantMorph && formantAmount == other.formantAmount &&
               vibratoRateHz == other.vibratoRateHz && vibratoDepthCents == other.vibratoDepthCents &&
               vibratoDelayMs == other.vibratoDelayMs && glideMs == other.glideMs &&
               oscWave == other.oscWave && oscUnisonVoices == other.oscUnisonVoices &&
               oscDetuneCents == other.oscDetuneCents && oscPulseWidth == other.oscPulseWidth;
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

    // Per-track level + pan (Phase 1), applied to the track's bus sum right
    // before it's mixed into the master (same point as the insert-effect
    // chain below). Defaults reproduce pre-Phase-1 behavior exactly: full
    // volume, centered.
    float volume = 1.0f;
    float pan = 0.0f;

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

    // Per-track aux sends (Phase 5, SEND=BusName,Amount) into the shared
    // master Delay/Reverb buses — on TOP of this track's normal dry mix
    // into the master (see AudioEngine::process), not instead of it. 0 = no
    // send (pre-Phase-5 behavior).
    float sendDelayAmount = 0.0f;
    float sendReverbAmount = 0.0f;

    bool operator==(const Track& other) const {
        return patchName == other.patchName && notes == other.notes && audioClips == other.audioClips &&
               arp == other.arp && volume == other.volume && pan == other.pan &&
               sendDelayAmount == other.sendDelayAmount && sendReverbAmount == other.sendReverbAmount &&
               effectsEquivalent(other);
    }
};

// Phase 2: breakpoint automation. Curve describes how the SEGMENT STARTING
// at this breakpoint interpolates toward the next one (the last point in a
// lane has no outgoing segment, so its curve is unused).
enum class Curve : uint8_t { Linear, Exponential, Step, Smooth };

struct Breakpoint {
    float beat = 0.0f;
    float value = 0.0f;
    Curve curve = Curve::Linear;

    bool operator==(const Breakpoint&) const = default;
};

// One automated parameter over time. trackTarget names a Track by its
// patchName (the only per-track identity the format has — see `[TRACK Name]`
// in AdxParser), or the literal "MASTER" for master-bus targets. paramTarget
// is a small dotted path resolved by AudioEngine: "mix.<field>",
// "patch.<field>", "effect.<TypeName>.<field>", or "master.<field>".
// Points must stay sorted ascending by beat (AdxParser sorts on load; the UI
// re-sorts after any drag that reorders points).
struct AutomationLane {
    std::string trackTarget;
    std::string paramTarget;
    std::vector<Breakpoint> points;

    bool operator==(const AutomationLane&) const = default;
};

// Arrangement/section marker (Phase 2). Timeline-ruler-only — not shipped to
// the audio thread, since nothing in process() needs it.
struct Marker {
    float beat = 0.0f;
    std::string name;

    bool operator==(const Marker&) const = default;
};

// Evaluates a lane's breakpoint curve at `beat`. Shared by AudioEngine (real
// automation playback) and SequencerUI (lane preview curve) so the two never
// disagree about interpolation shape. Clamps to the first/last point's value
// outside the lane's own beat range.
inline float EvaluateAutomationLane(const AutomationLane& lane, float beat) {
    const auto& pts = lane.points;
    if (pts.empty()) return 0.0f;
    if (beat <= pts.front().beat) return pts.front().value;
    if (beat >= pts.back().beat) return pts.back().value;

    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Breakpoint& a = pts[i];
        const Breakpoint& b = pts[i + 1];
        if (beat < a.beat || beat > b.beat) continue;

        float span = b.beat - a.beat;
        float t = span > 0.0f ? (beat - a.beat) / span : 1.0f;
        switch (a.curve) {
            case Curve::Step:
                return a.value;
            case Curve::Exponential:
                if (a.value > 0.0f && b.value > 0.0f) {
                    return a.value * std::pow(b.value / a.value, t);
                }
                return a.value + (b.value - a.value) * t; // non-positive: exp is undefined, fall back
            case Curve::Smooth: {
                float st = t * t * (3.0f - 2.0f * t);
                return a.value + (b.value - a.value) * st;
            }
            case Curve::Linear:
            default:
                return a.value + (b.value - a.value) * t;
        }
    }
    return pts.back().value;
}

// Tracks + the patch registry they reference, shipped to the audio thread
// together as one AudioEventType::SequenceUpdate payload. A Track only
// stores its patch by name (patchName) — the audio thread resolves that name
// to a const Patch* itself, against a registry whose lifetime IT owns
// (garbage-collected the same way as the tracks), rather than trusting a
// pointer resolved on the Main Thread into state.patches, which a later
// reload/clear could invalidate while the audio thread is still using it.
struct SequenceSnapshot {
    std::vector<Track> tracks;
    std::unordered_map<std::string, Patch> patches;
    std::vector<AutomationLane> automation; // Phase 2
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
    std::vector<AutomationLane> automation; // Phase 2
    std::vector<Marker> markers;            // Phase 2

    // Shared between Main and Audio threads
    // The main thread might reset it, the audio thread increments it
    std::atomic<float> playheadPositionBeats{0.0f};
    std::atomic<bool> isPlaying{false};
    std::atomic<float> bpm{120.0f};

    // Global parameters
    std::atomic<float> masterVolume{0.75f};
    std::atomic<float> tuning{440.0f};

    // live-PLAN Phase L1: sample-accurate loop/cycle region. When enabled,
    // AudioEngine::process wraps NoteOn/NoteOff scheduling at loopEndBeat
    // back to loopStartBeat sample-accurately; already-active voices/delay/
    // reverb ring out through the seam untouched (only future scheduling is
    // affected, not what's already sounding).
    std::atomic<bool> loopEnabled{false};
    std::atomic<float> loopStartBeat{0.0f};
    std::atomic<float> loopEndBeat{4.0f};
};

// --- Thread Synchronization ---

// Event types passed to the audio thread
enum class AudioEventType : uint8_t {
    NoteOn,
    NoteOff,
    ParameterChange,
    PlayStateChange,
    BpmChange,
    SequenceUpdate,
    MasterVolChange,
    GlobalTuningChange,
    LoopChange
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
    // If SequenceUpdate, it holds a pointer to a new SequenceSnapshot (tracks + patch registry).
    union Data {
        // NoteOn payload: the resolved per-track Patch* (looked up from the
        // active SequenceSnapshot when the note was scheduled — see
        // AudioEngine::m_trackPatches), or nullptr. Valid for the note's
        // whole lifetime since the engine pins the owning SequenceSnapshot in
        // its garbage bin until it's safe to free.
        const Patch* patch;

        // SequenceUpdate payload: a fresh tracks+patches snapshot for the
        // audio thread to take ownership of and resolve per-track patches
        // against (see SequenceSnapshot).
        const SequenceSnapshot* sequence;

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

        // LoopChange payload (live-PLAN L1): all three loop fields travel
        // together so the audio thread never sees an enabled loop with a
        // stale start/end pair from a previous event.
        struct {
            bool enabled;
            float startBeat;
            float endBeat;
        } loopState;
    } data;
};

static_assert(std::is_trivially_copyable_v<AudioEvent>, "AudioEvent must be trivially copyable for lock-free passing");

#pragma once

#include "AudioData.h"
#include "AudioEffect.h"
#include <array>
#include <vector>
#include <readerwriterqueue.h>
#include <RtAudio.h>

// Maximum number of tracks that get their own effect bus in process().
// Tracks beyond this cap still play, folded onto the last bus.
constexpr size_t kMaxEngineTracks = 16;

// Phase 3: how many octaves the resonant filter's cutoff sweeps at
// filterEnvAmount == 1.0 and a fully-open filter envelope (envLevel == 1.0).
constexpr float kFilterEnvOctaveRange = 5.0f;

// Phase 4: fixed Q for every formant band's TPT SVF bandpass — a narrow-ish
// resonant peak (not from the file; RESFILTER/FILTERENV carry an explicit
// resonance/env, but FORMANT's format has no per-band Q, so this is a design
// constant like kFilterEnvOctaveRange above). k = 1/Q in the Zavalishin form.
constexpr float kFormantQ = 8.0f;
constexpr float kFormantFilterK = 1.0f / kFormantQ;

// Phase 5: max detuned copies the virtual-analog unison oscillator will
// generate per voice (OSC's UnisonVoices is clamped to this).
constexpr size_t kMaxUnisonVoices = 7;

// Phase 5: how much the unison oscillator's L/R pan spread contributes at
// the track-bus mix stage — see the comment on Voice's stereo-width design
// tradeoff in AudioEngine.cpp's process().
constexpr float kUnisonWidthAmount = 0.6f;

// Represents a single active polyphonic voice
struct Voice {
    bool active = false;
    uint8_t pitch = 0;
    uint8_t velocity = 0;

    // Which track bus this voice sums into (sequence-scheduled notes carry
    // their source track; live/queue NoteOns land on bus 0).
    int trackIndex = 0;

    // DSP state: 16 harmonics
    std::array<float, 16> phase{};
    std::array<float, 16> harmonicAmplitudes{};

    // Envelope state
    EnvState envState = EnvState::Idle;
    float envLevel = 0.0f;
    uint64_t envSampleCount = 0;  // Current sample count in phase

    // Reference to the active patch (read-only)
    const Patch* patch = nullptr;

    // A timestamp to implement voice stealing (lowest number = oldest)
    uint64_t noteOnTimestamp = 0;

    // --- C418 suite: per-voice one-pole LPF + cutoff LFO ---
    float lpfState = 0.0f;
    float lfoPhase = 0.0f; // 0..1

    // --- STAKILLAZ suite: sub oscillator + pitch-drop transient ---
    float subPhase = 0.0f;      // 0..1
    uint64_t ageSamples = 0;    // samples since NoteOn (drives the pitch drop)

    // --- Phase 3: noise oscillator (xorshift32 white, optionally colored
    // pink via a 3-pole IIR). noiseRng must stay non-zero (xorshift property).
    uint32_t noiseRng = 1;
    std::array<float, 3> pinkState{};

    // --- Phase 3: resonant TPT state-variable filter state (two integrators).
    float svfIc1eq = 0.0f;
    float svfIc2eq = 0.0f;

    // --- Phase 3: dedicated filter envelope, independent of the amplitude
    // envelope above (same state-machine shape, own tables/clock).
    EnvState filterEnvState = EnvState::Idle;
    float filterEnvLevel = 0.0f;
    uint64_t filterEnvSampleCount = 0;

    // --- Phase 4: formant bank filter state — 3 parallel bandpass bands
    // (each its own TPT SVF, same design as the Phase 3 resonant filter).
    std::array<float, 3> formantIc1eq{};
    std::array<float, 3> formantIc2eq{};
    // Resolved once at NoteOn from patch->formantVowelA/B (see
    // GetVowelFormants in AudioEngine.cpp) so process() doesn't re-resolve a
    // vowel name by string comparison every sample — only the morph crossfade
    // between them changes per-block via the live-param.
    std::array<float, 3> formantFreqA{};
    std::array<float, 3> formantFreqB{};

    // --- Phase 4: vibrato LFO phase (0..1), applied to the fundamental.
    float vibratoPhase = 0.0f;

    // --- Phase 4: glide/portamento. glideStartFreq is the frequency this
    // voice slides FROM (captured at NoteOn from the track's last note);
    // glideSamplesTotal == 0 means "no glide" (instant, pre-Phase-4 jump).
    float glideStartFreq = 0.0f;
    uint64_t glideSamplesElapsed = 0;
    uint64_t glideSamplesTotal = 0;

    // --- Phase 5: virtual-analog unison oscillator. One phase accumulator
    // per unison voice, plus a leaky-integrator state for the triangle
    // waveform (bandlimited via integrating a polyBLEP square — see
    // GeneratePolyBlepSample in AudioEngine.cpp). Unused slots (beyond the
    // patch's oscUnisonVoices) just sit idle at 0.
    std::array<float, kMaxUnisonVoices> oscUnisonPhase{};
    std::array<float, kMaxUnisonVoices> oscTriIntegrator{};
};

// Phase 2: automation-writable live parameters for the track at a given bus
// index, seeded from that track/patch's static Phase-1 defaults on every
// SequenceUpdate and overwritten every process() block by any AutomationLane
// that targets it. Voices/mix read these instead of the static Track/Patch
// fields wherever a modulation target exists for them. atomic<float> so the
// Main Thread could read live-automated values for UI feedback even though
// only the audio thread ever writes them.
struct LiveTrackParams {
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<float> filterCutoffHz{20000.0f};
    // Phase 3: resonant-filter base cutoff/resonance, seeded from the
    // per-track patch's RESFILTER and read by the SVF stage in process().
    std::atomic<float> resFilterCutoff{20000.0f};
    std::atomic<float> resFilterResonance{0.0f};
    // Phase 4 modulation target: no reader exists yet (FORMANT isn't built),
    // but automation lanes can already target and hold a value here ahead of
    // that phase landing.
    std::atomic<float> formantMorph{0.0f};
};

// Represents the state of the peak compressor
struct CompressorState {
    float thresholdDb = -3.0f;
    float ratio = 4.0f; // 4:1

    // Coefficients calculated from attack/release times
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;

    // Smoothed gain reduction factor [0.0, 1.0]
    float currentGainReduction = 1.0f;

    // --- STAKILLAZ suite: sidechain input (Track 1's bus ducks the master).
    // The follower's envelope multiplies the master by 1 - amount*env before
    // the peak-compression stage — the classic phonk/trashwave pump.
    bool sidechainEnabled = false;
    float sidechainAmount = 0.6f;       // 0..1 duck depth
    float sidechainReleaseCoeff = 0.0f; // one-pole release for the follower
    float sidechainEnv = 0.0f;          // follower state
};

// C418 suite: master stereo ping-pong delay. Buffers are allocated once in
// the AudioEngine constructor (Main Thread) — never in process().
struct MasterDelayState {
    std::vector<float> bufferL;
    std::vector<float> bufferR;
    size_t writeIndex = 0;
};

// M0: shared audio tap. AudioEngine::process() writes the post-mix master
// frame here every sample (after the hard clamp, right before it's handed to
// RtAudio) — one tap instead of SCOPE/SPECTRUM/meters/visualizer each
// wiring up their own. Unlike moodycamel::ReaderWriterQueue (a *consuming*
// FIFO, right for discrete AudioEvents), every future consumer here wants
// the same rolling window of recent history redrawn every UI frame — a
// waveform, an FFT input, an RMS follower — so this is an overwrite-style
// ring: a fixed array plus a monotonically increasing write cursor, with no
// dequeue step. The write cursor is published with release ordering after
// the sample is stored, and readers acquire-load it first, so a reader never
// observes a cursor value pointing past a sample that isn't there yet. If a
// reader is slow, older frames simply get overwritten (dropped) rather than
// the audio thread ever blocking or allocating.
constexpr size_t kAudioTapCapacity = 8192; // frames; power of two for &-mask wrap

struct AudioTapFrame {
    float left = 0.0f;
    float right = 0.0f;
};

class AudioTap {
public:
    // Audio thread only. Never allocates, never blocks.
    void Write(float left, float right) {
        size_t idx = m_writeCursor.load(std::memory_order_relaxed) & (kAudioTapCapacity - 1);
        m_buffer[idx] = AudioTapFrame{ left, right };
        m_writeCursor.fetch_add(1, std::memory_order_release);
    }

    // Any reader thread. Copies up to `count` of the most recently written
    // frames into `out`, oldest-first, and returns how many were actually
    // available (< count only before the tap has filled once at startup).
    size_t ReadLatest(AudioTapFrame* out, size_t count) const {
        count = std::min(count, kAudioTapCapacity);
        size_t writePos = m_writeCursor.load(std::memory_order_acquire);
        size_t available = std::min(count, writePos);
        for (size_t i = 0; i < available; ++i) {
            size_t framesAgo = available - i;
            size_t idx = (writePos - framesAgo) & (kAudioTapCapacity - 1);
            out[i] = m_buffer[idx];
        }
        return available;
    }

private:
    std::array<AudioTapFrame, kAudioTapCapacity> m_buffer{};
    std::atomic<size_t> m_writeCursor{0};
};

class AudioEngine {
public:
    AudioEngine(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, unsigned int sampleRate, std::atomic<float>& playheadPositionBeats);

    // Releases the active sequence snapshot (a heap object the engine took
    // ownership of via a SequenceUpdate event). Only safe once no audio
    // callback can run anymore — i.e. after the stream is closed, or on an
    // offline ExportRenderer engine that was never attached to a stream.
    ~AudioEngine() {
        delete m_activeSequence;
    }

    // The static callback passed to RtAudio
    static int audioCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                             double streamTime, RtAudioStreamStatus status, void* userData);

    // M0: read-only handle to the shared master-output tap (see AudioTap
    // above). UI-thread consumers (SCOPE, SPECTRUM, meters, the particle
    // visualizer) call ReadLatest() on this each frame.
    const AudioTap& GetMasterTap() const { return m_masterTap; }

    // live-PLAN L4: per-track decaying peak-hold, updated in process() at
    // the same post-effect, post-volume/pan mix point MIX=Volume,Pan
    // already applies. UI-thread meters read this once per rendered frame;
    // safe to call with any trackIndex (out-of-range just reads silence).
    float GetTrackPeak(size_t trackIndex) const {
        return trackIndex < kMaxEngineTracks ? m_trackPeaks[trackIndex].load(std::memory_order_relaxed) : 0.0f;
    }

private:
    // Instance method performing the actual DSP work
    int process(float* outputBuffer, unsigned int nFrames);

    // Event handling
    void handleNoteOn(const AudioEvent& event, int trackIndex = 0);
    void handleNoteOff(const AudioEvent& event);
    void handleParameterChange(const AudioEvent& event);

    // Voice allocation / Stealing
    size_t allocateVoice();

    // Phase 2: automation engine. Evaluates every lane in m_activeSequence at
    // `beat` and writes the result to its resolved target, once per process()
    // block (not per-sample).
    void evaluateAutomation(double beat);
    void applyMasterAutomationTarget(const std::string& paramTarget, float value);
    void applyTrackAutomationTarget(int busIdx, const std::string& paramTarget, float value);
    // Reseeds m_liveParams from the track/patch defaults for a freshly
    // resolved sequence — called once from the SequenceUpdate handler, not
    // per-block, so a lane that never fires (target never matched) still
    // leaves the pre-Phase-2 static value in effect.
    void resetLiveParams();

    // Utility DSP Math
    float midiToFreq(uint8_t midiNote) const;
    void calculateCompressorCoefficients(unsigned int sampleRate);

    // References to externally provided queue
    moodycamel::ReaderWriterQueue<AudioEvent>& m_eventQueue;
    std::atomic<float>& m_playheadPositionBeats;

    // Audio context
    unsigned int m_sampleRate;
    uint64_t m_globalSampleCounter; // Used for timestamps

    // Engine state
    std::array<Voice, 64> m_voices;
    CompressorState m_compressor;

    // --- Master FX chain (C418 delay/reverb, STAKILLAZ drive), applied after
    // the per-track bus mix and before the sidechain/compressor stages.
    // M0: shared master-output tap (see AudioTap above). Written once per
    // sample in process(), after every other master-chain stage.
    AudioTap m_masterTap;

    MasterDelayState m_delay;
    float m_delayTimeMs = 350.0f;
    float m_delayFeedback = 0.35f;
    float m_delayMix = 0.0f;   // 0 = bypass
    ReverbEffect m_masterReverb{0.0f, 0.8f, 0.5f}; // mix 0 = bypass; params set via events
    float m_masterDrive = 0.0f; // 0 = bypass

    // Sequencer state
    bool m_isPlaying = false;
    float m_bpm = 120.0f;
    double m_currentSamplePosition = 0.0;

    // live-PLAN Phase L1: loop/cycle region, mirrored from SequencerState via
    // AudioEventType::LoopChange. Plain (non-atomic) members — only ever
    // touched here, on the audio thread, after being drained off the queue.
    bool m_loopEnabled = false;
    float m_loopStartBeat = 0.0f;
    float m_loopEndBeat = 4.0f;

    // The active tracks+patches snapshot (Phase 1: per-track patches replace
    // the old single shared m_activePatch). Owned by the engine.
    const SequenceSnapshot* m_activeSequence = nullptr;

    // Per-bus resolved patch pointers into m_activeSequence->patches,
    // rebuilt (by name lookup, no allocation) whenever m_activeSequence
    // changes. Indexed by the same busIdx voices/scheduled notes use.
    std::array<const Patch*, kMaxEngineTracks> m_trackPatches{};

    // Phase 2: per-bus automation-writable live parameters (see LiveTrackParams).
    std::array<LiveTrackParams, kMaxEngineTracks> m_liveParams{};

    // live-PLAN L4: per-bus decaying peak-hold (see GetTrackPeak above).
    // atomic<float> so the UI thread can read it lock-free; audio-thread-only
    // writes, one-pole release so a meter reading it once per UI frame sees a
    // smoothly decaying value instead of near-silence between transients.
    std::array<std::atomic<float>, kMaxEngineTracks> m_trackPeaks{};

    // Phase 4: per-bus "last triggered frequency" memory, used by glide to
    // know what to slide FROM when a new note starts on a track whose patch
    // has glideMs > 0. 0.0f (never triggered) means "no glide, this is the
    // first note on this bus".
    std::array<float, kMaxEngineTracks> m_lastTrackFreq{};

    // Global parameters
    float m_masterVolume = 0.75f;
    float m_tuning = 440.0f;

    // Garbage collection bin for replaced sequence snapshots (tracks +
    // patches), safely surrendered to the Main Thread to free once no audio
    // callback can be mid-flight against the old pointer.
    std::vector<std::unique_ptr<const SequenceSnapshot>> m_sequenceGarbageBin;

    // Allow main function to access garbage bins for cleanup
    friend int main(int, char**);
};

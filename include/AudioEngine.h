#pragma once

#include "AudioData.h"
#include <array>
#include <readerwriterqueue.h>
#include <RtAudio.h>

// Represents a single active polyphonic voice
struct Voice {
    bool active = false;
    uint8_t pitch = 0;
    uint8_t velocity = 0;

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
};

class AudioEngine {
public:
    AudioEngine(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, unsigned int sampleRate, std::atomic<float>& playheadPositionBeats);
    ~AudioEngine() = default;

    // The static callback passed to RtAudio
    static int audioCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                             double streamTime, RtAudioStreamStatus status, void* userData);

private:
    // Instance method performing the actual DSP work
    int process(float* outputBuffer, unsigned int nFrames);

    // Event handling
    void handleNoteOn(const AudioEvent& event);
    void handleNoteOff(const AudioEvent& event);
    void handleParameterChange(const AudioEvent& event);

    // Voice allocation / Stealing
    size_t allocateVoice();

    // Utility DSP Math
    static float midiToFreq(uint8_t midiNote);
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

    // The active patch used for new notes
    const Patch* m_activePatch = nullptr;

    // Sequencer state
    bool m_isPlaying = false;
    float m_bpm = 120.0f;
    double m_currentSamplePosition = 0.0;
    const Track* m_activeSequence = nullptr;

    // Garbage collection bin for replaced patches to prevent data races during live editing
    std::vector<std::unique_ptr<const Patch>> m_patchGarbageBin;

    // Garbage collection bin for replaced sequences
    std::vector<std::unique_ptr<const Track>> m_sequenceGarbageBin;

    // Allow main function to access garbage bins for cleanup
    friend int main();
};

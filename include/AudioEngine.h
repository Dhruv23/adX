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

    // DSP state
    float phase = 0.0f;
    float phaseInc = 0.0f; // Phase increment per sample

    // Envelope state
    EnvState envState = EnvState::Idle;
    float envLevel = 0.0f;
    uint64_t envSamplesTotal = 0; // Total length of current phase in samples
    uint64_t envSampleCount = 0;  // Current sample count in phase

    // Temporary copy of patch parameters for this voice
    float attackTime = 0.0f;
    float decayTime = 0.0f;
    float sustainLevel = 0.0f;
    float releaseTime = 0.0f;

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
    AudioEngine(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, unsigned int sampleRate);
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

    // Audio context
    unsigned int m_sampleRate;
    uint64_t m_globalSampleCounter; // Used for timestamps

    // Engine state
    std::array<Voice, 64> m_voices;
    CompressorState m_compressor;
};

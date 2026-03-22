#include "AudioEngine.h"
#include <cmath>
#include <algorithm>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

AudioEngine::AudioEngine(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, unsigned int sampleRate)
    : m_eventQueue(eventQueue), m_sampleRate(sampleRate), m_globalSampleCounter(0) {
    calculateCompressorCoefficients(sampleRate);
}

int AudioEngine::audioCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                               double streamTime, RtAudioStreamStatus status, void* userData) {
    (void)inputBuffer;
    (void)streamTime;
    (void)status;

    AudioEngine* engine = static_cast<AudioEngine*>(userData);
    float* out = static_cast<float*>(outputBuffer);

    return engine->process(out, nFrames);
}

int AudioEngine::process(float* outputBuffer, unsigned int nFrames) {
    // 1. Drain the lock-free queue for any incoming events
    AudioEvent event;
    while (m_eventQueue.try_dequeue(event)) {
        if (event.type == AudioEventType::NoteOn) {
            handleNoteOn(event);
        } else if (event.type == AudioEventType::NoteOff) {
            handleNoteOff(event);
        } else if (event.type == AudioEventType::ParameterChange) {
            handleParameterChange(event);
        }
    }

    // 2. Process Audio
    for (unsigned int i = 0; i < nFrames; ++i) {
        float sampleLeft = 0.0f;
        float sampleRight = 0.0f;

        // Summation: Iterate over all active voices
        for (auto& voice : m_voices) {
            if (!voice.active) continue;

            // Generate oscillator sample (simple sine wave)
            float oscVal = std::sin(voice.phase * 2.0f * static_cast<float>(M_PI));

            // Envelope calculation (linear segments)
            if (voice.envState == EnvState::Attack) {
                if (voice.envSampleCount < voice.envSamplesTotal) {
                    voice.envLevel = static_cast<float>(voice.envSampleCount) / static_cast<float>(voice.envSamplesTotal);
                    voice.envSampleCount++;
                } else {
                    voice.envState = EnvState::Decay;
                    voice.envSampleCount = 0;
                    voice.envSamplesTotal = static_cast<uint64_t>(voice.decayTime * m_sampleRate);
                }
            } else if (voice.envState == EnvState::Decay) {
                if (voice.envSampleCount < voice.envSamplesTotal) {
                    float ratio = static_cast<float>(voice.envSampleCount) / static_cast<float>(voice.envSamplesTotal);
                    voice.envLevel = 1.0f - ratio * (1.0f - voice.sustainLevel);
                    voice.envSampleCount++;
                } else {
                    voice.envState = EnvState::Sustain;
                    voice.envLevel = voice.sustainLevel;
                }
            } else if (voice.envState == EnvState::Sustain) {
                // Hold at sustain level until NoteOff
                voice.envLevel = voice.sustainLevel;
            } else if (voice.envState == EnvState::Release) {
                if (voice.envSampleCount < voice.envSamplesTotal) {
                    float ratio = static_cast<float>(voice.envSampleCount) / static_cast<float>(voice.envSamplesTotal);
                    // Fade from current level (could be sustain, or interrupted attack/decay) to 0
                    // For simplicity, we assume we fade from sustain level.
                    voice.envLevel = voice.sustainLevel * (1.0f - ratio);
                    voice.envSampleCount++;
                } else {
                    // Release finished, mark voice inactive
                    voice.envState = EnvState::Idle;
                    voice.envLevel = 0.0f;
                    voice.active = false;
                }
            }

            // Apply envelope and velocity (normalized 0-1)
            float velNorm = static_cast<float>(voice.velocity) / 127.0f;
            float currentSample = oscVal * voice.envLevel * velNorm;

            // Pan center
            sampleLeft += currentSample * 0.5f;
            sampleRight += currentSample * 0.5f;

            // Update phase
            voice.phase += voice.phaseInc;
            if (voice.phase >= 1.0f) {
                voice.phase -= 1.0f;
            }
        }

        // --- Peak Compressor ---
        // Find peak amplitude across both channels
        float peak = std::max(std::abs(sampleLeft), std::abs(sampleRight));

        // Convert to dB
        float peakDb = peak > 0.00001f ? 20.0f * std::log10(peak) : -100.0f;

        float targetGainReduction = 1.0f; // Default: no reduction

        if (peakDb > m_compressor.thresholdDb) {
            // Calculate how far above threshold we are
            float overDb = peakDb - m_compressor.thresholdDb;

            // Apply ratio to find target dB output
            float outputDb = m_compressor.thresholdDb + (overDb / m_compressor.ratio);

            // Difference is the gain reduction required in dB
            float reductionDb = outputDb - peakDb;

            // Convert dB reduction back to linear multiplier
            targetGainReduction = std::pow(10.0f, reductionDb / 20.0f);
        }

        // Apply one-pole low-pass filter to smooth the reduction factor
        if (targetGainReduction < m_compressor.currentGainReduction) {
            // Attack phase (gain is decreasing to compress signal)
            m_compressor.currentGainReduction = m_compressor.attackCoeff * (m_compressor.currentGainReduction - targetGainReduction) + targetGainReduction;
        } else {
            // Release phase (gain is increasing to return to unity)
            m_compressor.currentGainReduction = m_compressor.releaseCoeff * (m_compressor.currentGainReduction - targetGainReduction) + targetGainReduction;
        }

        // Apply gain reduction
        sampleLeft *= m_compressor.currentGainReduction;
        sampleRight *= m_compressor.currentGainReduction;

        // --- Hard Clamp ---
        sampleLeft = std::clamp(sampleLeft, -1.0f, 1.0f);
        sampleRight = std::clamp(sampleRight, -1.0f, 1.0f);

        // Output to interleaved channels
        *outputBuffer++ = sampleLeft;
        *outputBuffer++ = sampleRight;

        m_globalSampleCounter++;
    }

    return 0; // Continue stream
}

void AudioEngine::handleNoteOn(const AudioEvent& event) {
    size_t voiceIdx = allocateVoice();

    // Initialize voice state
    m_voices[voiceIdx].active = true;
    m_voices[voiceIdx].pitch = event.pitch;
    m_voices[voiceIdx].velocity = event.velocity;
    m_voices[voiceIdx].phaseInc = midiToFreq(event.pitch) / static_cast<float>(m_sampleRate);
    m_voices[voiceIdx].phase = 0.0f;
    m_voices[voiceIdx].noteOnTimestamp = m_globalSampleCounter;

    // Load simplified ADSR
    m_voices[voiceIdx].attackTime = event.data.patch.attackTime;
    m_voices[voiceIdx].decayTime = event.data.patch.decayTime;
    m_voices[voiceIdx].sustainLevel = event.data.patch.sustainLevel;
    m_voices[voiceIdx].releaseTime = event.data.patch.releaseTime;

    // Start Attack Phase
    m_voices[voiceIdx].envState = EnvState::Attack;
    m_voices[voiceIdx].envLevel = 0.0f;
    m_voices[voiceIdx].envSampleCount = 0;
    m_voices[voiceIdx].envSamplesTotal = static_cast<uint64_t>(m_voices[voiceIdx].attackTime * m_sampleRate);

    // Edge case: zero attack time
    if (m_voices[voiceIdx].envSamplesTotal == 0) {
        m_voices[voiceIdx].envState = EnvState::Decay;
        m_voices[voiceIdx].envLevel = 1.0f;
        m_voices[voiceIdx].envSamplesTotal = static_cast<uint64_t>(m_voices[voiceIdx].decayTime * m_sampleRate);
    }
}

void AudioEngine::handleNoteOff(const AudioEvent& event) {
    // Find all active voices matching this pitch and put them into release
    for (auto& voice : m_voices) {
        if (voice.active && voice.pitch == event.pitch && voice.envState != EnvState::Release) {
            voice.envState = EnvState::Release;

            // For simplicity, we capture the current level as the starting point for release
            // In a more complex envelope, we'd interpolate from current level to 0 over releaseTime
            // For this linear segment, we pretend we are fading from sustainLevel down to 0
            // but we adjust samples total to match current level if we aren't at sustain.

            // To properly linear fade from *current* level:
            // Let's store the current level instead of assuming sustain
            voice.sustainLevel = voice.envLevel;

            voice.envSampleCount = 0;
            voice.envSamplesTotal = static_cast<uint64_t>(voice.releaseTime * m_sampleRate);

            if (voice.envSamplesTotal == 0) {
                voice.active = false;
                voice.envLevel = 0.0f;
                voice.envState = EnvState::Idle;
            }
        }
    }
}

void AudioEngine::handleParameterChange(const AudioEvent& event) {
    (void)event;
    // Parameter updates will be handled here
}

size_t AudioEngine::allocateVoice() {
    // 1. Look for an inactive voice
    for (size_t i = 0; i < m_voices.size(); ++i) {
        if (!m_voices[i].active) {
            return i;
        }
    }

    // 2. All active. Voice Stealing: Try to find the oldest voice in Release
    size_t oldestReleaseIdx = m_voices.size();
    uint64_t oldestReleaseTime = std::numeric_limits<uint64_t>::max();

    for (size_t i = 0; i < m_voices.size(); ++i) {
        if (m_voices[i].envState == EnvState::Release) {
            if (m_voices[i].noteOnTimestamp < oldestReleaseTime) {
                oldestReleaseTime = m_voices[i].noteOnTimestamp;
                oldestReleaseIdx = i;
            }
        }
    }

    if (oldestReleaseIdx < m_voices.size()) {
        return oldestReleaseIdx;
    }

    // 3. None in release. Steal the absolute oldest voice
    size_t oldestIdx = 0;
    uint64_t oldestTime = m_voices[0].noteOnTimestamp;

    for (size_t i = 1; i < m_voices.size(); ++i) {
        if (m_voices[i].noteOnTimestamp < oldestTime) {
            oldestTime = m_voices[i].noteOnTimestamp;
            oldestIdx = i;
        }
    }

    return oldestIdx;
}

float AudioEngine::midiToFreq(uint8_t midiNote) {
    return 440.0f * std::pow(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
}

void AudioEngine::calculateCompressorCoefficients(unsigned int sampleRate) {
    // 5ms attack, 50ms release
    float attackTime = 0.005f;
    float releaseTime = 0.050f;

    // Standard DSP one-pole filter coefficients
    m_compressor.attackCoeff = std::exp(-1.0f / (attackTime * sampleRate));
    m_compressor.releaseCoeff = std::exp(-1.0f / (releaseTime * sampleRate));
}

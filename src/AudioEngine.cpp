#include "AudioEngine.h"
#include <cmath>
#include <algorithm>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

AudioEngine::AudioEngine(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, unsigned int sampleRate, std::atomic<float>& playheadPositionBeats)
    : m_eventQueue(eventQueue), m_playheadPositionBeats(playheadPositionBeats), m_sampleRate(sampleRate), m_globalSampleCounter(0) {
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
        } else if (event.type == AudioEventType::PatchUpdate) {
            if (event.data.patch) {
                if (m_activePatch) {
                    m_patchGarbageBin.push_back(std::unique_ptr<const Patch>(m_activePatch));
                }
                m_activePatch = event.data.patch;

                // Also update any active voices using the old patch to the new one seamlessly
                for (auto& voice : m_voices) {
                    if (voice.active && voice.patch) {
                        voice.patch = m_activePatch;
                    }
                }
            }
        } else if (event.type == AudioEventType::PlayStateChange) {
            m_isPlaying = event.data.playState.isPlaying;
            if (!m_isPlaying) {
                // When stopped, reset playhead and kill all active sequence voices
                m_currentSamplePosition = 0.0;
                for (auto& voice : m_voices) {
                    if (voice.active) { // Optionally only kill sequence voices, but usually stop kills all
                        voice.envState = EnvState::Release;
                        voice.envSampleCount = 0;
                        if (!voice.patch || voice.patch->releaseTable.empty()) {
                            voice.active = false;
                            voice.envLevel = 0.0f;
                            voice.envState = EnvState::Idle;
                        } else {
                            voice.envLevel = voice.envLevel * voice.patch->releaseTable[0];
                        }
                    }
                }
            }
        } else if (event.type == AudioEventType::BpmChange) {
            m_bpm = event.data.bpmState.bpm;
        } else if (event.type == AudioEventType::MasterVolChange) {
            m_masterVolume = event.data.masterVol.volume;
        } else if (event.type == AudioEventType::GlobalTuningChange) {
            m_tuning = event.data.globalTuning.tuning;
        } else if (event.type == AudioEventType::SequenceUpdate) {
            if (event.data.track) {
                if (m_activeSequence) {
                    m_sequenceGarbageBin.push_back(std::unique_ptr<const Track>(m_activeSequence));
                }
                m_activeSequence = event.data.track;

                // When sequence changes, to be safe, cut active sequence notes to prevent stuck notes
                // For a more robust approach, we could tag voices triggered by the sequence, but
                // for simplicity we'll just send all notes to release to prevent infinite hangs.
                for (auto& voice : m_voices) {
                    if (voice.active && voice.envState != EnvState::Release) {
                        voice.envState = EnvState::Release;
                        voice.envSampleCount = 0;
                        if (!voice.patch || voice.patch->releaseTable.empty()) {
                            voice.active = false;
                            voice.envLevel = 0.0f;
                            voice.envState = EnvState::Idle;
                        } else {
                            voice.envLevel = voice.envLevel * voice.patch->releaseTable[0];
                        }
                    }
                }
            }
        }
    }

    // Calculate samples per beat for sequence playback
    double samplesPerBeat = (m_sampleRate * 60.0) / m_bpm;

    // --- Sequencer Event Pre-calculation ---
    // Instead of evaluating floats per-sample, we find all NoteOn/NoteOff events
    // that occur within this entire audio block [m_currentSamplePosition, m_currentSamplePosition + nFrames)
    struct ScheduledEvent {
        unsigned int sampleOffset;
        bool isNoteOn;
        uint8_t pitch;
        uint8_t velocity;
    };
    std::vector<ScheduledEvent> scheduledEvents;

    if (m_isPlaying && m_activeSequence) {
        double blockStartBeat = m_currentSamplePosition / samplesPerBeat;
        double blockEndBeat = (m_currentSamplePosition + nFrames) / samplesPerBeat;

        for (const auto& note : m_activeSequence->notes) {
            double noteStartBeat = note.startBeat;
            double noteEndBeat = note.startBeat + note.lengthBeats;

            // Check Note On
            if (noteStartBeat >= blockStartBeat && noteStartBeat < blockEndBeat) {
                double beatOffset = noteStartBeat - blockStartBeat;
                unsigned int sampleOffset = static_cast<unsigned int>(beatOffset * samplesPerBeat);
                if (sampleOffset < nFrames) {
                    scheduledEvents.push_back({sampleOffset, true, note.pitch, note.velocity});
                }
            }

            // Check Note Off
            if (noteEndBeat >= blockStartBeat && noteEndBeat < blockEndBeat) {
                double beatOffset = noteEndBeat - blockStartBeat;
                unsigned int sampleOffset = static_cast<unsigned int>(beatOffset * samplesPerBeat);
                if (sampleOffset < nFrames) {
                    scheduledEvents.push_back({sampleOffset, false, note.pitch, note.velocity});
                }
            }
        }

        // Advance global position by block size
        m_currentSamplePosition += nFrames;
    }

    // 2. Process Audio
    for (unsigned int i = 0; i < nFrames; ++i) {

        // Dispatch scheduled events for this specific sample
        for (const auto& ev : scheduledEvents) {
            if (ev.sampleOffset == i) {
                if (ev.isNoteOn) {
                    AudioEvent onEvt{};
                    onEvt.type = AudioEventType::NoteOn;
                    onEvt.pitch = ev.pitch;
                    onEvt.velocity = ev.velocity;
                    onEvt.data.patch = nullptr;
                    handleNoteOn(onEvt);
                } else {
                    AudioEvent offEvt{};
                    offEvt.type = AudioEventType::NoteOff;
                    offEvt.pitch = ev.pitch;
                    offEvt.velocity = ev.velocity;
                    handleNoteOff(offEvt);
                }
            }
        }

        float sampleLeft = 0.0f;
        float sampleRight = 0.0f;

        // Summation: Iterate over all active voices
        for (auto& voice : m_voices) {
            if (!voice.active) continue;

            float oscVal = 0.0f;
            float fundamentalFreq = midiToFreq(voice.pitch);
            float basePhaseInc = fundamentalFreq / static_cast<float>(m_sampleRate);

            // Generate oscillator sample (Additive synthesis, up to 16 harmonics)
            for (size_t h = 0; h < 16; ++h) {
                if (voice.harmonicAmplitudes[h] > 0.0001f) {
                    oscVal += std::sin(voice.phase[h] * 2.0f * static_cast<float>(M_PI)) * voice.harmonicAmplitudes[h];
                }

                // Update phase for this harmonic (fundamentalFreq * (h + 1))
                voice.phase[h] += basePhaseInc * static_cast<float>(h + 1);
                if (voice.phase[h] >= 1.0f) {
                    voice.phase[h] -= 1.0f;
                }
            }

            // Envelope calculation (Lookup tables)
            if (voice.envState == EnvState::Attack) {
                if (voice.patch && voice.envSampleCount < voice.patch->attackTable.size()) {
                    voice.envLevel = voice.patch->attackTable[voice.envSampleCount];
                    voice.envSampleCount++;
                } else {
                    voice.envState = EnvState::Decay;
                    voice.envSampleCount = 0;
                    if (voice.patch && !voice.patch->decayTable.empty()) {
                        voice.envLevel = voice.patch->decayTable[0];
                    } else {
                        voice.envLevel = voice.patch ? voice.patch->sustainLevel : 1.0f;
                    }
                }
            } else if (voice.envState == EnvState::Decay) {
                if (voice.patch && voice.envSampleCount < voice.patch->decayTable.size()) {
                    voice.envLevel = voice.patch->decayTable[voice.envSampleCount];
                    voice.envSampleCount++;
                } else {
                    voice.envState = EnvState::Sustain;
                    voice.envLevel = voice.patch ? voice.patch->sustainLevel : 1.0f;
                }
            } else if (voice.envState == EnvState::Sustain) {
                // Hold at sustain level until NoteOff
                voice.envLevel = voice.patch ? voice.patch->sustainLevel : 1.0f;
            } else if (voice.envState == EnvState::Release) {
                if (voice.patch && voice.envSampleCount < voice.patch->releaseTable.size()) {
                    // Assuming release table goes from 1.0 down to 0.0, we scale it by current envLevel
                    // (But handleNoteOff already scaled the start, so if the table is normalized 0-1 it's tricky.
                    //  Let's assume the table itself contains the absolute envelope multiplier if we started from 1.0.
                    //  To be robust against releasing early, we multiply the table value by the level we had right before release.)
                    // Wait, handleNoteOff just set voice.envLevel = voice.envLevel * releaseTable[0].
                    // Let's just use the table value directly scaled by the sustain level if we were in sustain,
                    // or better yet, scale the normalized release table by the level captured at note off.
                    // For now, let's just assume the table provides the exact multiplier 1.0 -> 0.0.
                    // The simplest approach is to track a "releaseStartLevel" and multiply, but we don't have that in Voice.
                    // So let's just use the table value directly. The table should go 1.0 to 0.0,
                    // and we scale it by `voice.patch->sustainLevel` if that's what was expected.
                    // For simplicity as requested, we just index the table. The UI should populate the table to match levels.
                    voice.envLevel = voice.patch->releaseTable[voice.envSampleCount];
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

        // Apply master volume
        sampleLeft *= m_masterVolume;
        sampleRight *= m_masterVolume;

        // --- Hard Clamp ---
        sampleLeft = std::clamp(sampleLeft, -1.0f, 1.0f);
        sampleRight = std::clamp(sampleRight, -1.0f, 1.0f);

        // Output to interleaved channels
        *outputBuffer++ = sampleLeft;
        *outputBuffer++ = sampleRight;

        m_globalSampleCounter++;
    }

    // After processing the block, update the global playhead position
    // if the sequencer is playing. We'll use relaxed memory ordering
    // because this is for the UI to read loosely, not for strict sync.
    if (m_isPlaying) {
        float currentBeatFloat = static_cast<float>(m_currentSamplePosition / samplesPerBeat);
        m_playheadPositionBeats.store(currentBeatFloat, std::memory_order_relaxed);
    } else {
        m_playheadPositionBeats.store(0.0f, std::memory_order_relaxed);
    }

    return 0; // Continue stream
}

void AudioEngine::handleNoteOn(const AudioEvent& event) {
    size_t voiceIdx = allocateVoice();
    Voice& voice = m_voices[voiceIdx];

    // Initialize voice state
    voice.active = true;
    voice.pitch = event.pitch;
    voice.velocity = event.velocity;
    voice.noteOnTimestamp = m_globalSampleCounter;
    voice.patch = event.data.patch ? event.data.patch : m_activePatch;

    // Reset phases
    for (size_t i = 0; i < 16; ++i) {
        voice.phase[i] = 0.0f;
        voice.harmonicAmplitudes[i] = 0.0f;
    }

    // Interpolate harmonic amplitudes from keyframes
    if (voice.patch && !voice.patch->timbreKeyframes.empty()) {
        const auto& keyframes = voice.patch->timbreKeyframes;
        if (keyframes.size() == 1) {
            // Only one keyframe, copy its amplitudes
            for (size_t i = 0; i < 16 && i < keyframes[0].harmonics.size(); ++i) {
                voice.harmonicAmplitudes[i] = keyframes[0].harmonics[i];
            }
        } else {
            // Find the two closest keyframes
            const TimbreKeyframe* kf1 = &keyframes.front();
            const TimbreKeyframe* kf2 = &keyframes.back();

            for (size_t i = 0; i < keyframes.size() - 1; ++i) {
                if (event.pitch >= keyframes[i].midiNote && event.pitch <= keyframes[i+1].midiNote) {
                    kf1 = &keyframes[i];
                    kf2 = &keyframes[i+1];
                    break;
                }
                // If pitch is lower than first keyframe or higher than last, it will extrapolate or cap
                if (event.pitch < keyframes.front().midiNote) {
                    kf1 = &keyframes.front();
                    kf2 = &keyframes.front(); // Cap at bottom
                } else if (event.pitch > keyframes.back().midiNote) {
                    kf1 = &keyframes.back();
                    kf2 = &keyframes.back();  // Cap at top
                }
            }

            if (kf1 == kf2) {
                for (size_t i = 0; i < 16 && i < kf1->harmonics.size(); ++i) {
                    voice.harmonicAmplitudes[i] = kf1->harmonics[i];
                }
            } else {
                // Interpolate
                float t = static_cast<float>(event.pitch - kf1->midiNote) / static_cast<float>(kf2->midiNote - kf1->midiNote);
                for (size_t i = 0; i < 16; ++i) {
                    float val1 = (i < kf1->harmonics.size()) ? kf1->harmonics[i] : 0.0f;
                    float val2 = (i < kf2->harmonics.size()) ? kf2->harmonics[i] : 0.0f;
                    voice.harmonicAmplitudes[i] = std::lerp(val1, val2, t);
                }
            }
        }
    } else {
        // Fallback: simple fundamental sine if no keyframes
        voice.harmonicAmplitudes[0] = 1.0f;
    }

    // Envelope state initialization
    voice.envSampleCount = 0;

    if (voice.patch && !voice.patch->attackTable.empty()) {
        voice.envState = EnvState::Attack;
        voice.envLevel = voice.patch->attackTable[0];
    } else if (voice.patch && !voice.patch->decayTable.empty()) {
        voice.envState = EnvState::Decay;
        voice.envLevel = voice.patch->decayTable[0];
    } else {
        // If no attack or decay tables, jump straight to sustain
        voice.envState = EnvState::Sustain;
        if (voice.patch) {
            voice.envLevel = voice.patch->sustainLevel;
        } else {
            voice.envLevel = 1.0f;
        }
    }
}

void AudioEngine::handleNoteOff(const AudioEvent& event) {
    // Find all active voices matching this pitch and put them into release
    for (auto& voice : m_voices) {
        if (voice.active && voice.pitch == event.pitch && voice.envState != EnvState::Release) {
            voice.envState = EnvState::Release;
            voice.envSampleCount = 0;

            if (!voice.patch || voice.patch->releaseTable.empty()) {
                voice.active = false;
                voice.envLevel = 0.0f;
                voice.envState = EnvState::Idle;
            } else {
                voice.envLevel = voice.envLevel * voice.patch->releaseTable[0];
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

float AudioEngine::midiToFreq(uint8_t midiNote) const {
    return m_tuning * std::pow(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
}

void AudioEngine::calculateCompressorCoefficients(unsigned int sampleRate) {
    // 5ms attack, 50ms release
    float attackTime = 0.005f;
    float releaseTime = 0.050f;

    // Standard DSP one-pole filter coefficients
    m_compressor.attackCoeff = std::exp(-1.0f / (attackTime * sampleRate));
    m_compressor.releaseCoeff = std::exp(-1.0f / (releaseTime * sampleRate));
}

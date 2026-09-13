#include "AudioEngine.h"
#include "AudioEffect.h"
#include <cmath>
#include <algorithm>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Phase 4: classic vowel formant center frequencies (F1, F2, F3 in Hz) —
// standard textbook approximations (Peterson & Barney-style averages), not
// derived from suffocation.adx (FORMANT's format just names a vowel, it
// doesn't carry frequencies). Unrecognized names fall back to "Ah".
static std::array<float, 3> GetVowelFormants(const std::string& vowel) {
    if (vowel == "Ee") return {270.0f, 2290.0f, 3010.0f};
    if (vowel == "Eh") return {530.0f, 1840.0f, 2480.0f};
    if (vowel == "Oh") return {570.0f, 840.0f, 2410.0f};
    if (vowel == "Oo") return {300.0f, 870.0f, 2240.0f};
    if (vowel == "Uh") return {640.0f, 1190.0f, 2390.0f};
    return {700.0f, 1220.0f, 2600.0f}; // "Ah" and unrecognized names
}

// Phase 4: relative gain per formant band when summing the bank's bandpass
// outputs — F1 carries most of a vowel's perceived body, F3 the least, so
// weight the sum accordingly (design choice, not file-specified).
static constexpr std::array<float, 3> kFormantBandGain = {1.0f, 0.6f, 0.35f};

// Phase 5: single-sample polyBLEP discontinuity correction (Valimaki-style
// 2-sample-wide polynomial patch around a naive waveform's jump), applied at
// normalized phase t with per-sample phase increment dt.
static inline float PolyBlep(float t, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// Phase 5: one unison voice's raw band-limited waveform sample at its own
// phase/phaseInc (phase is advanced by the caller, matching the additive
// harmonic loop's style above). Triangle is a leaky-integrated bandlimited
// square (a standard "trivial integration" trick — integrating a bandlimited
// square yields a bandlimited triangle; the small leak each sample prevents
// the integrator from drifting off with DC bias over a long render).
// triIntegrator is that unison voice's own persistent integrator state.
static inline float GeneratePolyBlepSample(float phase, float phaseInc, int wave, float pulseWidth, float& triIntegrator) {
    if (wave == 1) { // Saw
        float naive = 2.0f * phase - 1.0f;
        return naive - PolyBlep(phase, phaseInc);
    }
    if (wave == 2) { // Square / Pulse
        float pw = std::clamp(pulseWidth, 0.05f, 0.95f);
        float naive = phase < pw ? 1.0f : -1.0f;
        naive += PolyBlep(phase, phaseInc);
        float t2 = phase + (1.0f - pw);
        if (t2 >= 1.0f) t2 -= 1.0f;
        naive -= PolyBlep(t2, phaseInc);
        return naive;
    }
    if (wave == 3) { // Triangle: leaky-integrated 50%-duty bandlimited square
        float sq = phase < 0.5f ? 1.0f : -1.0f;
        sq += PolyBlep(phase, phaseInc);
        float t2 = phase + 0.5f;
        if (t2 >= 1.0f) t2 -= 1.0f;
        sq -= PolyBlep(t2, phaseInc);
        triIntegrator += 4.0f * phaseInc * sq;
        triIntegrator -= triIntegrator * 0.002f; // gentle leak, prevents DC drift
        return triIntegrator;
    }
    return 0.0f;
}

AudioEngine::AudioEngine(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, unsigned int sampleRate, std::atomic<float>& playheadPositionBeats)
    : m_eventQueue(eventQueue), m_playheadPositionBeats(playheadPositionBeats), m_sampleRate(sampleRate), m_globalSampleCounter(0) {
    calculateCompressorCoefficients(sampleRate);

    // Master delay: 2 seconds of stereo buffer, allocated here on the Main
    // Thread before the stream starts — process() only indexes into it.
    m_delay.bufferL.assign(sampleRate * 2, 0.0f);
    m_delay.bufferR.assign(sampleRate * 2, 0.0f);

    // Sidechain follower release (default 120ms; updated via events)
    m_compressor.sidechainReleaseCoeff = std::exp(-1.0f / (0.120f * sampleRate));
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
        } else if (event.type == AudioEventType::LoopChange) {
            m_loopEnabled = event.data.loopState.enabled;
            m_loopStartBeat = event.data.loopState.startBeat;
            m_loopEndBeat = event.data.loopState.endBeat;
        } else if (event.type == AudioEventType::SequenceUpdate) {
            if (event.data.sequence) {
                if (m_activeSequence) {
                    m_sequenceGarbageBin.push_back(std::unique_ptr<const SequenceSnapshot>(m_activeSequence));
                }
                m_activeSequence = event.data.sequence;

                // Resolve each track's patch by name against the registry
                // shipped alongside it (Phase 1) — once here, not per-note in
                // the real-time scheduling loop below. Fixed-size array, no
                // allocation.
                m_trackPatches.fill(nullptr);
                const auto& tracks = m_activeSequence->tracks;
                for (size_t t = 0; t < tracks.size() && t < kMaxEngineTracks; ++t) {
                    auto it = m_activeSequence->patches.find(tracks[t].patchName);
                    if (it != m_activeSequence->patches.end()) {
                        m_trackPatches[t] = &it->second;
                    }
                }

                // Phase 2: reseed automation live-params from this sequence's
                // static Track/Patch values so a lane with no matching target
                // (or a track with no lanes at all) sounds exactly like Phase 1.
                resetLiveParams();

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

    // Absolute sample position at the start of this block, captured before
    // m_currentSamplePosition is advanced below. Audio clips are placed in
    // absolute seconds (not beats), so this is what lets the per-sample loop
    // test clip overlap with plain index arithmetic.
    double blockStartSample = m_currentSamplePosition;

    // --- Sequencer Event Pre-calculation ---
    // Instead of evaluating floats per-sample, we find all NoteOn/NoteOff events
    // that occur within this entire audio block [m_currentSamplePosition, m_currentSamplePosition + nFrames)
    struct ScheduledEvent {
        unsigned int sampleOffset;
        bool isNoteOn;
        uint8_t pitch;
        uint8_t velocity;
        int trackIndex; // which per-track bus the resulting voice sums into
    };
    std::vector<ScheduledEvent> scheduledEvents;

    // NOTE: notes from every track are merged into one scheduledEvents list and
    // play through one shared m_voices pool (each voice still carries its own
    // per-track-resolved patch pointer, stamped in via m_trackPatches below —
    // see Phase 1's plan). handleNoteOff() below matches purely by MIDI pitch,
    // with no track tag, so two tracks sounding the same pitch at once can
    // cross-release each other's voice. Accepted consequence of the
    // shared-voice-pool design, not a bug.
    if (m_isPlaying && m_activeSequence) {
        const auto& seqTracks = m_activeSequence->tracks;

        // live-PLAN L1: split this callback into sub-ranges at the loop-end
        // boundary, so a wrap can never skip or double-fire a note/automation
        // event exactly at the seam. Each iteration schedules against its own
        // [subBlockStartBeat, subBlockEndBeat) the same way the old single-
        // pass code did against [blockStartBeat, blockEndBeat) for the whole
        // callback; sampleOffset is then shifted by framesScheduled to land
        // at the right spot in the output buffer. Only scheduling (which new
        // NoteOns/NoteOffs fire) is affected by the position reset below —
        // already-active voices, delay lines, and the reverb tail are
        // untouched and ring out naturally across the seam.
        unsigned int framesScheduled = 0;
        while (framesScheduled < nFrames) {
            unsigned int subFrames = nFrames - framesScheduled;
            bool willWrap = false;

            if (m_loopEnabled && m_loopEndBeat > m_loopStartBeat) {
                double loopEndSample = static_cast<double>(m_loopEndBeat) * samplesPerBeat;
                double samplesToLoopEnd = loopEndSample - m_currentSamplePosition;
                if (samplesToLoopEnd < static_cast<double>(subFrames)) {
                    // max(1, ...) guarantees framesScheduled always progresses,
                    // even for a pathologically short loop region or a position
                    // already past loopEndBeat (e.g. loop just got enabled).
                    subFrames = static_cast<unsigned int>(std::max(1.0, std::floor(samplesToLoopEnd)));
                    willWrap = true;
                }
            }

            double subBlockStartBeat = m_currentSamplePosition / samplesPerBeat;
            double subBlockEndBeat = (m_currentSamplePosition + subFrames) / samplesPerBeat;

            // Phase 2: evaluate every automation lane once per sub-range,
            // before any voice/mix/effect code below reads the live-param
            // targets they write.
            evaluateAutomation(subBlockStartBeat);

            for (size_t trackIdx = 0; trackIdx < seqTracks.size(); ++trackIdx) {
                const auto& track = seqTracks[trackIdx];
                int busIdx = static_cast<int>(std::min(trackIdx, kMaxEngineTracks - 1));

                // --- C418 suite: track-level arpeggiator ---
                // Arp-enabled tracks don't schedule their notes directly. Instead,
                // each step on the rateBeats grid gathers the chord sounding at
                // the step's onset and emits one staccato note from the pattern.
                // Everything is derived from the beat clock (stateless per block),
                // so NoteOffs recompute the SAME chord their NoteOn used and hot
                // reloads/seeks can't desync the pattern.
                if (track.arp.mode != 0 && !track.notes.empty()) {
                    double rate = std::max(0.0625f, track.arp.rateBeats);
                    double gateBeats = rate * std::clamp(track.arp.gate, 0.05f, 0.98f);
                    int octaves = std::clamp(track.arp.octaves, 1, 4);

                    // Cover steps whose NoteOn OR NoteOff can land inside this sub-range.
                    long firstStep = static_cast<long>(std::floor((subBlockStartBeat - gateBeats) / rate));
                    long lastStep = static_cast<long>(std::floor(subBlockEndBeat / rate)) + 1;
                    for (long step = std::max(0L, firstStep); step <= lastStep; ++step) {
                        double onBeat = step * rate;
                        double offBeat = onBeat + gateBeats;
                        bool onInBlock = onBeat >= subBlockStartBeat && onBeat < subBlockEndBeat;
                        bool offInBlock = offBeat >= subBlockStartBeat && offBeat < subBlockEndBeat;
                        if (!onInBlock && !offInBlock) continue;

                        // Chord sounding at the step onset (fixed-size, no allocation)
                        uint8_t chord[16];
                        int chordCount = 0;
                        uint8_t velocity = 100;
                        for (const auto& note : track.notes) {
                            if (note.startBeat <= onBeat && onBeat < note.startBeat + note.lengthBeats && chordCount < 16) {
                                chord[chordCount++] = note.pitch;
                                velocity = note.velocity;
                            }
                        }
                        if (chordCount == 0) continue;

                        // Insertion sort ascending (tiny N)
                        for (int a = 1; a < chordCount; ++a) {
                            uint8_t key = chord[a];
                            int b = a - 1;
                            while (b >= 0 && chord[b] > key) { chord[b + 1] = chord[b]; --b; }
                            chord[b + 1] = key;
                        }

                        int total = chordCount * octaves;
                        int idx;
                        if (track.arp.mode == 2) { // down
                            idx = total - 1 - static_cast<int>(step % total);
                        } else if (track.arp.mode == 3) { // up-down
                            int period = std::max(1, 2 * total - 2);
                            int k = static_cast<int>(step % period);
                            idx = k < total ? k : 2 * total - 2 - k;
                        } else { // up
                            idx = static_cast<int>(step % total);
                        }
                        int pitch = chord[idx % chordCount] + 12 * (idx / chordCount);
                        uint8_t arpPitch = static_cast<uint8_t>(std::clamp(pitch, 0, 127));

                        if (onInBlock) {
                            unsigned int off = framesScheduled + static_cast<unsigned int>((onBeat - subBlockStartBeat) * samplesPerBeat);
                            if (off < nFrames) scheduledEvents.push_back({off, true, arpPitch, velocity, busIdx});
                        }
                        if (offInBlock) {
                            unsigned int off = framesScheduled + static_cast<unsigned int>((offBeat - subBlockStartBeat) * samplesPerBeat);
                            if (off < nFrames) scheduledEvents.push_back({off, false, arpPitch, velocity, busIdx});
                        }
                    }
                    continue; // arp replaces direct note scheduling for this track
                }

                for (const auto& note : track.notes) {
                    double noteStartBeat = note.startBeat;
                    double noteEndBeat = note.startBeat + note.lengthBeats;

                    // Check Note On
                    if (noteStartBeat >= subBlockStartBeat && noteStartBeat < subBlockEndBeat) {
                        double beatOffset = noteStartBeat - subBlockStartBeat;
                        unsigned int sampleOffset = framesScheduled + static_cast<unsigned int>(beatOffset * samplesPerBeat);
                        if (sampleOffset < nFrames) {
                            scheduledEvents.push_back({sampleOffset, true, note.pitch, note.velocity, busIdx});
                        }
                    }

                    // Check Note Off
                    if (noteEndBeat >= subBlockStartBeat && noteEndBeat < subBlockEndBeat) {
                        double beatOffset = noteEndBeat - subBlockStartBeat;
                        unsigned int sampleOffset = framesScheduled + static_cast<unsigned int>(beatOffset * samplesPerBeat);
                        if (sampleOffset < nFrames) {
                            scheduledEvents.push_back({sampleOffset, false, note.pitch, note.velocity, busIdx});
                        }
                    }
                }
            }

            m_currentSamplePosition += subFrames;
            framesScheduled += subFrames;
            if (willWrap) {
                // Rewinds scheduling only — active voices/delay/reverb are
                // untouched and continue ringing out past the seam.
                m_currentSamplePosition = static_cast<double>(m_loopStartBeat) * samplesPerBeat;
            }
        }
    }

    // --- Audio Clip Pre-calculation ---
    // Mirrors the note-scheduling precalculation above: clipStartSample/
    // clipFrameCount are invariant per clip within this block (in fact for the
    // clip's whole lifetime), so compute them once per block instead of on
    // every one of the nFrames samples. Also skips clips that can't possibly
    // overlap this block at all, rather than testing that per-sample too.
    struct ActiveClipRef {
        const std::vector<float>* pcmData;
        double clipStartSample;
        size_t clipFrameCount;
        unsigned int channels;
        int trackIndex;
    };
    std::vector<ActiveClipRef> activeClips;
    if (m_isPlaying && m_activeSequence) {
        double blockEndSample = blockStartSample + nFrames;
        const auto& seqTracks = m_activeSequence->tracks;
        for (size_t trackIdx = 0; trackIdx < seqTracks.size(); ++trackIdx) {
            const auto& track = seqTracks[trackIdx];
            int busIdx = static_cast<int>(std::min(trackIdx, kMaxEngineTracks - 1));
            for (const auto& clip : track.audioClips) {
                double clipStartSample = static_cast<double>(clip.startTimeSeconds) * m_sampleRate;
                size_t clipFrameCount = clip.pcmData->size() / clip.channels;
                if (clipStartSample >= blockEndSample) continue; // starts after this block
                if (clipStartSample + static_cast<double>(clipFrameCount) <= blockStartSample) continue; // ended before this block
                activeClips.push_back({clip.pcmData.get(), clipStartSample, clipFrameCount, clip.channels, busIdx});
            }
        }
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
                    // Resolved once per SequenceUpdate (Phase 1), not here —
                    // ev.trackIndex is always a valid bus index (clamped when
                    // scheduledEvents was built above).
                    onEvt.data.patch = m_trackPatches[ev.trackIndex];
                    handleNoteOn(onEvt, ev.trackIndex);
                } else {
                    AudioEvent offEvt{};
                    offEvt.type = AudioEventType::NoteOff;
                    offEvt.pitch = ev.pitch;
                    offEvt.velocity = ev.velocity;
                    handleNoteOff(offEvt);
                }
            }
        }

        // Per-track buses (Phase 5): voices and clips sum into their own
        // track's bus, each bus runs its insert-effect chain, and the results
        // are mixed into the master below. Plain stack arrays — no allocation.
        float busL[kMaxEngineTracks] = {};
        float busR[kMaxEngineTracks] = {};

        float sampleLeft = 0.0f;
        float sampleRight = 0.0f;

        // Summation: Iterate over all active voices
        for (auto& voice : m_voices) {
            if (!voice.active) continue;

            // Resolved once per voice per sample — used by the C418 filter,
            // Phase 3 resonant filter, and Phase 4 formant bank below, all of
            // which read this voice's track's live-params.
            int liveBusIdx = std::clamp(voice.trackIndex, 0, static_cast<int>(kMaxEngineTracks) - 1);

            float oscVal = 0.0f;
            float fundamentalFreq = midiToFreq(voice.pitch);

            // --- STAKILLAZ suite: pitch-drop transient (hardstyle kick / 808
            // "tok"). Starts pitchDropSemitones above the note and decays
            // exponentially to the root over ~pitchDropMs. Multiplies the
            // fundamental of BOTH the harmonic stack and the sub oscillator.
            if (voice.patch && voice.patch->pitchDropSemitones != 0.0f) {
                float tauSamples = std::max(1.0f, (voice.patch->pitchDropMs / 1000.0f) * static_cast<float>(m_sampleRate));
                float dropNow = voice.patch->pitchDropSemitones * std::exp(-static_cast<float>(voice.ageSamples) / tauSamples);
                fundamentalFreq *= std::pow(2.0f, dropNow / 12.0f);
            }

            // --- Phase 4: glide/portamento. Slides from the frequency this
            // voice was set up to glide FROM (captured at NoteOn) to the
            // note's own (possibly pitch-dropped) frequency over glideMs,
            // interpolating linearly in log2-frequency (constant-rate pitch
            // slide, not a Hz-linear sweep) so it sounds like a musical glide
            // rather than a warped one.
            if (voice.glideSamplesTotal > 0) {
                float t = std::min(1.0f, static_cast<float>(voice.glideSamplesElapsed) / static_cast<float>(voice.glideSamplesTotal));
                float logStart = std::log2(voice.glideStartFreq);
                float logTarget = std::log2(fundamentalFreq);
                fundamentalFreq = std::exp2(logStart + (logTarget - logStart) * t);
                voice.glideSamplesElapsed++;
            }

            // --- Phase 4: pitch-LFO vibrato, applied to the fundamental so
            // the harmonic stack, sub-osc, and formant bank all wobble
            // together. Phase keeps advancing during the onset delay (not
            // reset once it ends) so there's no phase-sync click when the
            // gate opens.
            if (voice.patch && voice.patch->vibratoDepthCents > 0.0f && voice.patch->vibratoRateHz > 0.0f) {
                uint64_t delaySamples = static_cast<uint64_t>((voice.patch->vibratoDelayMs / 1000.0f) * static_cast<float>(m_sampleRate));
                if (voice.ageSamples >= delaySamples) {
                    float cents = std::sin(voice.vibratoPhase * 2.0f * static_cast<float>(M_PI)) * voice.patch->vibratoDepthCents;
                    fundamentalFreq *= std::pow(2.0f, cents / 1200.0f);
                }
                voice.vibratoPhase += voice.patch->vibratoRateHz / static_cast<float>(m_sampleRate);
                if (voice.vibratoPhase >= 1.0f) voice.vibratoPhase -= 1.0f;
            }

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

            // --- STAKILLAZ suite: 808-style sub oscillator. Bypasses the
            // harmonic array entirely — a pure sine/triangle at the (pitch-
            // dropped) fundamental, summed on top of the harmonic stack.
            if (voice.patch && voice.patch->subOscLevel > 0.0f) {
                float subVal;
                if (voice.patch->subOscWave == 1) {
                    // Triangle from phase: 4|p-0.5| - 1 gives -1..1
                    subVal = 4.0f * std::abs(voice.subPhase - 0.5f) - 1.0f;
                } else {
                    subVal = std::sin(voice.subPhase * 2.0f * static_cast<float>(M_PI));
                }
                oscVal += subVal * voice.patch->subOscLevel;
                voice.subPhase += basePhaseInc;
                if (voice.subPhase >= 1.0f) voice.subPhase -= 1.0f;
            }

            // --- Phase 3: noise oscillator. Summed alongside the harmonic
            // stack and sub-oscillator — the source for synthesized
            // percussion (Kick beater-click, Snare/Hat body) and, shaped by
            // the resonant filter below, subtractive texture on melodic
            // patches too.
            if (voice.patch && voice.patch->noiseLevel > 0.0f) {
                // xorshift32 (no heap allocation, no std::rand global state).
                voice.noiseRng ^= voice.noiseRng << 13;
                voice.noiseRng ^= voice.noiseRng >> 17;
                voice.noiseRng ^= voice.noiseRng << 5;
                float white = (static_cast<float>(voice.noiseRng) / 4294967295.0f) * 2.0f - 1.0f;

                float noiseSample;
                if (voice.patch->noiseType == 1) {
                    // Paul Kellett's "economy" pink-noise filter (3-pole IIR
                    // approximation of a -3dB/octave spectral tilt).
                    voice.pinkState[0] = 0.99765f * voice.pinkState[0] + white * 0.0990460f;
                    voice.pinkState[1] = 0.96300f * voice.pinkState[1] + white * 0.2965164f;
                    voice.pinkState[2] = 0.57000f * voice.pinkState[2] + white * 1.0526913f;
                    noiseSample = (voice.pinkState[0] + voice.pinkState[1] + voice.pinkState[2] + white * 0.1848f) * 0.11f;
                } else {
                    noiseSample = white;
                }
                oscVal += noiseSample * voice.patch->noiseLevel;
            }

            // --- Phase 5: virtual-analog unison oscillator. An alternative/
            // complement to the additive harmonic stack above: up to
            // kMaxUnisonVoices detuned copies of a band-limited (polyBLEP)
            // saw/square/triangle, summed in MONO into oscVal so the SAME
            // envelope/filter/formant chain that shapes every other
            // oscillator source shapes this one too. Actually spreading each
            // unison voice across the stereo field would mean running that
            // whole chain twice (duplicate per-channel filter state) for a
            // supersaw's width — too much added state/CPU for this pass, so
            // instead the pan spread is captured as a separate, lighter
            // "stereo width" signal (unisonStereoWidth) that only goes
            // through the envelope, then gets added directly at the
            // track-bus pan stage below. A standard shortcut: unison/chorus
            // width commonly sits "on top of" a mono voice rather than
            // requiring true per-channel filtering.
            float unisonStereoWidth = 0.0f;
            if (voice.patch && voice.patch->oscWave > 0) {
                int unisonCount = std::clamp(voice.patch->oscUnisonVoices, 1, static_cast<int>(kMaxUnisonVoices));
                float unisonGain = 1.0f / std::sqrt(static_cast<float>(unisonCount));
                for (int u = 0; u < unisonCount; ++u) {
                    float centsSpread = (unisonCount > 1)
                        ? std::lerp(-voice.patch->oscDetuneCents * 0.5f, voice.patch->oscDetuneCents * 0.5f,
                                    static_cast<float>(u) / static_cast<float>(unisonCount - 1))
                        : 0.0f;
                    float freqU = fundamentalFreq * std::pow(2.0f, centsSpread / 1200.0f);
                    float phaseIncU = freqU / static_cast<float>(m_sampleRate);

                    float sampleU = GeneratePolyBlepSample(voice.oscUnisonPhase[u], phaseIncU,
                                                            voice.patch->oscWave, voice.patch->oscPulseWidth,
                                                            voice.oscTriIntegrator[u]);
                    voice.oscUnisonPhase[u] += phaseIncU;
                    if (voice.oscUnisonPhase[u] >= 1.0f) voice.oscUnisonPhase[u] -= 1.0f;

                    float weighted = sampleU * unisonGain;
                    oscVal += weighted;
                    float pan = (unisonCount > 1)
                        ? std::lerp(-1.0f, 1.0f, static_cast<float>(u) / static_cast<float>(unisonCount - 1))
                        : 0.0f;
                    unisonStereoWidth += weighted * pan;
                }
            }

            // --- Phase 4: formant bank. Three parallel bandpass TPT SVFs
            // (fixed Q, see kFormantFilterK) sitting directly on the raw
            // oscillator sum — "on top of" the harmonic/sub/noise source, in
            // parallel with (not replacing) the downstream envelope/one-pole/
            // resonant-filter/drive chain below. Center frequencies morph
            // between the patch's two vowel presets via this track's live
            // patch.formantMorph (Phase 2 automation target), then the wet
            // (formant-filtered) signal is crossfaded against the dry oscVal
            // by formantAmount (0 = bypass, matching every other Phase 3/4
            // stage's "0 = off" convention).
            if (voice.patch && voice.patch->formantAmount > 0.0f) {
                float morph = std::clamp(m_liveParams[liveBusIdx].formantMorph.load(std::memory_order_relaxed), 0.0f, 1.0f);
                float wet = 0.0f;
                for (int b = 0; b < 3; ++b) {
                    float centerFreq = std::lerp(voice.formantFreqA[b], voice.formantFreqB[b], morph);
                    centerFreq = std::clamp(centerFreq, 20.0f, static_cast<float>(m_sampleRate) * 0.49f);

                    float g = std::tan(static_cast<float>(M_PI) * centerFreq / static_cast<float>(m_sampleRate));
                    float a1 = 1.0f / (1.0f + g * (g + kFormantFilterK));
                    float a2 = g * a1;
                    float a3 = g * a2;

                    float v3 = oscVal - voice.formantIc2eq[b];
                    float v1 = a1 * voice.formantIc1eq[b] + a2 * v3; // band-pass output
                    float v2 = voice.formantIc2eq[b] + a2 * voice.formantIc1eq[b] + a3 * v3;
                    voice.formantIc1eq[b] = 2.0f * v1 - voice.formantIc1eq[b];
                    voice.formantIc2eq[b] = 2.0f * v2 - voice.formantIc2eq[b];

                    wet += v1 * kFormantBandGain[b];
                }
                oscVal = std::lerp(oscVal, wet, voice.patch->formantAmount);
            }

            voice.ageSamples++;

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

            // Phase 5: unison stereo-width signal follows the same
            // envelope/velocity shape as currentSample (so it fades in/out
            // naturally with the note) but deliberately skips the filter/
            // drive stages below — see the design-tradeoff comment where
            // unisonStereoWidth is computed above.
            float unisonWidthSample = unisonStereoWidth * voice.envLevel * velNorm * kUnisonWidthAmount;

            // --- C418 suite: one-pole low-pass filter with LFO-modulated
            // cutoff (warm, slowly-evolving pads). Bypassed at high cutoffs.
            // Cutoff is sourced from this voice's track live-param (Phase 2:
            // automatable via patch.filterCutoffHz lanes — the static Patch
            // value is just the seed written by resetLiveParams()).
            float liveCutoffHz = m_liveParams[liveBusIdx].filterCutoffHz.load(std::memory_order_relaxed);
            if (voice.patch && liveCutoffHz < kFilterBypassHz) {
                float cutoff = liveCutoffHz;
                if (voice.patch->filterLfoDepth > 0.0f && voice.patch->filterLfoRateHz > 0.0f) {
                    cutoff *= 1.0f + voice.patch->filterLfoDepth *
                              std::sin(voice.lfoPhase * 2.0f * static_cast<float>(M_PI));
                    voice.lfoPhase += voice.patch->filterLfoRateHz / static_cast<float>(m_sampleRate);
                    if (voice.lfoPhase >= 1.0f) voice.lfoPhase -= 1.0f;
                }
                cutoff = std::clamp(cutoff, 20.0f, 20000.0f);
                float coeff = 1.0f - std::exp(-2.0f * static_cast<float>(M_PI) * cutoff / static_cast<float>(m_sampleRate));
                voice.lpfState += coeff * (currentSample - voice.lpfState);
                currentSample = voice.lpfState;
            }

            // --- Phase 3: resonant multimode filter (TPT/Zavalishin state-
            // variable design) with a dedicated envelope and optional
            // keyboard tracking. Independent of, and applied after, the
            // one-pole LPF above — same bypass convention (cutoff at/above
            // kFilterBypassHz means "no filtering"). Base cutoff/resonance
            // are sourced from this voice's track live-param (Phase 2:
            // automatable via patch.resFilterCutoff/resFilterResonance lanes).
            float liveResCutoff = m_liveParams[liveBusIdx].resFilterCutoff.load(std::memory_order_relaxed);
            if (voice.patch && liveResCutoff < kFilterBypassHz) {
                // Filter envelope: same Attack/Decay/Sustain/Release shape as
                // the amplitude envelope, but its own clock/tables so it can
                // move independently (e.g. a fast filter snap under a slow
                // amp swell).
                if (voice.filterEnvState == EnvState::Attack) {
                    if (voice.filterEnvSampleCount < voice.patch->filterEnvAttackTable.size()) {
                        voice.filterEnvLevel = voice.patch->filterEnvAttackTable[voice.filterEnvSampleCount++];
                    } else {
                        voice.filterEnvState = EnvState::Decay;
                        voice.filterEnvSampleCount = 0;
                        voice.filterEnvLevel = voice.patch->filterEnvDecayTable.empty()
                            ? voice.patch->filterEnvSustainLevel : voice.patch->filterEnvDecayTable[0];
                    }
                } else if (voice.filterEnvState == EnvState::Decay) {
                    if (voice.filterEnvSampleCount < voice.patch->filterEnvDecayTable.size()) {
                        voice.filterEnvLevel = voice.patch->filterEnvDecayTable[voice.filterEnvSampleCount++];
                    } else {
                        voice.filterEnvState = EnvState::Sustain;
                        voice.filterEnvLevel = voice.patch->filterEnvSustainLevel;
                    }
                } else if (voice.filterEnvState == EnvState::Sustain) {
                    voice.filterEnvLevel = voice.patch->filterEnvSustainLevel;
                } else if (voice.filterEnvState == EnvState::Release) {
                    if (voice.filterEnvSampleCount < voice.patch->filterEnvReleaseTable.size()) {
                        voice.filterEnvLevel = voice.patch->filterEnvReleaseTable[voice.filterEnvSampleCount++];
                    } else {
                        voice.filterEnvState = EnvState::Idle;
                        voice.filterEnvLevel = 0.0f;
                    }
                }

                float envOctaves = voice.patch->filterEnvAmount * voice.filterEnvLevel * kFilterEnvOctaveRange;
                float keyOctaves = voice.patch->keyTrack * (static_cast<float>(voice.pitch) - 60.0f) / 12.0f;
                float resCutoff = std::clamp(liveResCutoff * std::pow(2.0f, envOctaves + keyOctaves), 20.0f, 20000.0f);
                float resonance = std::clamp(m_liveParams[liveBusIdx].resFilterResonance.load(std::memory_order_relaxed), 0.0f, 1.0f);

                // TPT SVF coefficients (Zavalishin). k = 2 - 2*resonance: 2 at
                // resonance 0 (Q=0.5, no peak), approaching 0 (self-
                // oscillating) as resonance approaches 1.
                float g = std::tan(static_cast<float>(M_PI) * resCutoff / static_cast<float>(m_sampleRate));
                float k = 2.0f - 2.0f * resonance;
                float a1 = 1.0f / (1.0f + g * (g + k));
                float a2 = g * a1;
                float a3 = g * a2;

                float v3 = currentSample - voice.svfIc2eq;
                float v1 = a1 * voice.svfIc1eq + a2 * v3;
                float v2 = voice.svfIc2eq + a2 * voice.svfIc1eq + a3 * v3;
                voice.svfIc1eq = 2.0f * v1 - voice.svfIc1eq;
                voice.svfIc2eq = 2.0f * v2 - voice.svfIc2eq;

                switch (voice.patch->resFilterType) {
                    case 1: currentSample = v1; break;                          // Band-pass
                    case 2: currentSample = currentSample - k * v1 - v2; break; // High-pass
                    default: currentSample = v2; break;                        // Low-pass
                }
            }

            // --- STAKILLAZ suite: per-voice waveshaper drive. tanh-normalized
            // so drive changes the shape (harder clipping, denser overtones)
            // without exploding the level.
            if (voice.patch && voice.patch->drive > 0.0f) {
                float d = 1.0f + voice.patch->drive;
                currentSample = std::tanh(currentSample * d) / std::tanh(d);
            }

            // Pan center, into this voice's track bus. The unison width
            // signal spreads symmetrically around that center pan (Phase 5).
            int busIdx = std::clamp(voice.trackIndex, 0, static_cast<int>(kMaxEngineTracks) - 1);
            busL[busIdx] += currentSample * 0.5f - unisonWidthSample * 0.5f;
            busR[busIdx] += currentSample * 0.5f + unisonWidthSample * 0.5f;
        }

        // Summation: Mix in any active audio clips (pre-decoded PCM, purely
        // read-only index arithmetic here — no allocation, no locks, no I/O).
        // activeClips/clipStartSample/clipFrameCount were precomputed once per
        // block above, not recomputed on every sample.
        {
            double absoluteSample = blockStartSample + i;
            for (const auto& clip : activeClips) {
                double clipFrameOffsetD = absoluteSample - clip.clipStartSample;
                if (clipFrameOffsetD < 0.0) continue;

                size_t clipFrameOffset = static_cast<size_t>(clipFrameOffsetD);
                if (clipFrameOffset >= clip.clipFrameCount) continue;

                busL[clip.trackIndex] += (*clip.pcmData)[clipFrameOffset * clip.channels + 0];
                busR[clip.trackIndex] += (*clip.pcmData)[clipFrameOffset * clip.channels + 1];
            }
        }

        // Run each track bus through its insert-effect chain, then apply
        // per-track volume/pan (Phase 1) before mixing into the master.
        // Effects run every sample regardless of bus activity so reverb
        // tails ring out after their source stops. Traversal is read-only
        // over shared_ptrs the audio thread's track list owns.
        // Post-effect, post-volume/pan level of Track 1's bus — the
        // STAKILLAZ sidechain key.
        float sidechainKey = 0.0f;

        // Phase 5: per-track aux sends (SEND=BusName,Amount) accumulate HERE,
        // separately from sampleLeft/sampleRight's normal dry sum below, and
        // get fed as EXTRA input to the shared master Delay/Reverb further
        // down — on top of each track's ordinary dry contribution, not
        // instead of it.
        float auxDelayL = 0.0f, auxDelayR = 0.0f;
        float auxReverbL = 0.0f, auxReverbR = 0.0f;

        if (m_activeSequence && !m_activeSequence->tracks.empty()) {
            const auto& seqTracks = m_activeSequence->tracks;
            size_t busCount = std::min(seqTracks.size(), kMaxEngineTracks);
            for (size_t t = 0; t < busCount; ++t) {
                float l = busL[t];
                float r = busR[t];
                const Track& trk = seqTracks[t];
                for (const auto& fx : trk.effects) {
                    if (fx) fx->processSample(l, r);
                }

                // Simple linear pan (not equal-power): pan=0/volume=1 leaves
                // l,r unchanged, so pre-Phase-1 projects (no MIX key) sound
                // identical to before. Sourced from the live-param block
                // (Phase 2: automatable via mix.volume/mix.pan lanes), seeded
                // from trk.volume/trk.pan by resetLiveParams().
                float liveVolume = m_liveParams[t].volume.load(std::memory_order_relaxed);
                float pan = std::clamp(m_liveParams[t].pan.load(std::memory_order_relaxed), -1.0f, 1.0f);
                l *= liveVolume * std::min(1.0f, 1.0f - pan);
                r *= liveVolume * std::min(1.0f, 1.0f + pan);

                // live-PLAN L4: decaying peak-hold at this same post-fader
                // point, for the sequencer's per-track meters. One-pole
                // release (~a few hundred ms at kEngineSampleRate) so a UI
                // frame reading this once sees a smoothly falling value
                // rather than near-silence between transients.
                constexpr float kTrackPeakReleaseCoeff = 0.9999f;
                float absLevel = std::max(std::abs(l), std::abs(r));
                float prevPeak = m_trackPeaks[t].load(std::memory_order_relaxed);
                m_trackPeaks[t].store(std::max(absLevel, prevPeak * kTrackPeakReleaseCoeff), std::memory_order_relaxed);

                // Phase 5: post-fader sends (same tap point a DAW send would
                // use — after the track's own insert FX and volume/pan).
                if (trk.sendDelayAmount > 0.0f) {
                    auxDelayL += l * trk.sendDelayAmount;
                    auxDelayR += r * trk.sendDelayAmount;
                }
                if (trk.sendReverbAmount > 0.0f) {
                    auxReverbL += l * trk.sendReverbAmount;
                    auxReverbR += r * trk.sendReverbAmount;
                }

                if (t == 0) sidechainKey = std::max(std::abs(l), std::abs(r));
                sampleLeft += l;
                sampleRight += r;
            }
        } else {
            // No track list yet (live/queue voices only) — bus 0 passes through
            sampleLeft += busL[0];
            sampleRight += busR[0];
            sidechainKey = std::max(std::abs(busL[0]), std::abs(busR[0]));
        }

        // --- Master FX chain (before the peak compressor) ---

        // 1. C418 stereo ping-pong delay: cross-feedback between channels.
        // Phase 5: per-track SEND=Delay content is mixed into the buffer's
        // INPUT only (auxDelayL/R) — the existing wet return below stays a
        // pure addition (sampleLeft/R are never attenuated by this block),
        // so there's no risk of double-counting a track's dry signal.
        if (m_delayMix > 0.0f) {
            size_t bufSize = m_delay.bufferL.size();
            size_t delaySamples = static_cast<size_t>((m_delayTimeMs / 1000.0f) * m_sampleRate);
            delaySamples = std::clamp<size_t>(delaySamples, 1, bufSize - 1);
            size_t readIndex = (m_delay.writeIndex + bufSize - delaySamples) % bufSize;

            float delayedL = m_delay.bufferL[readIndex];
            float delayedR = m_delay.bufferR[readIndex];

            float delayInputL = sampleLeft + auxDelayL;
            float delayInputR = sampleRight + auxDelayR;

            // Ping-pong: each channel's feedback goes to the OTHER channel
            m_delay.bufferL[m_delay.writeIndex] = delayInputL + delayedR * m_delayFeedback;
            m_delay.bufferR[m_delay.writeIndex] = delayInputR + delayedL * m_delayFeedback;
            if (++m_delay.writeIndex >= bufSize) m_delay.writeIndex = 0;

            sampleLeft += delayedL * m_delayMix;
            sampleRight += delayedR * m_delayMix;
        }

        // 2. C418 master reverb (Freeverb core; mix 0 = bypass inside).
        // Phase 5: SEND=Reverb content (auxReverbL/R) is fed into the WET
        // generation only, via processWetOnly — sampleLeft/R (the dry
        // passthrough) stay untouched by the sends, only the wet tail
        // picks up the extra content. Using the normal processSample here
        // instead would crossfade the aux-send content into the dry output
        // too (its dry/wet mix has no way to know part of its input was
        // send-only), effectively double-dosing sent tracks.
        float reverbMixNow = m_masterReverb.mix.load(std::memory_order_relaxed);
        if (reverbMixNow > 0.0f) {
            float reverbWetL = 0.0f, reverbWetR = 0.0f;
            m_masterReverb.processWetOnly(sampleLeft + auxReverbL, sampleRight + auxReverbR, reverbWetL, reverbWetR);
            sampleLeft = sampleLeft * (1.0f - reverbMixNow) + reverbWetL * reverbMixNow;
            sampleRight = sampleRight * (1.0f - reverbMixNow) + reverbWetR * reverbMixNow;
        }

        // 3. STAKILLAZ sidechain pump: Track 1's bus ducks the master.
        if (m_compressor.sidechainEnabled) {
            // Instant attack, one-pole release
            m_compressor.sidechainEnv = std::max(sidechainKey,
                m_compressor.sidechainEnv * m_compressor.sidechainReleaseCoeff);
            float duck = 1.0f - m_compressor.sidechainAmount * std::min(1.0f, m_compressor.sidechainEnv);
            duck = std::max(0.0f, duck);
            sampleLeft *= duck;
            sampleRight *= duck;
        }

        // 4. STAKILLAZ master drive: final tanh saturation/hard-clip character
        if (m_masterDrive > 0.0f) {
            float d = 1.0f + m_masterDrive;
            float norm = std::tanh(d);
            sampleLeft = std::tanh(sampleLeft * d) / norm;
            sampleRight = std::tanh(sampleRight * d) / norm;
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

        // M0: shared tap — the exact post-mix master frame about to be
        // handed to RtAudio, for SCOPE/SPECTRUM/meters/visualizer to read.
        m_masterTap.Write(sampleLeft, sampleRight);

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

void AudioEngine::handleNoteOn(const AudioEvent& event, int trackIndex) {
    size_t voiceIdx = allocateVoice();
    Voice& voice = m_voices[voiceIdx];

    // Initialize voice state
    voice.active = true;
    voice.pitch = event.pitch;
    voice.velocity = event.velocity;
    voice.trackIndex = trackIndex;
    voice.noteOnTimestamp = m_globalSampleCounter;
    voice.patch = event.data.patch; // resolved per-track before dispatch (Phase 1); may be nullptr

    // Reset phases
    for (size_t i = 0; i < 16; ++i) {
        voice.phase[i] = 0.0f;
        voice.harmonicAmplitudes[i] = 0.0f;
    }

    // Phase 5: reset the unison oscillator's per-voice phases and the
    // triangle wave's leaky-integrator state.
    voice.oscUnisonPhase.fill(0.0f);
    voice.oscTriIntegrator.fill(0.0f);

    // Reset per-voice DSP state (LPF, LFO, sub oscillator, pitch-drop clock)
    voice.lpfState = 0.0f;
    voice.lfoPhase = 0.0f;
    voice.subPhase = 0.0f;
    voice.ageSamples = 0;

    // Phase 3: reset noise RNG (must stay non-zero), resonant-filter state,
    // and the dedicated filter envelope's clock.
    voice.noiseRng = static_cast<uint32_t>(m_globalSampleCounter ^ (voiceIdx * 0x9E3779B9u)) | 1u;
    voice.pinkState = {0.0f, 0.0f, 0.0f};
    voice.svfIc1eq = 0.0f;
    voice.svfIc2eq = 0.0f;
    voice.filterEnvSampleCount = 0;
    if (voice.patch && !voice.patch->filterEnvAttackTable.empty()) {
        voice.filterEnvState = EnvState::Attack;
        voice.filterEnvLevel = voice.patch->filterEnvAttackTable[0];
    } else if (voice.patch && !voice.patch->filterEnvDecayTable.empty()) {
        voice.filterEnvState = EnvState::Decay;
        voice.filterEnvLevel = voice.patch->filterEnvDecayTable[0];
    } else {
        voice.filterEnvState = EnvState::Sustain;
        voice.filterEnvLevel = voice.patch ? voice.patch->filterEnvSustainLevel : 1.0f;
    }

    // Phase 4: reset formant bank filter state and resolve this note's vowel
    // formant frequencies once (not re-resolved by string comparison every
    // sample — see the comment on Voice::formantFreqA/B).
    voice.formantIc1eq = {0.0f, 0.0f, 0.0f};
    voice.formantIc2eq = {0.0f, 0.0f, 0.0f};
    if (voice.patch) {
        voice.formantFreqA = GetVowelFormants(voice.patch->formantVowelA);
        voice.formantFreqB = GetVowelFormants(voice.patch->formantVowelB);
    }
    voice.vibratoPhase = 0.0f;

    // Phase 4: glide/portamento. Slides from this track's last-triggered
    // frequency (m_lastTrackFreq == 0.0f means no previous note on this bus
    // yet, so no glide) to this note's own frequency over glideMs; then
    // remembers this note's frequency for the NEXT note's glide.
    int glideBusIdx = std::clamp(trackIndex, 0, static_cast<int>(kMaxEngineTracks) - 1);
    float noteFreq = midiToFreq(voice.pitch);
    voice.glideSamplesElapsed = 0;
    voice.glideSamplesTotal = 0;
    voice.glideStartFreq = noteFreq;
    if (voice.patch && voice.patch->glideMs > 0.0f && m_lastTrackFreq[glideBusIdx] > 0.0f) {
        voice.glideStartFreq = m_lastTrackFreq[glideBusIdx];
        voice.glideSamplesTotal = static_cast<uint64_t>((voice.patch->glideMs / 1000.0f) * static_cast<float>(m_sampleRate));
    }
    m_lastTrackFreq[glideBusIdx] = noteFreq;

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

            // Phase 3: release the dedicated filter envelope alongside the
            // amplitude envelope.
            if (voice.filterEnvState != EnvState::Release) {
                voice.filterEnvState = EnvState::Release;
                voice.filterEnvSampleCount = 0;
                if (!voice.patch || voice.patch->filterEnvReleaseTable.empty()) {
                    voice.filterEnvState = EnvState::Idle;
                    voice.filterEnvLevel = 0.0f;
                } else {
                    voice.filterEnvLevel = voice.filterEnvLevel * voice.patch->filterEnvReleaseTable[0];
                }
            }
        }
    }
}

void AudioEngine::handleParameterChange(const AudioEvent& event) {
    float value = event.data.paramData.value;
    switch (static_cast<EngineParam>(event.data.paramData.paramId)) {
        case EngineParam::DelayTimeMs:
            m_delayTimeMs = std::clamp(value, 1.0f, 1990.0f);
            break;
        case EngineParam::DelayFeedback:
            m_delayFeedback = std::clamp(value, 0.0f, 0.95f);
            break;
        case EngineParam::DelayMix:
            m_delayMix = std::clamp(value, 0.0f, 1.0f);
            break;
        case EngineParam::ReverbRoom:
            m_masterReverb.roomSize.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case EngineParam::ReverbDamp:
            m_masterReverb.damping.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case EngineParam::ReverbMix:
            m_masterReverb.mix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            break;
        case EngineParam::SidechainEnabled:
            m_compressor.sidechainEnabled = value >= 0.5f;
            break;
        case EngineParam::SidechainAmount:
            m_compressor.sidechainAmount = std::clamp(value, 0.0f, 1.0f);
            break;
        case EngineParam::SidechainReleaseMs: {
            float ms = std::clamp(value, 10.0f, 1000.0f);
            m_compressor.sidechainReleaseCoeff = std::exp(-1.0f / ((ms / 1000.0f) * m_sampleRate));
            break;
        }
        case EngineParam::MasterDrive:
            m_masterDrive = std::clamp(value, 0.0f, 30.0f);
            break;
    }
}

void AudioEngine::resetLiveParams() {
    // std::array<LiveTrackParams>::fill() would need copy-assignment, which
    // atomic<float> doesn't have — reset each field individually instead.
    for (auto& live : m_liveParams) {
        live.volume.store(1.0f, std::memory_order_relaxed);
        live.pan.store(0.0f, std::memory_order_relaxed);
        live.filterCutoffHz.store(20000.0f, std::memory_order_relaxed);
        live.resFilterCutoff.store(20000.0f, std::memory_order_relaxed);
        live.resFilterResonance.store(0.0f, std::memory_order_relaxed);
        live.formantMorph.store(0.0f, std::memory_order_relaxed);
    }
    // Phase 4: a track patch/instrument swap shouldn't leave a stale glide
    // target from whatever used to be on that bus.
    m_lastTrackFreq.fill(0.0f);
    if (!m_activeSequence) return;
    const auto& tracks = m_activeSequence->tracks;
    for (size_t t = 0; t < tracks.size() && t < kMaxEngineTracks; ++t) {
        m_liveParams[t].volume.store(tracks[t].volume, std::memory_order_relaxed);
        m_liveParams[t].pan.store(tracks[t].pan, std::memory_order_relaxed);
        const Patch* p = m_trackPatches[t];
        m_liveParams[t].filterCutoffHz.store(p ? p->filterCutoffHz : 20000.0f, std::memory_order_relaxed);
        m_liveParams[t].resFilterCutoff.store(p ? p->resFilterCutoff : 20000.0f, std::memory_order_relaxed);
        m_liveParams[t].resFilterResonance.store(p ? p->resFilterResonance : 0.0f, std::memory_order_relaxed);
        m_liveParams[t].formantMorph.store(p ? p->formantMorph : 0.0f, std::memory_order_relaxed);
    }
}

void AudioEngine::evaluateAutomation(double beatD) {
    if (!m_activeSequence) return;
    float beat = static_cast<float>(beatD);
    const auto& tracks = m_activeSequence->tracks;

    for (const auto& lane : m_activeSequence->automation) {
        if (lane.points.empty()) continue;
        float value = EvaluateAutomationLane(lane, beat);

        if (lane.trackTarget == "MASTER") {
            applyMasterAutomationTarget(lane.paramTarget, value);
            continue;
        }

        // trackTarget names a Track by patchName — the only per-track
        // identity the .adx format has (see AutomationLane's comment).
        for (size_t t = 0; t < tracks.size() && t < kMaxEngineTracks; ++t) {
            if (tracks[t].patchName == lane.trackTarget) {
                applyTrackAutomationTarget(static_cast<int>(t), lane.paramTarget, value);
                break;
            }
        }
    }
}

void AudioEngine::applyMasterAutomationTarget(const std::string& paramTarget, float value) {
    // Same fields/clamps as handleParameterChange's EngineParam switch —
    // automation is just another writer of the same master-FX state.
    if (paramTarget == "master.sidechainAmount") {
        m_compressor.sidechainAmount = std::clamp(value, 0.0f, 1.0f);
    } else if (paramTarget == "master.sidechainEnabled") {
        m_compressor.sidechainEnabled = value >= 0.5f;
    } else if (paramTarget == "master.sidechainReleaseMs") {
        float ms = std::clamp(value, 10.0f, 1000.0f);
        m_compressor.sidechainReleaseCoeff = std::exp(-1.0f / ((ms / 1000.0f) * m_sampleRate));
    } else if (paramTarget == "master.reverbMix") {
        m_masterReverb.mix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    } else if (paramTarget == "master.reverbRoom") {
        m_masterReverb.roomSize.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    } else if (paramTarget == "master.reverbDamp") {
        m_masterReverb.damping.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    } else if (paramTarget == "master.delayMix") {
        m_delayMix = std::clamp(value, 0.0f, 1.0f);
    } else if (paramTarget == "master.delayFeedback") {
        m_delayFeedback = std::clamp(value, 0.0f, 0.95f);
    } else if (paramTarget == "master.delayTimeMs") {
        m_delayTimeMs = std::clamp(value, 1.0f, 1990.0f);
    } else if (paramTarget == "master.masterDrive") {
        m_masterDrive = std::clamp(value, 0.0f, 30.0f);
    }
    // Unrecognized target: silently ignored, same contract as an unrecognized
    // .adx key — lets lanes for not-yet-built phases sit inert instead of
    // failing to load.
}

void AudioEngine::applyTrackAutomationTarget(int busIdx, const std::string& paramTarget, float value) {
    if (busIdx < 0 || busIdx >= static_cast<int>(kMaxEngineTracks)) return;
    auto& live = m_liveParams[busIdx];

    if (paramTarget == "mix.volume") {
        live.volume.store(value, std::memory_order_relaxed);
    } else if (paramTarget == "mix.pan") {
        live.pan.store(std::clamp(value, -1.0f, 1.0f), std::memory_order_relaxed);
    } else if (paramTarget == "patch.filterCutoffHz") {
        live.filterCutoffHz.store(value, std::memory_order_relaxed);
    } else if (paramTarget == "patch.resFilterCutoff") {
        live.resFilterCutoff.store(value, std::memory_order_relaxed);
    } else if (paramTarget == "patch.resFilterResonance") {
        live.resFilterResonance.store(value, std::memory_order_relaxed);
    } else if (paramTarget == "patch.formantMorph") {
        live.formantMorph.store(value, std::memory_order_relaxed);
    } else if (paramTarget.rfind("effect.", 0) == 0 && m_activeSequence) {
        // "effect.<TypeName>.<field>" — find the matching effect instance in
        // this track's insert chain by typeName() and write straight into
        // its atomic parameter, same as the UI's TRACK FX sliders do.
        size_t firstDot = paramTarget.find('.');
        size_t secondDot = paramTarget.find('.', firstDot + 1);
        if (secondDot == std::string::npos) return;
        std::string typeName = paramTarget.substr(firstDot + 1, secondDot - firstDot - 1);
        std::string field = paramTarget.substr(secondDot + 1);

        const auto& tracks = m_activeSequence->tracks;
        if (busIdx >= static_cast<int>(tracks.size())) return;
        for (const auto& fx : tracks[busIdx].effects) {
            if (!fx || typeName != fx->typeName()) continue;
            if (auto* rev = dynamic_cast<ReverbEffect*>(fx.get())) {
                if (field == "mix") rev->mix.store(std::clamp(value, 0.0f, 1.0f));
                else if (field == "roomSize") rev->roomSize.store(std::clamp(value, 0.0f, 1.0f));
                else if (field == "damping") rev->damping.store(std::clamp(value, 0.0f, 1.0f));
            } else if (auto* dist = dynamic_cast<DistortionEffect*>(fx.get())) {
                if (field == "drive") dist->drive.store(std::clamp(value, 1.0f, 30.0f));
                else if (field == "mix") dist->mix.store(std::clamp(value, 0.0f, 1.0f));
            } else if (auto* bc = dynamic_cast<BitcrushEffect*>(fx.get())) {
                if (field == "mix") bc->mix.store(std::clamp(value, 0.0f, 1.0f));
                else if (field == "bitDepth") bc->bitDepth.store(std::clamp(value, 1.0f, 16.0f));
                else if (field == "rateHz") bc->rateHz.store(std::clamp(value, 100.0f, static_cast<float>(kEngineSampleRate)));
            } else if (auto* ch = dynamic_cast<ChorusEffect*>(fx.get())) {
                if (field == "mix") ch->mix.store(std::clamp(value, 0.0f, 1.0f));
                else if (field == "rateHz") ch->rateHz.store(std::max(0.01f, value));
                else if (field == "depth") ch->depth.store(std::clamp(value, 0.0f, 1.0f));
            } else if (auto* eq = dynamic_cast<EQEffect*>(fx.get())) {
                if (field == "lowGainDb") eq->lowGainDb.store(value);
                else if (field == "midGainDb") eq->midGainDb.store(value);
                else if (field == "highGainDb") eq->highGainDb.store(value);
            }
            break;
        }
    }
    // Unrecognized param path: silently ignored (same contract as above).
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

#include "AdxParser.h"
#include "AudioFileLoader.h"
#include "AudioClipProcessor.h"
#include "AudioEffect.h"
#include "PatternCompiler.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <string_view>

// .adx only stores each patch's ADSR as scalar milliseconds — curve SHAPE
// (the PATCH EDITOR's Bezier handles) has no on-disk representation (see
// Patch::operator=='s comment). Before Phase 1, that was invisible: every
// track shared one engine-side patch, and main.cpp built that one's tables
// from the live Bezier UI state. Now each track resolves its OWN named
// patch straight out of state.patches, so every parsed patch needs SOME
// playable table, not just the one wired into the editor. A straight-line
// ramp is a reasonable default shape for that — the editor still overwrites
// it with the real Bezier curve the moment its patch is opened/edited.
static std::vector<float> BuildLinearRamp(float fromLevel, float toLevel, float ms) {
    size_t samples = static_cast<size_t>((ms / 1000.0f) * kEngineSampleRate);
    if (samples == 0) return {};
    std::vector<float> table(samples);
    for (size_t i = 0; i < samples; ++i) {
        float t = (samples > 1) ? static_cast<float>(i) / static_cast<float>(samples - 1) : 1.0f;
        table[i] = fromLevel + (toLevel - fromLevel) * t;
    }
    return table;
}

static void BuildEnvelopeTables(Patch& p) {
    p.attackTable = BuildLinearRamp(0.0f, 1.0f, p.attackMs);
    p.decayTable = BuildLinearRamp(1.0f, p.sustainLevel, p.decayMs);
    p.releaseTable = BuildLinearRamp(p.sustainLevel, 0.0f, p.releaseMs);
}

// Phase 3: the dedicated filter envelope's tables, same shape/convention as
// BuildEnvelopeTables above.
static void BuildFilterEnvelopeTables(Patch& p) {
    p.filterEnvAttackTable = BuildLinearRamp(0.0f, 1.0f, p.filterEnvAttackMs);
    p.filterEnvDecayTable = BuildLinearRamp(1.0f, p.filterEnvSustainLevel, p.filterEnvDecayMs);
    p.filterEnvReleaseTable = BuildLinearRamp(p.filterEnvSustainLevel, 0.0f, p.filterEnvReleaseMs);
}

static std::string_view trim(std::string_view str) {
    auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return (start < end) ? str.substr(std::distance(str.begin(), start), std::distance(start, end)) : std::string_view();
}

// Strips a trailing "# ..." comment, but only when the '#' starts a token
// (preceded by whitespace, or line-initial) — NOT when it's glued to a note
// name like "F#5", where '#' means sharp. Phase 1 needs this because
// suffocation.adx documents units inline, e.g. "SUB=1.0, 0, 36, 55  # comment".
static std::string_view stripComment(std::string_view line) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))) {
            return trim(line.substr(0, i));
        }
    }
    return line;
}

static std::vector<std::string_view> split(std::string_view str, char delim) {
    std::vector<std::string_view> tokens;
    size_t start = 0;
    size_t end = str.find(delim);
    while (end != std::string_view::npos) {
        tokens.push_back(trim(str.substr(start, end - start)));
        start = end + 1;
        end = str.find(delim, start);
    }
    tokens.push_back(trim(str.substr(start)));
    return tokens;
}

uint8_t AdxParser::NoteNameToMidi(std::string_view noteName) {
    if (noteName.empty()) return 60; // Default Middle C

    std::string note(noteName);
    std::transform(note.begin(), note.end(), note.begin(), ::toupper);

    int octave = 0;
    size_t numPos = note.find_first_of("0123456789-");
    if (numPos != std::string::npos) {
        try {
            octave = std::stoi(note.substr(numPos));
        } catch (...) {
            return 60;
        }
        note = note.substr(0, numPos);
    }

    int baseNote = 0;
    if (note.length() > 0) {
        switch (note[0]) {
            case 'C': baseNote = 0; break;
            case 'D': baseNote = 2; break;
            case 'E': baseNote = 4; break;
            case 'F': baseNote = 5; break;
            case 'G': baseNote = 7; break;
            case 'A': baseNote = 9; break;
            case 'B': baseNote = 11; break;
            default: return 60;
        }
    }

    if (note.length() > 1) {
        if (note[1] == '#') baseNote += 1;
        else if (note[1] == 'B') baseNote -= 1; // Flat (Cb is B, but usually flats are b)
    }

    int midi = (octave + 1) * 12 + baseNote;
    return static_cast<uint8_t>(std::clamp(midi, 0, 127));
}

std::string AdxParser::MidiToNoteName(uint8_t midiNote) {
    static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int octave = (midiNote / 12) - 1;
    return std::string(noteNames[midiNote % 12]) + std::to_string(octave);
}

bool AdxParser::LoadProject(const std::string& filepath, SequencerState& state, std::string& outFirstPatchName) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "AdxParser Error: Could not open file " << filepath << std::endl;
        return false;
    }

    state.patches.clear();
    state.tracks.clear();
    state.masterFx = MasterFxSettings{}; // files without master-FX keys mean "defaults", not "keep previous"
    state.automation.clear();
    state.markers.clear();
    outFirstPatchName = "";

    std::string lineStr;
    int lineNumber = 0;
    std::string currentSection = "";
    std::string currentEntityName = "";
    // live-PLAN L2: (trackIndex, raw mini-notation text) pairs, compiled once
    // the whole file is read (below) so a hand-authored [TRACK] section that
    // precedes [GLOBAL] still sees the right LOOP= bounds.
    std::vector<std::pair<size_t, std::string>> pendingPatterns;

    while (std::getline(file, lineStr)) {
        lineNumber++;
        std::string_view line = stripComment(trim(lineStr));
        if (line.empty()) continue;

        if (line[0] == '[' && line.back() == ']') {
            std::string_view header = line.substr(1, line.length() - 2);
            size_t spacePos = header.find(' ');
            if (spacePos != std::string_view::npos) {
                currentSection = std::string(header.substr(0, spacePos));
                currentEntityName = std::string(header.substr(spacePos + 1));
            } else {
                currentSection = std::string(header);
                currentEntityName = "";
            }

            if (currentSection == "PATCH") {
                if (outFirstPatchName.empty()) {
                    outFirstPatchName = currentEntityName;
                }
                Patch newPatch;
                newPatch.name = currentEntityName;
                state.patches[currentEntityName] = newPatch;
            } else if (currentSection == "TRACK") {
                Track newTrack;
                newTrack.patchName = currentEntityName;
                state.tracks.push_back(newTrack);
            } else if (currentSection == "AUTOMATION") {
                // "[AUTOMATION <Track> <target>]" — currentEntityName is
                // "<Track> <target>" (header.find(' ') above only split off
                // "AUTOMATION"); split it again on its own first space.
                AutomationLane lane;
                size_t sp = currentEntityName.find(' ');
                if (sp != std::string::npos) {
                    lane.trackTarget = currentEntityName.substr(0, sp);
                    lane.paramTarget = currentEntityName.substr(sp + 1);
                } else {
                    lane.trackTarget = currentEntityName;
                }
                state.automation.push_back(std::move(lane));
            }
            continue;
        }

        if (currentSection == "GLOBAL") {
            auto tokens = split(line, '=');
            if (tokens.size() == 2) {
                try {
                    if (tokens[0] == "BPM") state.bpm.store(std::stof(std::string(tokens[1])));
                    else if (tokens[0] == "MASTER_VOL") state.masterVolume.store(std::stof(std::string(tokens[1])));
                    else if (tokens[0] == "TUNING") state.tuning.store(std::stof(std::string(tokens[1])));
                    else if (tokens[0] == "MASTER_DRIVE") state.masterFx.masterDrive = std::stof(std::string(tokens[1]));
                    else if (tokens[0] == "DELAY") {
                        auto vals = split(tokens[1], ',');
                        if (vals.size() >= 3) {
                            state.masterFx.delayTimeMs = std::stof(std::string(vals[0]));
                            state.masterFx.delayFeedback = std::stof(std::string(vals[1]));
                            state.masterFx.delayMix = std::stof(std::string(vals[2]));
                        }
                    }
                    else if (tokens[0] == "REVERB") {
                        auto vals = split(tokens[1], ',');
                        if (vals.size() >= 3) {
                            state.masterFx.reverbRoom = std::stof(std::string(vals[0]));
                            state.masterFx.reverbDamp = std::stof(std::string(vals[1]));
                            state.masterFx.reverbMix = std::stof(std::string(vals[2]));
                        }
                    }
                    else if (tokens[0] == "SIDECHAIN") {
                        auto vals = split(tokens[1], ',');
                        if (vals.size() >= 3) {
                            state.masterFx.sidechainEnabled = std::stof(std::string(vals[0]));
                            state.masterFx.sidechainAmount = std::stof(std::string(vals[1]));
                            state.masterFx.sidechainReleaseMs = std::stof(std::string(vals[2]));
                        }
                    }
                    else if (tokens[0] == "LOOP") {
                        // LOOP=StartBeat,EndBeat (live-PLAN L1). Presence of
                        // this key means the loop is enabled; a project with
                        // no LOOP= line keeps loopEnabled at its false
                        // default, same "unrecognized/absent keys are
                        // skipped" discipline as the rest of this format.
                        auto vals = split(tokens[1], ',');
                        if (vals.size() >= 2) {
                            state.loopStartBeat.store(std::stof(std::string(vals[0])));
                            state.loopEndBeat.store(std::stof(std::string(vals[1])));
                            state.loopEnabled.store(true);
                        }
                    }
                    else if (tokens[0] == "MARKER") {
                        // MARKER=Beat,Name — Name may itself contain commas, so
                        // split only on the FIRST one (unlike the numeric-only
                        // vals above).
                        size_t commaPos = tokens[1].find(',');
                        if (commaPos != std::string_view::npos) {
                            Marker m;
                            m.beat = std::stof(std::string(trim(tokens[1].substr(0, commaPos))));
                            m.name = std::string(trim(tokens[1].substr(commaPos + 1)));
                            state.markers.push_back(std::move(m));
                        } else {
                            std::cerr << "AdxParser Warning: Malformed MARKER on line " << lineNumber << std::endl;
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "AdxParser Warning: Malformed GLOBAL parameter on line " << lineNumber << std::endl;
                }
            }
        } else if (currentSection == "PATCH") {
            auto tokens = split(line, '=');
            if (tokens.size() == 2 && state.patches.count(currentEntityName) > 0) {
                Patch& p = state.patches[currentEntityName];
                if (tokens[0] == "ENVELOPE") {
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 4) {
                        try {
                            p.attackMs = std::stof(std::string(vals[0])) * 1000.0f;
                            p.decayMs = std::stof(std::string(vals[1])) * 1000.0f;
                            p.sustainLevel = std::clamp(std::stof(std::string(vals[2])), 0.0f, 1.0f);
                            p.releaseMs = std::stof(std::string(vals[3])) * 1000.0f;
                        } catch (const std::exception& e) {
                            std::cerr << "AdxParser Warning: Malformed ENVELOPE on line " << lineNumber << std::endl;
                        }
                    }
                } else if (tokens[0] == "DRIVE") {
                    try { p.drive = std::stof(std::string(tokens[1])); } catch (...) {}
                } else if (tokens[0] == "FILTER") {
                    // FILTER=CutoffHz, LfoRateHz, LfoDepth
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 3) {
                        try {
                            p.filterCutoffHz = std::stof(std::string(vals[0]));
                            p.filterLfoRateHz = std::stof(std::string(vals[1]));
                            p.filterLfoDepth = std::stof(std::string(vals[2]));
                        } catch (...) {}
                    }
                } else if (tokens[0] == "SUB") {
                    // SUB=Level, Wave, DropSemitones, DropMs
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 4) {
                        try {
                            p.subOscLevel = std::stof(std::string(vals[0]));
                            p.subOscWave = static_cast<int>(std::stof(std::string(vals[1])));
                            p.pitchDropSemitones = std::stof(std::string(vals[2]));
                            p.pitchDropMs = std::stof(std::string(vals[3]));
                        } catch (...) {}
                    }
                } else if (tokens[0] == "NOISE") {
                    // NOISE=Level, Type (0 = white, 1 = pink) [P3]
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 2) {
                        try {
                            p.noiseLevel = std::stof(std::string(vals[0]));
                            p.noiseType = static_cast<int>(std::stof(std::string(vals[1])));
                        } catch (...) {}
                    }
                } else if (tokens[0] == "RESFILTER") {
                    // RESFILTER=Type, Cutoff, Resonance, EnvAmt, KeyTrack [P3]
                    // (Type: 0 = LP, 1 = BP, 2 = HP)
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 5) {
                        try {
                            p.resFilterType = static_cast<int>(std::stof(std::string(vals[0])));
                            p.resFilterCutoff = std::stof(std::string(vals[1]));
                            p.resFilterResonance = std::stof(std::string(vals[2]));
                            p.filterEnvAmount = std::stof(std::string(vals[3]));
                            p.keyTrack = std::stof(std::string(vals[4]));
                        } catch (...) {}
                    }
                } else if (tokens[0] == "FILTERENV") {
                    // FILTERENV=Attack, Decay, Sustain, Release (seconds, same
                    // units as ENVELOPE) [P3]
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 4) {
                        try {
                            p.filterEnvAttackMs = std::stof(std::string(vals[0])) * 1000.0f;
                            p.filterEnvDecayMs = std::stof(std::string(vals[1])) * 1000.0f;
                            p.filterEnvSustainLevel = std::clamp(std::stof(std::string(vals[2])), 0.0f, 1.0f);
                            p.filterEnvReleaseMs = std::stof(std::string(vals[3])) * 1000.0f;
                        } catch (...) {}
                    }
                } else if (tokens[0] == "FORMANT") {
                    // FORMANT=VowelA, VowelB, Morph, Amount [P4]
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 4) {
                        p.formantVowelA = std::string(vals[0]);
                        p.formantVowelB = std::string(vals[1]);
                        try {
                            p.formantMorph = std::clamp(std::stof(std::string(vals[2])), 0.0f, 1.0f);
                            p.formantAmount = std::clamp(std::stof(std::string(vals[3])), 0.0f, 1.0f);
                        } catch (...) {}
                    }
                } else if (tokens[0] == "VIBRATO") {
                    // VIBRATO=RateHz, DepthCents, DelayMs [P4]
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 3) {
                        try {
                            p.vibratoRateHz = std::stof(std::string(vals[0]));
                            p.vibratoDepthCents = std::stof(std::string(vals[1]));
                            p.vibratoDelayMs = std::stof(std::string(vals[2]));
                        } catch (...) {}
                    }
                } else if (tokens[0] == "GLIDE") {
                    // GLIDE=Ms [P4]
                    try { p.glideMs = std::stof(std::string(tokens[1])); } catch (...) {}
                } else if (tokens[0] == "OSC") {
                    // OSC=Wave(0=off,1=Saw,2=Square,3=Triangle), UnisonVoices, DetuneCents, PulseWidth [P5]
                    auto vals = split(tokens[1], ',');
                    if (vals.size() >= 4) {
                        try {
                            p.oscWave = static_cast<int>(std::stof(std::string(vals[0])));
                            p.oscUnisonVoices = static_cast<int>(std::stof(std::string(vals[1])));
                            p.oscDetuneCents = std::stof(std::string(vals[2]));
                            p.oscPulseWidth = std::stof(std::string(vals[3]));
                        } catch (...) {}
                    }
                } else if (tokens[0] == "HARMONICS") {
                    auto vals = split(tokens[1], ',');
                    TimbreKeyframe kf;
                    for (const auto& v : vals) {
                        try { kf.harmonics.push_back(std::stof(std::string(v))); } catch(...) { kf.harmonics.push_back(0.0f); }
                    }
                    p.timbreKeyframes.clear();
                    p.timbreKeyframes.push_back(kf);
                }
            }
        } else if (currentSection == "TRACK") {
            if (!state.tracks.empty()) {
                Track& t = state.tracks.back();

                // MIX=Volume,Pan (Phase 1) — key=value like GLOBAL/PATCH, so
                // it needs its own check before the space-tokenized note/ARP/
                // EFFECT/CLIP formats below.
                if (line.size() >= 4 && line.substr(0, 4) == "MIX=") {
                    auto vals = split(line.substr(4), ',');
                    if (vals.size() >= 2) {
                        try {
                            t.volume = std::stof(std::string(vals[0]));
                            t.pan = std::stof(std::string(vals[1]));
                        } catch (const std::exception&) {
                            std::cerr << "AdxParser Warning: Malformed MIX on line " << lineNumber << std::endl;
                        }
                    } else {
                        std::cerr << "AdxParser Warning: Malformed MIX format on line " << lineNumber << std::endl;
                    }
                    continue;
                }

                // SEND=BusName,Amount (Phase 5) — aux send into the shared
                // master Delay/Reverb bus. Same key=value shape as MIX above.
                if (line.size() >= 5 && line.substr(0, 5) == "SEND=") {
                    auto vals = split(line.substr(5), ',');
                    if (vals.size() >= 2) {
                        std::string busName(vals[0]);
                        try {
                            float amount = std::stof(std::string(vals[1]));
                            if (busName == "Delay") t.sendDelayAmount = amount;
                            else if (busName == "Reverb") t.sendReverbAmount = amount;
                            else std::cerr << "AdxParser Warning: Unknown SEND bus '" << busName << "' on line " << lineNumber << std::endl;
                        } catch (const std::exception&) {
                            std::cerr << "AdxParser Warning: Malformed SEND on line " << lineNumber << std::endl;
                        }
                    } else {
                        std::cerr << "AdxParser Warning: Malformed SEND format on line " << lineNumber << std::endl;
                    }
                    continue;
                }

                // PATTERN=<mini-notation> (live-PLAN L2) — recorded here,
                // compiled into t.notes below once GLOBAL's LOOP= (if any)
                // has definitely been read. Coexists with hand-authored NOTE
                // lines below (both add to t.notes; PATTERN augments rather
                // than replaces).
                if (line.size() >= 8 && line.substr(0, 8) == "PATTERN=") {
                    pendingPatterns.emplace_back(state.tracks.size() - 1, std::string(line.substr(8)));
                    continue;
                }

                // Format: Note StartBeat Duration Velocity
                // or:     CLIP FilePath StartTimeSeconds [PitchShiftSemitones TimeStretchFactor]
                // (FilePath must not contain spaces; the trailing pitch/stretch pair is
                // optional for backward compatibility with pre-Phase-3 .adx files)
                auto tokens = split(line, ' ');
                // Filter empty tokens
                std::vector<std::string_view> cleanTokens;
                for (const auto& tk : tokens) {
                    if (!tk.empty()) cleanTokens.push_back(tk);
                }

                if (!cleanTokens.empty() && cleanTokens[0] == "ARP") {
                    // Format: ARP Mode RateBeats Octaves Gate
                    if (cleanTokens.size() == 5) {
                        try {
                            t.arp.mode = std::stoi(std::string(cleanTokens[1]));
                            t.arp.rateBeats = std::stof(std::string(cleanTokens[2]));
                            t.arp.octaves = std::stoi(std::string(cleanTokens[3]));
                            t.arp.gate = std::stof(std::string(cleanTokens[4]));
                        } catch (const std::exception&) {
                            std::cerr << "AdxParser Warning: Malformed ARP on line " << lineNumber << std::endl;
                        }
                    } else {
                        std::cerr << "AdxParser Warning: Malformed ARP format on line " << lineNumber << std::endl;
                    }
                } else if (!cleanTokens.empty() && cleanTokens[0] == "EFFECT") {
                    // Format: EFFECT Reverb Mix RoomSize Damping
                    //         EFFECT Distortion Drive Mix
                    //         EFFECT Bitcrush BitDepth RateHz Mix       [P5]
                    //         EFFECT Chorus Rate Depth Mix              [P5]
                    //         EFFECT EQ LowGainDb MidGainDb HighGainDb  [P5]
                    try {
                        if (cleanTokens.size() == 5 && cleanTokens[1] == "Reverb") {
                            t.effects.push_back(std::make_shared<ReverbEffect>(
                                std::stof(std::string(cleanTokens[2])),
                                std::stof(std::string(cleanTokens[3])),
                                std::stof(std::string(cleanTokens[4]))));
                        } else if (cleanTokens.size() == 4 && cleanTokens[1] == "Distortion") {
                            t.effects.push_back(std::make_shared<DistortionEffect>(
                                std::stof(std::string(cleanTokens[2])),
                                std::stof(std::string(cleanTokens[3]))));
                        } else if (cleanTokens.size() == 5 && cleanTokens[1] == "Bitcrush") {
                            t.effects.push_back(std::make_shared<BitcrushEffect>(
                                std::stof(std::string(cleanTokens[2])),
                                std::stof(std::string(cleanTokens[3])),
                                std::stof(std::string(cleanTokens[4]))));
                        } else if (cleanTokens.size() == 5 && cleanTokens[1] == "Chorus") {
                            t.effects.push_back(std::make_shared<ChorusEffect>(
                                std::stof(std::string(cleanTokens[2])),
                                std::stof(std::string(cleanTokens[3])),
                                std::stof(std::string(cleanTokens[4]))));
                        } else if (cleanTokens.size() == 5 && cleanTokens[1] == "EQ") {
                            t.effects.push_back(std::make_shared<EQEffect>(
                                std::stof(std::string(cleanTokens[2])),
                                std::stof(std::string(cleanTokens[3])),
                                std::stof(std::string(cleanTokens[4]))));
                        } else {
                            std::cerr << "AdxParser Warning: Unknown EFFECT on line " << lineNumber << std::endl;
                        }
                    } catch (const std::exception&) {
                        std::cerr << "AdxParser Warning: Malformed EFFECT on line " << lineNumber << std::endl;
                    }
                } else if (!cleanTokens.empty() && cleanTokens[0] == "CLIP") {
                    if (cleanTokens.size() == 3 || cleanTokens.size() == 5 || cleanTokens.size() == 6) {
                        try {
                            std::string filePath(cleanTokens[1]);
                            float startTimeSeconds = std::stof(std::string(cleanTokens[2]));
                            auto clip = AudioFileLoader::LoadAudioClip(filePath, startTimeSeconds);
                            if (clip) {
                                if (cleanTokens.size() >= 5) {
                                    clip->pitchShiftSemitones = std::stof(std::string(cleanTokens[3]));
                                    clip->timeStretchFactor = std::stof(std::string(cleanTokens[4]));
                                    clip->reversed = (cleanTokens.size() == 6 && cleanTokens[5] == "R");
                                    AudioClipProcessor::ReprocessClip(*clip);
                                }
                                t.audioClips.push_back(std::move(*clip));
                            } else {
                                std::cerr << "AdxParser Warning: Could not load audio clip on line " << lineNumber << std::endl;
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "AdxParser Warning: Malformed CLIP on line " << lineNumber << std::endl;
                        }
                    } else {
                        std::cerr << "AdxParser Warning: Malformed CLIP format on line " << lineNumber << std::endl;
                    }
                } else if (cleanTokens.size() == 4) {
                    try {
                        Note n;
                        n.pitch = NoteNameToMidi(cleanTokens[0]);
                        n.startBeat = std::stof(std::string(cleanTokens[1]));
                        n.lengthBeats = std::stof(std::string(cleanTokens[2]));
                        n.velocity = static_cast<uint8_t>(std::clamp(std::stof(std::string(cleanTokens[3])) * 127.0f, 0.0f, 127.0f));
                        t.notes.push_back(n);
                    } catch (const std::exception& e) {
                        std::cerr << "AdxParser Warning: Malformed note on line " << lineNumber << std::endl;
                    }
                } else {
                    std::cerr << "AdxParser Warning: Malformed note format on line " << lineNumber << std::endl;
                }
            }
        } else if (currentSection == "AUTOMATION") {
            // Format: Beat Value Curve  (curve: lin | exp | step | smooth)
            if (!state.automation.empty()) {
                auto tokens = split(line, ' ');
                std::vector<std::string_view> cleanTokens;
                for (const auto& tk : tokens) {
                    if (!tk.empty()) cleanTokens.push_back(tk);
                }
                if (cleanTokens.size() == 3) {
                    try {
                        Breakpoint bp;
                        bp.beat = std::stof(std::string(cleanTokens[0]));
                        bp.value = std::stof(std::string(cleanTokens[1]));
                        std::string curveStr(cleanTokens[2]);
                        std::transform(curveStr.begin(), curveStr.end(), curveStr.begin(), ::tolower);
                        if (curveStr == "exp") bp.curve = Curve::Exponential;
                        else if (curveStr == "step") bp.curve = Curve::Step;
                        else if (curveStr == "smooth") bp.curve = Curve::Smooth;
                        else bp.curve = Curve::Linear;
                        state.automation.back().points.push_back(bp);
                    } catch (const std::exception&) {
                        std::cerr << "AdxParser Warning: Malformed AUTOMATION breakpoint on line " << lineNumber << std::endl;
                    }
                } else {
                    std::cerr << "AdxParser Warning: Malformed AUTOMATION breakpoint format on line " << lineNumber << std::endl;
                }
            }
        }
    }

    // Breakpoints must be sorted ascending by beat — EvaluateAutomationLane
    // (and the .adx author) assume it, but don't enforce authoring order.
    for (auto& lane : state.automation) {
        std::sort(lane.points.begin(), lane.points.end(),
                   [](const Breakpoint& a, const Breakpoint& b) { return a.beat < b.beat; });
    }

    // live-PLAN L2: compile every PATTERN= line now that LOOP= (GLOBAL) has
    // definitely been read regardless of section order in the file. Compiled
    // notes are appended alongside any hand-authored NOTE lines the same
    // track already has.
    if (!pendingPatterns.empty()) {
        float loopStart = state.loopStartBeat.load();
        float loopEnd = state.loopEndBeat.load();
        if (loopEnd <= loopStart) loopEnd = loopStart + 4.0f; // PATTERN needs a positive span even with no/degenerate LOOP=
        for (const auto& [trackIdx, patternText] : pendingPatterns) {
            if (trackIdx >= state.tracks.size()) continue;
            PatternCompiler::CompileResult compiled = PatternCompiler::Compile(patternText, loopStart, loopEnd);
            if (compiled.ok) {
                Track& t = state.tracks[trackIdx];
                t.notes.insert(t.notes.end(), compiled.notes.begin(), compiled.notes.end());
            } else {
                std::cerr << "AdxParser Warning: PATTERN compile error in track '"
                          << state.tracks[trackIdx].patchName << "': " << compiled.error << std::endl;
            }
        }
    }

    // Every parsed patch needs playable envelope tables, not just whichever
    // one the caller wires into the PATCH EDITOR — see BuildEnvelopeTables.
    for (auto& [name, patch] : state.patches) {
        BuildEnvelopeTables(patch);
        BuildFilterEnvelopeTables(patch);
    }

    return true;
}

bool AdxParser::SaveProject(const std::string& filepath, const SequencerState& state) {
    std::ofstream file(filepath);
    if (!file.is_open()) return false;

    file << "# .adx Project File\n";

    file << "[GLOBAL]\n";
    file << "BPM=" << state.bpm.load() << "\n";
    file << "MASTER_VOL=" << state.masterVolume.load() << "\n";
    file << "TUNING=" << state.tuning.load() << "\n";
    file << "# Master FX: DELAY=TimeMs, Feedback, Mix | REVERB=Room, Damp, Mix | SIDECHAIN=Enabled, Amount, ReleaseMs\n";
    file << "DELAY=" << state.masterFx.delayTimeMs << ", " << state.masterFx.delayFeedback << ", " << state.masterFx.delayMix << "\n";
    file << "REVERB=" << state.masterFx.reverbRoom << ", " << state.masterFx.reverbDamp << ", " << state.masterFx.reverbMix << "\n";
    file << "SIDECHAIN=" << state.masterFx.sidechainEnabled << ", " << state.masterFx.sidechainAmount << ", " << state.masterFx.sidechainReleaseMs << "\n";
    file << "MASTER_DRIVE=" << state.masterFx.masterDrive << "\n";
    if (state.loopEnabled.load()) {
        file << "LOOP=" << state.loopStartBeat.load() << "," << state.loopEndBeat.load() << "\n";
    }
    if (!state.markers.empty()) {
        file << "# Arrangement markers: MARKER=Beat,Name\n";
        for (const auto& m : state.markers) {
            file << "MARKER=" << m.beat << "," << m.name << "\n";
        }
    }
    file << "\n";

    for (const auto& [name, patch] : state.patches) {
        file << "[PATCH " << name << "]\n";
        file << "# ADSR: Attack, Decay, Sustain, Release (0.0 to 10.0 seconds)\n";

        file << "ENVELOPE="
             << (patch.attackMs / 1000.0f) << ", "
             << (patch.decayMs / 1000.0f) << ", "
             << patch.sustainLevel << ", "
             << (patch.releaseMs / 1000.0f) << "\n";

        file << "# Harmonics: 16 values representing overtone amplitudes\n";
        file << "HARMONICS=";
        if (!patch.timbreKeyframes.empty()) {
            const auto& h = patch.timbreKeyframes[0].harmonics;
            for (size_t i = 0; i < 16; ++i) {
                file << (i < h.size() ? h[i] : 0.0f) << (i < 15 ? ", " : "");
            }
        } else {
            for (int i=0; i<16; ++i) file << (i==0 ? "1.0" : "0.0") << (i<15 ? ", " : "");
        }
        file << "\n";
        file << "# Suite params: DRIVE=x | FILTER=CutoffHz, LfoRateHz, LfoDepth | SUB=Level, Wave, DropSemitones, DropMs\n";
        file << "DRIVE=" << patch.drive << "\n";
        file << "FILTER=" << patch.filterCutoffHz << ", " << patch.filterLfoRateHz << ", " << patch.filterLfoDepth << "\n";
        file << "SUB=" << patch.subOscLevel << ", " << patch.subOscWave << ", "
             << patch.pitchDropSemitones << ", " << patch.pitchDropMs << "\n";
        file << "# Phase 3: NOISE=Level,Type(0=white,1=pink) | RESFILTER=Type(0=LP,1=BP,2=HP),Cutoff,Resonance,EnvAmt,KeyTrack | FILTERENV=Attack,Decay,Sustain,Release (seconds)\n";
        file << "NOISE=" << patch.noiseLevel << ", " << patch.noiseType << "\n";
        file << "RESFILTER=" << patch.resFilterType << ", " << patch.resFilterCutoff << ", "
             << patch.resFilterResonance << ", " << patch.filterEnvAmount << ", " << patch.keyTrack << "\n";
        file << "FILTERENV=" << (patch.filterEnvAttackMs / 1000.0f) << ", " << (patch.filterEnvDecayMs / 1000.0f) << ", "
             << patch.filterEnvSustainLevel << ", " << (patch.filterEnvReleaseMs / 1000.0f) << "\n";
        file << "# Phase 4: FORMANT=VowelA,VowelB,Morph,Amount | VIBRATO=RateHz,DepthCents,DelayMs | GLIDE=Ms\n";
        file << "FORMANT=" << patch.formantVowelA << ", " << patch.formantVowelB << ", "
             << patch.formantMorph << ", " << patch.formantAmount << "\n";
        file << "VIBRATO=" << patch.vibratoRateHz << ", " << patch.vibratoDepthCents << ", " << patch.vibratoDelayMs << "\n";
        file << "GLIDE=" << patch.glideMs << "\n";
        file << "# Phase 5: OSC=Wave(0=off,1=Saw,2=Square,3=Triangle),UnisonVoices,DetuneCents,PulseWidth\n";
        file << "OSC=" << patch.oscWave << ", " << patch.oscUnisonVoices << ", "
             << patch.oscDetuneCents << ", " << patch.oscPulseWidth << "\n\n";
    }

    for (const auto& track : state.tracks) {
        file << "[TRACK " << track.patchName << "]\n";
        file << "MIX=" << track.volume << ", " << track.pan << "  # volume, pan\n";
        if (track.sendDelayAmount != 0.0f) {
            file << "SEND=Delay, " << track.sendDelayAmount << "  # aux send amount [P5]\n";
        }
        if (track.sendReverbAmount != 0.0f) {
            file << "SEND=Reverb, " << track.sendReverbAmount << "  # aux send amount [P5]\n";
        }
        if (track.arp.mode != 0) {
            file << "# Format: ARP Mode RateBeats Octaves Gate\n";
            file << "ARP " << track.arp.mode << " " << track.arp.rateBeats << " "
                 << track.arp.octaves << " " << track.arp.gate << "\n";
        }
        file << "# Format: Note StartBeat Duration Velocity\n";
        for (const auto& note : track.notes) {
            file << MidiToNoteName(note.pitch) << " "
                 << std::fixed << std::setprecision(2) << note.startBeat << " "
                 << note.lengthBeats << " "
                 << (note.velocity / 127.0f) << "\n";
        }
        if (!track.audioClips.empty()) {
            file << "# Format: CLIP FilePath StartTimeSeconds PitchShiftSemitones TimeStretchFactor\n";
            for (const auto& clip : track.audioClips) {
                file << "CLIP " << clip.filePath << " "
                     << std::fixed << std::setprecision(3) << clip.startTimeSeconds << " "
                     << clip.pitchShiftSemitones << " "
                     << clip.timeStretchFactor
                     << (clip.reversed ? " R" : "") << "\n";
            }
        }
        if (!track.effects.empty()) {
            file << "# Format: EFFECT Reverb Mix RoomSize Damping | EFFECT Distortion Drive Mix | "
                 << "EFFECT Bitcrush BitDepth RateHz Mix | EFFECT Chorus Rate Depth Mix | EFFECT EQ LowGainDb MidGainDb HighGainDb\n";
            for (const auto& fx : track.effects) {
                if (!fx) continue;
                if (const auto* rev = dynamic_cast<const ReverbEffect*>(fx.get())) {
                    file << "EFFECT Reverb " << std::fixed << std::setprecision(3)
                         << rev->mix.load() << " " << rev->roomSize.load() << " " << rev->damping.load() << "\n";
                } else if (const auto* dist = dynamic_cast<const DistortionEffect*>(fx.get())) {
                    file << "EFFECT Distortion " << std::fixed << std::setprecision(3)
                         << dist->drive.load() << " " << dist->mix.load() << "\n";
                } else if (const auto* bc = dynamic_cast<const BitcrushEffect*>(fx.get())) {
                    file << "EFFECT Bitcrush " << std::fixed << std::setprecision(3)
                         << bc->bitDepth.load() << " " << bc->rateHz.load() << " " << bc->mix.load() << "\n";
                } else if (const auto* ch = dynamic_cast<const ChorusEffect*>(fx.get())) {
                    file << "EFFECT Chorus " << std::fixed << std::setprecision(3)
                         << ch->rateHz.load() << " " << ch->depth.load() << " " << ch->mix.load() << "\n";
                } else if (const auto* eq = dynamic_cast<const EQEffect*>(fx.get())) {
                    file << "EFFECT EQ " << std::fixed << std::setprecision(3)
                         << eq->lowGainDb.load() << " " << eq->midGainDb.load() << " " << eq->highGainDb.load() << "\n";
                }
            }
        }
        file << "\n";
    }

    if (!state.automation.empty()) {
        file << "# Automation: [AUTOMATION Track Target] then Beat Value Curve lines (curve: lin | exp | step | smooth)\n";
        for (const auto& lane : state.automation) {
            file << "[AUTOMATION " << lane.trackTarget << " " << lane.paramTarget << "]\n";
            for (const auto& bp : lane.points) {
                const char* curveStr = bp.curve == Curve::Exponential ? "exp"
                                      : bp.curve == Curve::Step        ? "step"
                                      : bp.curve == Curve::Smooth      ? "smooth"
                                                                        : "lin";
                file << std::fixed << std::setprecision(3) << bp.beat << " " << bp.value << " " << curveStr << "\n";
            }
            file << "\n";
        }
    }

    return true;
}
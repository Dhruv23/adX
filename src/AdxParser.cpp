#include "AdxParser.h"
#include "AudioFileLoader.h"
#include "AudioClipProcessor.h"
#include "AudioEffect.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <string_view>

static std::string_view trim(std::string_view str) {
    auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return (start < end) ? str.substr(std::distance(str.begin(), start), std::distance(start, end)) : std::string_view();
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
    outFirstPatchName = "";

    std::string lineStr;
    int lineNumber = 0;
    std::string currentSection = "";
    std::string currentEntityName = "";

    while (std::getline(file, lineStr)) {
        lineNumber++;
        std::string_view line = trim(lineStr);
        if (line.empty() || line[0] == '#') continue;

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
        }
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
    file << "MASTER_DRIVE=" << state.masterFx.masterDrive << "\n\n";

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
             << patch.pitchDropSemitones << ", " << patch.pitchDropMs << "\n\n";
    }

    for (const auto& track : state.tracks) {
        file << "[TRACK " << track.patchName << "]\n";
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
            file << "# Format: EFFECT Reverb Mix RoomSize Damping | EFFECT Distortion Drive Mix\n";
            for (const auto& fx : track.effects) {
                if (!fx) continue;
                if (const auto* rev = dynamic_cast<const ReverbEffect*>(fx.get())) {
                    file << "EFFECT Reverb " << std::fixed << std::setprecision(3)
                         << rev->mix.load() << " " << rev->roomSize.load() << " " << rev->damping.load() << "\n";
                } else if (const auto* dist = dynamic_cast<const DistortionEffect*>(fx.get())) {
                    file << "EFFECT Distortion " << std::fixed << std::setprecision(3)
                         << dist->drive.load() << " " << dist->mix.load() << "\n";
                }
            }
        }
        file << "\n";
    }

    return true;
}
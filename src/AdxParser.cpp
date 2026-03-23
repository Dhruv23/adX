#include "AdxParser.h"
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
                auto tokens = split(line, ' ');
                // Filter empty tokens
                std::vector<std::string_view> cleanTokens;
                for (const auto& tk : tokens) {
                    if (!tk.empty()) cleanTokens.push_back(tk);
                }

                if (cleanTokens.size() == 4) {
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
    file << "TUNING=" << state.tuning.load() << "\n\n";

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
        file << "\n\n";
    }

    for (const auto& track : state.tracks) {
        file << "[TRACK " << track.patchName << "]\n";
        file << "# Format: Note StartBeat Duration Velocity\n";
        for (const auto& note : track.notes) {
            file << MidiToNoteName(note.pitch) << " "
                 << std::fixed << std::setprecision(2) << note.startBeat << " "
                 << note.lengthBeats << " "
                 << (note.velocity / 127.0f) << "\n";
        }
        file << "\n";
    }

    return true;
}
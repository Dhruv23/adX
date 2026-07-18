#include "SampleBrowser.h"
#include "AudioFileLoader.h"
#include "AudioClipProcessor.h"
#include "BpmDetector.h"
#include "SequencerUI.h"

#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char* kSamplesDir = "samples";

struct SampleEntry {
    std::string path;     // relative path handed to AudioFileLoader
    std::string filename; // display name
    float detectedBpm = -1.0f; // -1 = not analyzed yet, 0 = analyzed, no pulse
};

std::vector<SampleEntry> s_entries;
int s_selectedIndex = -1;
std::chrono::steady_clock::time_point s_lastScan{};

void RescanSamplesDir() {
    // Preserve analysis results for files that are still present.
    std::map<std::string, float> knownBpm;
    for (const auto& e : s_entries) knownBpm[e.path] = e.detectedBpm;

    s_entries.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(kSamplesDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".wav" && ext != ".mp3") continue;

        SampleEntry se;
        se.path = entry.path().generic_string();
        se.filename = entry.path().filename().string();
        auto it = knownBpm.find(se.path);
        if (it != knownBpm.end()) se.detectedBpm = it->second;
        s_entries.push_back(std::move(se));
    }
    std::sort(s_entries.begin(), s_entries.end(),
              [](const SampleEntry& a, const SampleEntry& b) { return a.filename < b.filename; });
    if (s_selectedIndex >= static_cast<int>(s_entries.size())) s_selectedIndex = -1;
}

// Lazily runs BPM analysis for one entry (decode + autocorrelation, so only
// on explicit demand or selection — not for the whole directory at once).
void EnsureAnalyzed(SampleEntry& entry) {
    if (entry.detectedBpm >= 0.0f) return;
    auto mono = AudioFileLoader::DecodeMono(entry.path, kEngineSampleRate);
    entry.detectedBpm = mono ? BpmDetector::EstimateBpm(*mono, kEngineSampleRate) : 0.0f;
}

// Wraps a semitone interval into [-6, +6] so kick pitch-following moves to
// the nearest chromatic neighbor of the root instead of leaping octaves.
float WrapSemitones(int interval) {
    int wrapped = ((interval % 12) + 12) % 12; // 0..11
    if (wrapped > 6) wrapped -= 12;            // -5..6
    return static_cast<float>(wrapped);
}

// Phase 5.3: populate a new track with the selected kick sample on a fixed
// grid, pitch-shifting each kick to follow the root notes of the first track
// that has MIDI notes (typically the Phase 4 ML-extracted melody).
void GenerateHardstyleKicks(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue,
                            const std::string& samplePath, float spacingBeats, int fallbackBars) {
    float bpm = state.bpm.load();
    auto baseClip = AudioFileLoader::LoadAudioClip(samplePath, 0.0f);
    if (!baseClip) {
        std::cerr << "SampleBrowser: could not load kick sample " << samplePath << "\n";
        return;
    }

    // Find the melody source: first track with notes.
    const Track* melodyTrack = nullptr;
    for (const auto& t : state.tracks) {
        if (!t.notes.empty()) { melodyTrack = &t; break; }
    }

    float startBeat = 0.0f;
    float endBeat = static_cast<float>(fallbackBars * 4);
    uint8_t rootPitch = 60;
    if (melodyTrack) {
        float minBeat = 1e9f, maxBeat = 0.0f;
        for (const auto& n : melodyTrack->notes) {
            minBeat = std::min(minBeat, n.startBeat);
            maxBeat = std::max(maxBeat, n.startBeat + n.lengthBeats);
        }
        startBeat = std::floor(minBeat);
        endBeat = std::ceil(maxBeat);
        rootPitch = melodyTrack->notes.front().pitch;
    }
    if (endBeat <= startBeat) return;

    Track kickTrack;
    kickTrack.patchName = "Hardstyle Kicks";

    // One RubberBand pass per DISTINCT shift; kicks at the same pitch share
    // the processed PCM buffer via AudioClip's shared_ptr semantics.
    std::map<float, AudioClip> processedByShift;

    for (float beat = startBeat; beat < endBeat - 0.001f; beat += spacingBeats) {
        float shift = 0.0f;
        if (melodyTrack) {
            // Latest-starting note covering this beat wins (matches what is heard).
            const Note* covering = nullptr;
            for (const auto& n : melodyTrack->notes) {
                if (n.startBeat <= beat && beat < n.startBeat + n.lengthBeats) {
                    if (!covering || n.startBeat > covering->startBeat) covering = &n;
                }
            }
            if (covering) shift = WrapSemitones(static_cast<int>(covering->pitch) - static_cast<int>(rootPitch));
        }

        auto it = processedByShift.find(shift);
        if (it == processedByShift.end()) {
            AudioClip shifted = *baseClip;
            shifted.pitchShiftSemitones = shift;
            AudioClipProcessor::ReprocessClip(shifted);
            it = processedByShift.emplace(shift, std::move(shifted)).first;
        }

        AudioClip placed = it->second; // shares pcmData, cheap
        placed.startTimeSeconds = beat * 60.0f / bpm;
        kickTrack.audioClips.push_back(std::move(placed));
    }

    if (kickTrack.audioClips.empty()) return;

    state.tracks.push_back(std::move(kickTrack));
    SelectTrack(static_cast<int>(state.tracks.size()) - 1);
    DispatchSequenceUpdate(state, eventQueue);
    std::cout << "SampleBrowser: generated " << state.tracks.back().audioClips.size()
              << " kicks (" << (melodyTrack ? "melody-following" : "no melody found, flat pitch") << ")\n";
}

} // namespace

void DrawSampleBrowser(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    auto now = std::chrono::steady_clock::now();
    if (s_lastScan.time_since_epoch().count() == 0 || now - s_lastScan >= std::chrono::seconds(3)) {
        s_lastScan = now;
        RescanSamplesDir();
    }

    ImGui::SetNextWindowSize(ImVec2(320.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SAMPLE BROWSER")) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("./%s — drag onto the Arranger timeline", kSamplesDir);
    ImGui::SameLine();
    if (ImGui::SmallButton("REFRESH")) RescanSamplesDir();
    ImGui::Separator();

    ImGui::BeginChild("SampleList", ImVec2(0, -130.0f), true);
    if (s_entries.empty()) {
        ImGui::TextDisabled("No .wav/.mp3 files found in ./%s", kSamplesDir);
    }
    for (int i = 0; i < static_cast<int>(s_entries.size()); ++i) {
        auto& entry = s_entries[i];
        ImGui::PushID(i);
        if (ImGui::Selectable(entry.filename.c_str(), s_selectedIndex == i)) {
            s_selectedIndex = i;
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ADX_SAMPLE_PATH", entry.path.c_str(), entry.path.size() + 1);
            ImGui::Text("%s", entry.filename.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered() && entry.detectedBpm >= 0.0f) {
            if (entry.detectedBpm > 0.0f) ImGui::SetTooltip("Detected BPM: %.1f", entry.detectedBpm);
            else ImGui::SetTooltip("One-shot (no BPM)");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // --- Selected sample info / analysis ---
    if (s_selectedIndex >= 0 && s_selectedIndex < static_cast<int>(s_entries.size())) {
        auto& sel = s_entries[s_selectedIndex];
        ImGui::Text("%s", sel.filename.c_str());
        if (sel.detectedBpm < 0.0f) {
            if (ImGui::SmallButton("DETECT BPM")) EnsureAnalyzed(sel);
        } else if (sel.detectedBpm > 0.0f) {
            ImGui::SameLine();
            ImGui::Text("BPM: %.1f", sel.detectedBpm);
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled("(one-shot)");
        }
    } else {
        ImGui::TextDisabled("Select a sample below to enable generation");
    }

    // --- Phase 5.3: Hardstyle Auto-Generation macro ---
    ImGui::Separator();
    ImGui::Text("HARDSTYLE AUTO-GENERATION");

    static int spacingChoice = 1; // default 1/2 beat per the plan (1, 1.5, 2, 2.5...)
    static int fallbackBars = 8;
    const char* spacingLabels[] = {"Every beat", "Every 1/2 beat", "Every 1/4 beat"};
    const float spacingValues[] = {1.0f, 0.5f, 0.25f};
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Kick spacing", &spacingChoice, spacingLabels, 3);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Bars (if no melody)", &fallbackBars, 1, 64);

    bool canGenerate = s_selectedIndex >= 0 && s_selectedIndex < static_cast<int>(s_entries.size());
    ImGui::BeginDisabled(!canGenerate);
    if (ImGui::Button("GENERATE KICK TRACK", ImVec2(-1.0f, 0.0f))) {
        GenerateHardstyleKicks(state, eventQueue, s_entries[s_selectedIndex].path,
                               spacingValues[spacingChoice], fallbackBars);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Places the selected kick on the grid, pitch-shifted to follow\nthe root notes of the first track containing MIDI notes.");
    }

    ImGui::End();
}

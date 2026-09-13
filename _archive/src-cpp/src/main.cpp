#include "AudioData.h"
#include "AudioEngine.h"
#include "SequencerUI.h"
#include "SampleBrowser.h"
#include "AdxParser.h"
#include "MelodyExtractor.h"
#include "MidiImporter.h"
#include "TimbreAnalyzer.h"
#include "ExportRenderer.h"
#include "LiveCodeEditor.h"
#include "SimpleFFT.h"
#include "ParticleVisualizer.h"
#include "SpectrogramHistory.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // consistent with the rest of the project; must precede <windows.h>
#endif
#include <windows.h> // GetModuleFileNameA, for resolving Cousine-Regular.ttf next to the exe
#endif
#include <portable-file-dialogs.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <RtAudio.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API for the default dock layout
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <readerwriterqueue.h>
#include <cmath>
#include <algorithm>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
// --- Custom Styling Constants ---
const ImU32 COLOR_ATTACK = IM_COL32(255, 60, 60, 255);
const ImU32 COLOR_DECAY = IM_COL32(255, 200, 50, 255);
const ImU32 COLOR_RELEASE = IM_COL32(50, 200, 255, 255);
const ImU32 COLOR_GRID_LINES = IM_COL32(26, 26, 36, 255);
const ImU32 COLOR_TEXT = IM_COL32(255, 255, 255, 255);

// Cousine-Regular.ttf (imgui's own bundled monospace font, no new dependency)
// lives next to the executable, copied there by CMakeLists.txt's post-build
// step — same resolution strategy as MelodyExtractor's nmp.onnx, since a bare
// relative path breaks when the process's cwd isn't the exe's own directory.
static std::string GetMonoFontPath() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string exePath(buffer, len);
        size_t pos = exePath.find_last_of("\\/");
        if (pos != std::string::npos) {
            return exePath.substr(0, pos + 1) + "Cousine-Regular.ttf";
        }
    }
#endif
    return "Cousine-Regular.ttf"; // fallback: cwd-relative (also the only option on non-Windows for now)
}

void SetupImGuiStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();

    // Pure black backgrounds
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    // Text color
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    // Denser DAW-style toolbar/frame padding (UI-Refactor Phase 1) — tighter
    // than ImGui defaults so the transport bar and track rows read as compact
    // chrome rather than default-spaced widgets.
    style.FramePadding = ImVec2(4.0f, 2.0f);
    style.ItemSpacing = ImVec2(6.0f, 4.0f);

    // Panels/clips (drawn via explicit AddRectFilled calls, not ImGui frame
    // rounding) stay hard-edged; only interactive frames (buttons, sliders,
    // tabs) get a subtle pill-like rounding to match the reference toolbar.
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 0.0f;
    style.TabRounding = 0.0f;

    // Thin low-alpha hairline dividers (track rows, panel edges, tab-bar
    // underline) instead of the previous zero-border flat look.
    style.Colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    style.Colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBarBorderSize = 1.0f;
}

// --- Globals ---
constexpr unsigned int SAMPLE_RATE = kEngineSampleRate;
constexpr unsigned int BUFFER_FRAMES = 512;
constexpr unsigned int OUT_CHANNELS = 2;

// The global SequencerState managed by the Main Thread
SequencerState state;

// Helper to generate a basic lookup table for testing
std::vector<float> generateLinearTable(float start, float end, float timeSeconds, unsigned int sampleRate)
{
    size_t samples = static_cast<size_t>(timeSeconds * sampleRate);
    std::vector<float> table(samples);
    for (size_t i = 0; i < samples; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(samples);
        table[i] = std::lerp(start, end, t);
    }
    return table;
}

// Evaluate cubic Bezier for X and Y components separately
float evalBezier(float p0, float p1, float p2, float p3, float t)
{
    float u = 1.0f - t;
    float tt = t * t;
    float uu = u * u;
    float uuu = uu * u;
    float ttt = tt * t;
    return uuu * p0 + 3.0f * uu * t * p1 + 3.0f * u * tt * p2 + ttt * p3;
}

// Find t given X (Newton-Raphson or binary search since X is monotonic)
float findTForBezierX(float targetX, float p0x, float p1x, float p2x, float p3x)
{
    float t = targetX; // Initial guess
    for (int i = 0; i < 8; ++i)
    {
        float currentX = evalBezier(p0x, p1x, p2x, p3x, t);
        float error = currentX - targetX;
        if (std::abs(error) < 0.001f)
            return t;

        // Derivative of cubic bezier X(t)
        float u = 1.0f - t;
        float dx = 3.0f * u * u * (p1x - p0x) + 6.0f * u * t * (p2x - p1x) + 3.0f * t * t * (p3x - p2x);

        if (std::abs(dx) < 0.0001f)
            break;
        t -= error / dx;
        t = std::clamp(t, 0.0f, 1.0f);
    }
    return t;
}

std::vector<float> generateBezierTable(int samples, float startY, float p1y, float p2y, float endY, float p1x, float p2x)
{
    std::vector<float> table(samples);
    for (int i = 0; i < samples; ++i)
    {
        float targetX = static_cast<float>(i) / static_cast<float>(samples - 1);
        float t = findTForBezierX(targetX, 0.0f, p1x, p2x, 1.0f);
        table[i] = evalBezier(startY, p1y, p2y, endY, t);
    }
    return table;
}

// Resample normalized table to match time in milliseconds
std::vector<float> scaleTableToTime(const std::vector<float> &normalizedTable, float timeMs, unsigned int sampleRate)
{
    size_t requiredSamples = static_cast<size_t>((timeMs / 1000.0f) * sampleRate);
    if (requiredSamples == 0)
        return {normalizedTable.back()};

    std::vector<float> scaledTable(requiredSamples);
    for (size_t i = 0; i < requiredSamples; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(requiredSamples - 1);
        float normIdx = t * static_cast<float>(normalizedTable.size() - 1);
        size_t idx1 = static_cast<size_t>(normIdx);
        size_t idx2 = std::min(idx1 + 1, normalizedTable.size() - 1);
        float frac = normIdx - static_cast<float>(idx1);
        scaledTable[i] = std::lerp(normalizedTable[idx1], normalizedTable[idx2], frac);
    }
    return scaledTable;
}

// Editor State
struct ADSRControlPoints
{
    ImVec2 p1{0.3f, 0.3f};
    ImVec2 p2{0.7f, 0.7f};
};

ADSRControlPoints attackPts, decayPts, releasePts;
float attackMs = 100.0f;
float decayMs = 100.0f;
float releaseMs = 500.0f;
float sustainLvl = 0.7f;
Patch draftPatch;

// Live-coding hot reload: which project file to watch, and its last known
// write time (empty/default until a project is LOADed or SAVEd).
std::string g_loadedProjectPath;
std::filesystem::file_time_type g_lastKnownWriteTime{};

// live-PLAN Phase L3: the "LIVE CODE" panel's in-app editor, mirroring
// g_loadedProjectPath's file (see LoadProjectAndSync/CheckForHotReload,
// which call SyncFromDisk to keep it in step).
LiveCodeEditor g_liveCodeEditor;

// ML melody extraction: runs on a background std::thread (potentially
// multi-second full-song ONNX inference, unlike Phase 3's quick per-clip
// reprocessing) with a simple atomic-flag handoff back to the Main Thread.
// resultNotes/success are written by the worker before done=true, and only
// read on the Main Thread after observing done==true — the atomic store/load
// pair establishes the happens-before relationship, no mutex needed.
struct MelodyExtractionJob {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    bool success = false;
    std::vector<Note> resultNotes;
    // Captured at job-start (not completion) so a LOAD/hot-reload that swaps
    // draftPatch/state.patches while extraction runs in the background can't
    // make the finished job stamp the new track with a patch name that no
    // longer exists in the (now different) project.
    std::string patchName;
};
MelodyExtractionJob g_melodyJob;
MelodyExtractor g_melodyExtractor;

// Algorithmic MIDI import (+ optional reference-audio timbre analysis): same
// atomic-flag handoff as MelodyExtractionJob. Parsing the .mid is fast, but
// the optional TimbreAnalyzer pass decodes and scans a full mp3, so the whole
// job lives on a background thread anyway.
struct MidiImportJob {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    bool success = false;
    std::vector<MidiImporter::ImportedTrack> resultTracks;
    float suggestedBpm = 0.0f;      // .mid tempo meta; 0 = keep project BPM
    bool hasAnalyzedPatch = false;  // TimbreAnalyzer succeeded on the reference audio
    Patch analyzedPatch;
    std::string patchName;          // fallback stamp, captured at job-start (same reasoning as MelodyExtractionJob)
};
MidiImportJob g_midiJob;

// Offline audio export: same atomic-flag handoff pattern as MelodyExtractionJob.
// result is written by the worker before done=true and only read on the Main
// Thread after observing done==true.
struct ExportJob {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<float> progress{0.0f};
    ExportRenderer::Result result;
};
ExportJob g_exportJob;
// Last completed export's outcome, shown in the EXPORT window (Main Thread only).
std::string g_lastExportMessage;

static void DispatchEngineParam(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, EngineParam id, float value) {
    AudioEvent evt{};
    evt.type = AudioEventType::ParameterChange;
    evt.data.paramData.paramId = static_cast<uint32_t>(id);
    evt.data.paramData.value = value;
    eventQueue.try_enqueue(evt);
}

// Mirrors every MasterFxSettings field to the audio thread (used after LOAD
// and hot reload; individual UI slider edits send just their own param).
static void DispatchAllMasterFx(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, const MasterFxSettings& fx) {
    DispatchEngineParam(eventQueue, EngineParam::DelayTimeMs, fx.delayTimeMs);
    DispatchEngineParam(eventQueue, EngineParam::DelayFeedback, fx.delayFeedback);
    DispatchEngineParam(eventQueue, EngineParam::DelayMix, fx.delayMix);
    DispatchEngineParam(eventQueue, EngineParam::ReverbRoom, fx.reverbRoom);
    DispatchEngineParam(eventQueue, EngineParam::ReverbDamp, fx.reverbDamp);
    DispatchEngineParam(eventQueue, EngineParam::ReverbMix, fx.reverbMix);
    DispatchEngineParam(eventQueue, EngineParam::SidechainEnabled, fx.sidechainEnabled);
    DispatchEngineParam(eventQueue, EngineParam::SidechainAmount, fx.sidechainAmount);
    DispatchEngineParam(eventQueue, EngineParam::SidechainReleaseMs, fx.sidechainReleaseMs);
    DispatchEngineParam(eventQueue, EngineParam::MasterDrive, fx.masterDrive);
}

// C418/STAKILLAZ suites: floating window with the factory patch library and
// the draft patch's synth-extra parameters (filter/LFO, drive, sub, drop).
static void DrawPatchSuiteWindow(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PATCH SUITE")) {
        ImGui::End();
        return;
    }

    // Factory patch library
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##PatchLibrary", "LOAD FACTORY PATCH...")) {
        for (const auto& name : PatchLibrary::Names()) {
            if (ImGui::Selectable(name.c_str())) {
                draftPatch = PatchLibrary::Create(name);
                // Sync the envelope editor's scalar state; the factory's
                // pre-built tables are kept until the user edits a curve.
                attackMs = draftPatch.attackMs;
                decayMs = draftPatch.decayMs;
                releaseMs = draftPatch.releaseMs;
                sustainLvl = draftPatch.sustainLevel;
                state.patches[draftPatch.name] = draftPatch;
                DispatchSequenceUpdate(state, eventQueue);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();

    bool extrasChanged = false;
    auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt, ImGuiSliderFlags flags = 0) {
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(label, v, lo, hi, fmt, flags)) extrasChanged = true;
    };

    ImGui::Text("C418: FILTER");
    slider("Cutoff", &draftPatch.filterCutoffHz, 100.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
    slider("LFO Rate", &draftPatch.filterLfoRateHz, 0.0f, 8.0f, "%.2f Hz");
    slider("LFO Depth", &draftPatch.filterLfoDepth, 0.0f, 1.0f, "%.2f");

    ImGui::Separator();
    ImGui::Text("STAKILLAZ: DRIVE / SUB / DROP");
    slider("Drive", &draftPatch.drive, 0.0f, 30.0f, "%.1f");
    slider("Sub Level", &draftPatch.subOscLevel, 0.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo("Sub Wave", &draftPatch.subOscWave, "Sine\0Triangle\0")) extrasChanged = true;
    slider("Pitch Drop", &draftPatch.pitchDropSemitones, 0.0f, 48.0f, "%.0f st");
    slider("Drop Time", &draftPatch.pitchDropMs, 5.0f, 500.0f, "%.0f ms");

    if (extrasChanged) {
        state.patches[draftPatch.name] = draftPatch;
        DispatchSequenceUpdate(state, eventQueue);
    }

    ImGui::End();
}

// Master-bus FX window (C418 delay/reverb + STAKILLAZ sidechain/drive).
// Edits update the Main Thread's authoritative copy in state.masterFx and
// send one ParameterChange event for just the touched field.
static void DrawMasterFxWindow(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MASTER FX")) {
        ImGui::End();
        return;
    }
    MasterFxSettings& fx = state.masterFx;

    auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt, EngineParam id, ImGuiSliderFlags flags = 0) {
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(label, v, lo, hi, fmt, flags)) {
            DispatchEngineParam(eventQueue, id, *v);
        }
    };

    ImGui::Text("PING-PONG DELAY");
    slider("Time", &fx.delayTimeMs, 1.0f, 1500.0f, "%.0f ms", EngineParam::DelayTimeMs);
    slider("Feedback", &fx.delayFeedback, 0.0f, 0.95f, "%.2f", EngineParam::DelayFeedback);
    slider("Mix##delay", &fx.delayMix, 0.0f, 1.0f, "%.2f", EngineParam::DelayMix);

    ImGui::Separator();
    ImGui::Text("REVERB");
    slider("Room", &fx.reverbRoom, 0.0f, 1.0f, "%.2f", EngineParam::ReverbRoom);
    slider("Damping", &fx.reverbDamp, 0.0f, 1.0f, "%.2f", EngineParam::ReverbDamp);
    slider("Mix##reverb", &fx.reverbMix, 0.0f, 1.0f, "%.2f", EngineParam::ReverbMix);

    ImGui::Separator();
    ImGui::Text("SIDECHAIN (Track 1 -> Master)");
    bool scOn = fx.sidechainEnabled >= 0.5f;
    if (ImGui::Checkbox("Enabled", &scOn)) {
        fx.sidechainEnabled = scOn ? 1.0f : 0.0f;
        DispatchEngineParam(eventQueue, EngineParam::SidechainEnabled, fx.sidechainEnabled);
    }
    slider("Amount", &fx.sidechainAmount, 0.0f, 1.0f, "%.2f", EngineParam::SidechainAmount);
    slider("Release", &fx.sidechainReleaseMs, 10.0f, 1000.0f, "%.0f ms", EngineParam::SidechainReleaseMs);

    ImGui::Separator();
    ImGui::Text("MASTER DRIVE");
    slider("Drive", &fx.masterDrive, 0.0f, 30.0f, "%.1f", EngineParam::MasterDrive);

    ImGui::End();
}

// EXPORT window: offline-renders the whole project to disk (WAV 16/24/32f,
// MP3, FLAC) through a fresh AudioEngine — see ExportRenderer. The render runs
// on a background std::thread; g_exportJob hands progress/result back to the
// Main Thread, which applies it in the main loop (same pattern as g_melodyJob).
static void DrawExportWindow() {
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("EXPORT")) {
        ImGui::End();
        return;
    }

    static constexpr ExportRenderer::Format kFormats[] = {
        ExportRenderer::Format::Wav16,
        ExportRenderer::Format::Wav24,
        ExportRenderer::Format::WavFloat32,
        ExportRenderer::Format::Mp3,
        ExportRenderer::Format::Flac,
    };
    static int s_formatIndex = 0;
    static float s_tailSeconds = 3.0f;

    ImGui::TextUnformatted("FORMAT");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##ExportFormat", ExportRenderer::FormatLabel(kFormats[s_formatIndex]))) {
        for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kFormats)); ++i) {
            if (ImGui::Selectable(ExportRenderer::FormatLabel(kFormats[i]), i == s_formatIndex)) {
                s_formatIndex = i;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Tail", &s_tailSeconds, 0.0f, 15.0f, "%.1f s");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Silence rendered after the last note so release\nenvelopes and delay/reverb tails are not cut off.");
    }

    float contentSeconds = ExportRenderer::EstimateContentSeconds(state.tracks, state.bpm.load());
    ImGui::Text("Length: %.1f s content + %.1f s tail", contentSeconds, s_tailSeconds);

    bool running = g_exportJob.running.load();
    bool nothingToExport = state.tracks.empty();
    ImGui::BeginDisabled(running || nothingToExport);
    if (ImGui::Button(running ? "EXPORTING..." : "EXPORT...", ImVec2(-1.0f, 30.0f))) {
        ExportRenderer::Format format = kFormats[s_formatIndex];
        const std::string ext = ExportRenderer::FormatExtension(format);
        std::string path = pfd::save_file("Export Audio", "export" + ext,
                                          {ExportRenderer::FormatLabel(format), "*" + ext,
                                           "All Files", "*"}).result();
        if (!path.empty()) {
            // Append the extension if the user typed a bare name.
            std::string lowered = path;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowered.size() < ext.size() ||
                lowered.compare(lowered.size() - ext.size(), ext.size(), ext) != 0) {
                path += ext;
            }

            // Snapshot the live state (deep-copied tracks, cloned effects) so
            // the background render never races the audio thread or later UI edits.
            auto request = std::make_shared<ExportRenderer::Request>(
                ExportRenderer::BuildRequest(state, format, path, s_tailSeconds));
            g_exportJob.progress.store(0.0f);
            g_exportJob.done.store(false);
            g_exportJob.running.store(true);
            std::thread([request]() {
                g_exportJob.result = ExportRenderer::Render(*request, &g_exportJob.progress);
                g_exportJob.done.store(true);
            }).detach();
        }
    }
    ImGui::EndDisabled();
    if (nothingToExport && !running) {
        ImGui::TextDisabled("Nothing to export: the project has no tracks.");
    }

    if (running) {
        ImGui::ProgressBar(g_exportJob.progress.load(), ImVec2(-1.0f, 0.0f));
    } else if (!g_lastExportMessage.empty()) {
        ImGui::TextWrapped("%s", g_lastExportMessage.c_str());
    }

    ImGui::End();
}

// Reads the last `count` tap frames (padding the front with silence if the
// tap hasn't filled that far yet, e.g. right at startup) and mono-sums them
// into `outMono` (resized to `count`). Shared by every AudioTap consumer
// below (SPECTRUM, SCOPE, and M9's future WATERFALL) so the read/pad/sum
// steps aren't duplicated per panel. UI-Thread-only.
static void ReadMonoFromTap(const AudioTap& tap, size_t count, std::vector<float>& outMono) {
    static std::vector<AudioTapFrame> s_scratch;
    s_scratch.resize(count);
    outMono.resize(count);
    size_t got = tap.ReadLatest(s_scratch.data(), count);
    size_t pad = count - got;
    for (size_t i = 0; i < pad; ++i) outMono[i] = 0.0f;
    for (size_t i = 0; i < got; ++i) {
        outMono[pad + i] = (s_scratch[i].left + s_scratch[i].right) * 0.5f;
    }
}

// UI-Refactor Phase 3: live frequency-domain readout, fed by M0's shared
// AudioTap (mono-summed) through a Hann-windowed radix-2 FFT
// (SimpleFFT.h). UI-Thread-only, called once per rendered frame -- never
// anywhere near AudioEngine::process.
static void DrawSpectrumWindow(const AudioTap& tap) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 220.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SPECTRUM")) {
        ImGui::End();
        return;
    }

    constexpr size_t kFftSize = 2048; // power of two, required by SimpleFFT::Transform
    static std::vector<float> s_mono;
    static std::vector<float> s_magnitudes;
    static std::vector<float> s_smoothed;

    ReadMonoFromTap(tap, kFftSize, s_mono);
    SimpleFFT::ComputeLogMagnitudeSpectrum(s_mono.data(), kFftSize, s_magnitudes);
    if (s_smoothed.size() != s_magnitudes.size()) s_smoothed.assign(s_magnitudes.size(), 0.0f);
    // Fast attack / slow release across frames so the trace reads as a
    // smooth-but-jagged line rather than a jittery raw FFT (per the plan).
    constexpr float kAttack = 0.6f;
    constexpr float kRelease = 0.15f;
    for (size_t i = 0; i < s_magnitudes.size(); ++i) {
        float target = s_magnitudes[i];
        float rate = target > s_smoothed[i] ? kAttack : kRelease;
        s_smoothed[i] += (target - s_smoothed[i]) * rate;
    }

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.y = std::max(canvasSize.y, 20.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(0, 0, 0, 255));

    // Log-scaled frequency axis (skip bin 0/DC) so bass isn't crushed into a
    // handful of pixels the way a linear bin->x mapping would.
    size_t numBins = s_smoothed.size();
    if (numBins > 2) {
        std::vector<ImVec2> points;
        points.reserve(numBins - 1);
        float logDenom = std::log(static_cast<float>(numBins - 1));
        for (size_t i = 1; i < numBins; ++i) {
            float t = std::log(static_cast<float>(i)) / logDenom;
            float x = canvasPos.x + t * canvasSize.x;
            float y = canvasPos.y + canvasSize.y * (1.0f - s_smoothed[i]);
            points.push_back(ImVec2(x, y));
        }
        dl->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(255, 255, 255, 255), ImDrawFlags_None, 1.5f);
    }

    ImGui::Dummy(canvasSize);
    ImGui::End();
}

// live-PLAN Phase L4: RondoCode-style "the code is playing" feedback --
// raw oscilloscope waveform plus a lightweight bucketed-magnitude spectrum
// bar view, both fed by M0's shared AudioTap. Deliberately coarser than
// SPECTRUM's full log-magnitude line (a handful of bars, not ~1000 bins) --
// this panel is about "is it making sound and roughly what kind," not
// frequency analysis.
static void DrawScopeWindow(const AudioTap& tap) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SCOPE")) {
        ImGui::End();
        return;
    }

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 fullSize = ImGui::GetContentRegionAvail();
    fullSize.y = std::max(fullSize.y, 40.0f);
    float waveH = fullSize.y * 0.6f;
    float barsH = fullSize.y - waveH - 4.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // --- Oscilloscope: raw (mono-summed) waveform ---
    constexpr size_t kScopeSamples = 1024;
    static std::vector<float> s_wave;
    ReadMonoFromTap(tap, kScopeSamples, s_wave);

    ImVec2 waveSize(fullSize.x, waveH);
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + waveSize.x, canvasPos.y + waveSize.y), IM_COL32(0, 0, 0, 255));
    float midY = canvasPos.y + waveSize.y * 0.5f;
    dl->AddLine(ImVec2(canvasPos.x, midY), ImVec2(canvasPos.x + waveSize.x, midY), IM_COL32(60, 60, 60, 255));
    if (s_wave.size() > 1) {
        std::vector<ImVec2> points;
        points.reserve(s_wave.size());
        for (size_t i = 0; i < s_wave.size(); ++i) {
            float x = canvasPos.x + waveSize.x * (static_cast<float>(i) / static_cast<float>(s_wave.size() - 1));
            float y = midY - s_wave[i] * (waveSize.y * 0.45f);
            points.push_back(ImVec2(x, y));
        }
        dl->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(255, 255, 255, 255), ImDrawFlags_None, 1.2f);
    }

    // --- Lightweight bucketed-magnitude spectrum bars ---
    ImVec2 barsPos(canvasPos.x, canvasPos.y + waveH + 4.0f);
    ImVec2 barsSize(fullSize.x, std::max(barsH, 10.0f));
    dl->AddRectFilled(barsPos, ImVec2(barsPos.x + barsSize.x, barsPos.y + barsSize.y), IM_COL32(0, 0, 0, 255));

    constexpr size_t kFftSize = 1024;
    constexpr int kNumBars = 24;
    static std::vector<float> s_fftMono;
    static std::vector<float> s_mags;
    static std::vector<float> s_barSmoothed(kNumBars, 0.0f);

    ReadMonoFromTap(tap, kFftSize, s_fftMono);
    SimpleFFT::ComputeLogMagnitudeSpectrum(s_fftMono.data(), kFftSize, s_mags);

    size_t numBins = s_mags.size();
    if (numBins > static_cast<size_t>(kNumBars) * 2) {
        float logSpan = std::log(static_cast<float>(numBins - 1));
        for (int b = 0; b < kNumBars; ++b) {
            float t0 = static_cast<float>(b) / static_cast<float>(kNumBars);
            float t1 = static_cast<float>(b + 1) / static_cast<float>(kNumBars);
            size_t lo = 1 + static_cast<size_t>(std::exp(t0 * logSpan));
            size_t hi = 1 + static_cast<size_t>(std::exp(t1 * logSpan));
            lo = std::min(lo, numBins - 1);
            hi = std::min(std::max(hi, lo + 1), numBins);
            float peak = 0.0f;
            for (size_t i = lo; i < hi; ++i) peak = std::max(peak, s_mags[i]);
            float rate = peak > s_barSmoothed[b] ? 0.5f : 0.12f;
            s_barSmoothed[b] += (peak - s_barSmoothed[b]) * rate;
        }
    }

    float barGap = 2.0f;
    float barW = (barsSize.x - barGap * static_cast<float>(kNumBars - 1)) / static_cast<float>(kNumBars);
    for (int b = 0; b < kNumBars; ++b) {
        float h = barsSize.y * s_barSmoothed[b];
        float x0 = barsPos.x + static_cast<float>(b) * (barW + barGap);
        dl->AddRectFilled(ImVec2(x0, barsPos.y + barsSize.y - h), ImVec2(x0 + barW, barsPos.y + barsSize.y), IM_COL32(255, 255, 255, 220));
    }

    ImGui::Dummy(fullSize);
    ImGui::End();
}

// UI-Refactor Phase 4: transient particle "fountain" (see
// ParticleVisualizer.h/.cpp for the onset-detection + particle-pool
// simulation). This function just advances it by the frame's delta time
// and draws the result.
static void DrawVisualizerWindow(const AudioTap& tap) {
    ParticleVisualizer::Update(tap, ImGui::GetIO().DeltaTime);

    ImGui::SetNextWindowSize(ImVec2(320.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("VISUALIZER")) {
        ImGui::End();
        return;
    }
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.y = std::max(canvasSize.y, 20.0f);
    ParticleVisualizer::Draw(ImGui::GetWindowDrawList(), canvasPos, canvasSize);
    ImGui::Dummy(canvasSize);
    ImGui::End();
}

// UI-Refactor Phase 5: spectrogram waterfall -- a receding stack of
// log-magnitude traces (SpectrogramHistory.h) built on M6's FFT, reproducing
// the reference's layered-line look without any texture upload (pure
// AddPolyline calls, per the plan).
static void DrawWaterfallWindow(const AudioTap& tap) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("WATERFALL")) {
        ImGui::End();
        return;
    }

    constexpr size_t kFftSize = 1024;
    constexpr size_t kHistoryFrames = 30;
    constexpr int kPushEveryNFrames = 6; // ~10 pushes/sec at 60fps -> ~3s of history retained
    static SpectrogramHistory s_history(kHistoryFrames);
    static int s_throttle = 0;
    static std::vector<float> s_mono;
    static std::vector<float> s_mags;

    if (++s_throttle >= kPushEveryNFrames) {
        s_throttle = 0;
        ReadMonoFromTap(tap, kFftSize, s_mono);
        SimpleFFT::ComputeLogMagnitudeSpectrum(s_mono.data(), kFftSize, s_mags);
        s_history.Push(s_mags);
    }

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.y = std::max(canvasSize.y, 20.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(0, 0, 0, 255));

    size_t n = s_history.size();
    if (n >= 2) {
        std::vector<ImVec2> points;
        // Draw oldest-first so newer (brighter) traces paint over older ones.
        for (size_t drawAge = n - 1; ; --drawAge) {
            const std::vector<float>& bins = s_history.Get(drawAge);
            float ageFrac = static_cast<float>(drawAge) / static_cast<float>(n - 1); // 0 = newest, 1 = oldest
            float brightness = std::lerp(1.0f, 0.15f, ageFrac);
            float yOffsetPx = ageFrac * canvasSize.y * 0.35f; // older traces recede upward

            size_t numBins = bins.size();
            if (numBins > 2) {
                points.clear();
                points.reserve(numBins - 1);
                float logDenom = std::log(static_cast<float>(numBins - 1));
                for (size_t i = 1; i < numBins; ++i) {
                    float t = std::log(static_cast<float>(i)) / logDenom;
                    float x = canvasPos.x + t * canvasSize.x;
                    float y = canvasPos.y + canvasSize.y * (1.0f - bins[i]) - yOffsetPx;
                    points.push_back(ImVec2(x, y));
                }
                int gray = static_cast<int>(255.0f * brightness);
                ImU32 color = IM_COL32(gray, gray, 255, gray);
                dl->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_None, 1.0f);
            }

            if (drawAge == 0) break;
        }
    }

    ImGui::Dummy(canvasSize);
    ImGui::End();
}

void initializeTestPatch()
{
    draftPatch.name = "Additive Patch";
    draftPatch.sustainLevel = sustainLvl;

    // Setup default keyframes
    TimbreKeyframe defaultKeyframe;
    defaultKeyframe.midiNote = 60; // C4
    defaultKeyframe.harmonics.resize(16, 0.0f);
    defaultKeyframe.harmonics[0] = 1.0f;
    defaultKeyframe.harmonics[1] = 0.5f;
    defaultKeyframe.harmonics[2] = 0.25f;
    draftPatch.timbreKeyframes.push_back(defaultKeyframe);
}

// Call this whenever an envelope handle or duration changes
void updateDraftPatchEnvelopes()
{
    draftPatch.attackMs = attackMs;
    draftPatch.decayMs = decayMs;
    draftPatch.releaseMs = releaseMs;
    draftPatch.sustainLevel = sustainLvl;

    // Y values: 0.0 is top (value 1.0), 1.0 is bottom (value 0.0) in UI space.
    // We want the table values to represent amplitude (0.0 to 1.0).
    // Start by generating the normalized 1024-sample shapes
    auto attackShape = generateBezierTable(1024, 0.0f, 1.0f - attackPts.p1.y, 1.0f - attackPts.p2.y, 1.0f, attackPts.p1.x, attackPts.p2.x);
    auto decayShape = generateBezierTable(1024, 1.0f, 1.0f - decayPts.p1.y, 1.0f - decayPts.p2.y, sustainLvl, decayPts.p1.x, decayPts.p2.x);
    auto releaseShape = generateBezierTable(1024, sustainLvl, 1.0f - releasePts.p1.y, 1.0f - releasePts.p2.y, 0.0f, releasePts.p1.x, releasePts.p2.x);

    draftPatch.attackTable = scaleTableToTime(attackShape, attackMs, SAMPLE_RATE);
    draftPatch.decayTable = scaleTableToTime(decayShape, decayMs, SAMPLE_RATE);
    draftPatch.releaseTable = scaleTableToTime(releaseShape, releaseMs, SAMPLE_RATE);
}

// Polls g_loadedProjectPath for changes (Main Thread only) and, if it changed
// on disk, reparses it into a temporary SequencerState, diffs that against the
// live state, and dispatches only what actually changed (Strudel-style live
// coding: edit the .adx file in a text editor, save, hear it update without
// stopping playback). All tracks participate; only the single active/draft
// patch does (tracks share one synth patch — see Phase 4's plan for why).
void CheckForHotReload(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue)
{
    if (g_loadedProjectPath.empty()) return;

    std::error_code ec;
    auto writeTime = std::filesystem::last_write_time(g_loadedProjectPath, ec);
    if (ec || writeTime == g_lastKnownWriteTime) return;
    g_lastKnownWriteTime = writeTime;

    SequencerState tempState;
    std::string tempFirstPatchName;
    if (!AdxParser::LoadProject(g_loadedProjectPath, tempState, tempFirstPatchName))
    {
        std::cerr << "Hot-reload: failed to parse " << g_loadedProjectPath << ", keeping previous state\n";
        return; // e.g. mid-write; will retry once the file settles and mtime changes again
    }
    std::cerr << "Hot-reload: " << g_loadedProjectPath << " changed, applying updates\n";
    // Keep the LIVE CODE panel's buffer in step with whatever changed the
    // file on disk (an external editor, or this panel's own Ctrl+S) -- a
    // no-op if the panel has unsaved local edits of its own.
    g_liveCodeEditor.SyncFromDisk(g_loadedProjectPath);

    if (tempState.bpm.load() != state.bpm.load())
    {
        state.bpm.store(tempState.bpm.load());
        AudioEvent evt{};
        evt.type = AudioEventType::BpmChange;
        evt.data.bpmState.bpm = state.bpm.load();
        eventQueue.try_enqueue(evt);
    }
    if (tempState.masterVolume.load() != state.masterVolume.load())
    {
        state.masterVolume.store(tempState.masterVolume.load());
        AudioEvent evt{};
        evt.type = AudioEventType::MasterVolChange;
        evt.data.masterVol.volume = state.masterVolume.load();
        eventQueue.try_enqueue(evt);
    }
    if (tempState.tuning.load() != state.tuning.load())
    {
        state.tuning.store(tempState.tuning.load());
        AudioEvent evt{};
        evt.type = AudioEventType::GlobalTuningChange;
        evt.data.globalTuning.tuning = state.tuning.load();
        eventQueue.try_enqueue(evt);
    }
    if (!(tempState.masterFx == state.masterFx))
    {
        state.masterFx = tempState.masterFx;
        DispatchAllMasterFx(eventQueue, state.masterFx);
    }
    if (tempState.loopEnabled.load() != state.loopEnabled.load() ||
        tempState.loopStartBeat.load() != state.loopStartBeat.load() ||
        tempState.loopEndBeat.load() != state.loopEndBeat.load())
    {
        state.loopEnabled.store(tempState.loopEnabled.load());
        state.loopStartBeat.store(tempState.loopStartBeat.load());
        state.loopEndBeat.store(tempState.loopEndBeat.load());
        AudioEvent evt{};
        evt.type = AudioEventType::LoopChange;
        evt.data.loopState.enabled = state.loopEnabled.load();
        evt.data.loopState.startBeat = state.loopStartBeat.load();
        evt.data.loopState.endBeat = state.loopEndBeat.load();
        eventQueue.try_enqueue(evt);
    }

    // Patch registry: state.patches is the full per-track registry now
    // (Phase 1), so diff the whole map — not just the one patch the PATCH
    // EDITOR happens to have open. unordered_map::operator== compares keys
    // and Patch::operator== values.
    bool patchesChanged = !(tempState.patches == state.patches);

    // The PATCH EDITOR/PATCH SUITE UI still only edits one "draft" patch at a
    // time; keep it (and its ADSR scalars) synced to the file's first PATCH.
    if (!tempFirstPatchName.empty() && tempState.patches.count(tempFirstPatchName))
    {
        const Patch& newPatch = tempState.patches[tempFirstPatchName];
        if (newPatch.name != draftPatch.name || !(newPatch == draftPatch))
        {
            draftPatch = newPatch;
            attackMs = newPatch.attackMs;
            decayMs = newPatch.decayMs;
            releaseMs = newPatch.releaseMs;
            sustainLvl = newPatch.sustainLevel;
            updateDraftPatchEnvelopes();
        }
    }

    // All tracks now participate (Part A of Phase 4 made multi-track real) — Track's
    // operator== already composes into vector<Track>::operator==, so this is a
    // straightforward whole-list comparison, no more single-track special-casing.
    bool tracksChanged = !(tempState.tracks == state.tracks);

    // Mirror the file's full patches/tracks into state (same effect manual LOAD already
    // has by parsing directly into state) so subsequent SAVE and future diffs stay accurate.
    state.patches = tempState.patches;
    state.tracks = tempState.tracks;

    if (tracksChanged || patchesChanged)
    {
        DispatchSequenceUpdate(state, eventQueue);
    }
}

// Shared by the LOAD button and the command-line startup path: parses the
// .adx into the global state, syncs the draft patch/envelope UI, mirrors
// everything to the audio thread, and starts hot-reload watching the file.
static bool LoadProjectAndSync(const std::string& path, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue)
{
    std::string firstPatchName;
    if (!AdxParser::LoadProject(path, state, firstPatchName))
    {
        return false;
    }

    // Synchronize UI if patches were loaded
    if (!firstPatchName.empty() && state.patches.count(firstPatchName))
    {
        auto &firstPatch = state.patches[firstPatchName];
        draftPatch = firstPatch;

        // Update UI state from patch
        attackMs = firstPatch.attackMs;
        decayMs = firstPatch.decayMs;
        releaseMs = firstPatch.releaseMs;
        sustainLvl = firstPatch.sustainLevel;

        updateDraftPatchEnvelopes();
    }

    // Send global updates
    AudioEvent volEvt{};
    volEvt.type = AudioEventType::MasterVolChange;
    volEvt.data.masterVol.volume = state.masterVolume.load();
    eventQueue.try_enqueue(volEvt);

    AudioEvent tuningEvt{};
    tuningEvt.type = AudioEventType::GlobalTuningChange;
    tuningEvt.data.globalTuning.tuning = state.tuning.load();
    eventQueue.try_enqueue(tuningEvt);

    AudioEvent bpmEvt{};
    bpmEvt.type = AudioEventType::BpmChange;
    bpmEvt.data.bpmState.bpm = state.bpm.load();
    eventQueue.try_enqueue(bpmEvt);

    // Master FX (delay/reverb/sidechain/drive) from [GLOBAL]
    DispatchAllMasterFx(eventQueue, state.masterFx);

    AudioEvent loopEvt{};
    loopEvt.type = AudioEventType::LoopChange;
    loopEvt.data.loopState.enabled = state.loopEnabled.load();
    loopEvt.data.loopState.startBeat = state.loopStartBeat.load();
    loopEvt.data.loopState.endBeat = state.loopEndBeat.load();
    eventQueue.try_enqueue(loopEvt);

    // Send tracks + patches (if any) — DispatchSequenceUpdate no-ops on an
    // empty track list.
    DispatchSequenceUpdate(state, eventQueue);

    // Start watching this file for external changes (live-coding hot reload)
    g_loadedProjectPath = path;
    std::error_code ec;
    g_lastKnownWriteTime = std::filesystem::last_write_time(g_loadedProjectPath, ec);
    g_liveCodeEditor.SyncFromDisk(g_loadedProjectPath);
    return true;
}

// Default dock layout, built only when imgui.ini has no layout for the
// dockspace yet (first launch, or after deleting imgui.ini to reset). VS
// Code-style arrangement (left sidebar, right sidebar, center editor with
// the sequencer docked underneath) PLUS, per UI-Refactor Phase 6 / M10 (the
// consolidated layout-polish pass, now that all five new live-coding/
// visualizer panels exist), a bottom analysis row spanning the full window
// width: particle fountain | spectrum/waveform | waterfall, mirroring the
// reference screenshots' bottom strip. Every window can still be dragged
// out to float, re-snapped elsewhere, or stacked as tabs.
static void BuildDefaultDockLayout(ImGuiID dockspaceId, ImVec2 size)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, size);

    // Split the analysis row off the FULL dockspace first (before left/
    // center/right), so it spans the entire window width, not just the
    // center editor's -- "a bottom-docked row spanning the window," per
    // the plan.
    ImGuiID mainArea = dockspaceId;
    ImGuiID bottomRow = ImGui::DockBuilderSplitNode(mainArea, ImGuiDir_Down, 0.22f, nullptr, &mainArea);

    ImGuiID center = mainArea;
    ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.20f, nullptr, &center);
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,  0.52f, nullptr, &center);

    ImGui::DockBuilderDockWindow("SAMPLE BROWSER", left);
    ImGui::DockBuilderDockWindow("PATCH SUITE",    left);
    ImGui::DockBuilderDockWindow("MASTER FX",      right);
    ImGui::DockBuilderDockWindow("TRACK FX",       right);
    ImGui::DockBuilderDockWindow("EXPORT",         right);
    ImGui::DockBuilderDockWindow("PATCH EDITOR",   center);
    ImGui::DockBuilderDockWindow("SEQUENCER",      bottom);
    ImGui::DockBuilderDockWindow("LIVE CODE",      bottom);

    // Bottom analysis row, three equal-ish slots left to right: the
    // particle fountain (VISUALIZER) | a combined spectrum/waveform slot
    // (SCOPE tabbed with SPECTRUM -- UI-Refactor.md's Phase 6 wording
    // predates SCOPE existing as its own panel, and SCOPE's own content is
    // literally a waveform + a coarse spectrum, so this is the natural
    // reconciliation rather than inventing a fourth column) | WATERFALL.
    ImGuiID bottomRemaining = bottomRow;
    ImGuiID fountainSlot = ImGui::DockBuilderSplitNode(bottomRemaining, ImGuiDir_Left, 0.3333f, nullptr, &bottomRemaining);
    ImGuiID middleSlot   = ImGui::DockBuilderSplitNode(bottomRemaining, ImGuiDir_Left, 0.5f, nullptr, &bottomRemaining);
    ImGuiID waterfallSlot = bottomRemaining;
    ImGui::DockBuilderDockWindow("VISUALIZER", fountainSlot);
    ImGui::DockBuilderDockWindow("SCOPE",      middleSlot);
    ImGui::DockBuilderDockWindow("SPECTRUM",   middleSlot);
    ImGui::DockBuilderDockWindow("WATERFALL",  waterfallSlot);

    ImGui::DockBuilderFinish(dockspaceId);
}

// Headless export (no window, no audio device):
//   AudioSequencer.exe project.adx --export out.mp3 [--format wav16|wav24|wav32f|mp3|flac] [--tail seconds]
// Format defaults from the output extension (.mp3 / .flac / .wav -> 16-bit WAV).
static int RunHeadlessExport(const std::string& projectPath, const std::string& outputPath,
                             const std::string& formatName, float tailSeconds)
{
    ExportRenderer::Format format = ExportRenderer::Format::Wav16;
    if (!formatName.empty()) {
        if      (formatName == "wav16")  format = ExportRenderer::Format::Wav16;
        else if (formatName == "wav24")  format = ExportRenderer::Format::Wav24;
        else if (formatName == "wav32f") format = ExportRenderer::Format::WavFloat32;
        else if (formatName == "mp3")    format = ExportRenderer::Format::Mp3;
        else if (formatName == "flac")   format = ExportRenderer::Format::Flac;
        else {
            std::cerr << "Unknown --format '" << formatName << "' (expected wav16|wav24|wav32f|mp3|flac)\n";
            return 1;
        }
    } else {
        std::string lowered = outputPath;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if      (lowered.ends_with(".mp3"))  format = ExportRenderer::Format::Mp3;
        else if (lowered.ends_with(".flac")) format = ExportRenderer::Format::Flac;
    }

    std::string firstPatchName;
    if (!AdxParser::LoadProject(projectPath, state, firstPatchName)) {
        std::cerr << "Failed to load project: " << projectPath << "\n";
        return 1;
    }
    if (!firstPatchName.empty() && state.patches.count(firstPatchName)) {
        const Patch& firstPatch = state.patches[firstPatchName];
        draftPatch = firstPatch;
        attackMs = firstPatch.attackMs;
        decayMs = firstPatch.decayMs;
        releaseMs = firstPatch.releaseMs;
        sustainLvl = firstPatch.sustainLevel;
    }
    updateDraftPatchEnvelopes(); // .adx stores ADSR scalars, not tables — rebuild them

    auto request = ExportRenderer::BuildRequest(state, format, outputPath, tailSeconds);
    std::cout << "Rendering " << ExportRenderer::EstimateContentSeconds(state.tracks, state.bpm.load())
              << " s of content (+" << tailSeconds << " s tail) to " << outputPath << "...\n";
    std::atomic<float> progress{0.0f};
    ExportRenderer::Result result = ExportRenderer::Render(request, &progress);
    (result.success ? std::cout : std::cerr) << result.message << "\n";
    return result.success ? 0 : 1;
}

// --- Main Application ---
int main(int argc, char** argv)
{
    initializeTestPatch();

    // Headless export mode: parse flags before touching GLFW/RtAudio.
    {
        std::string projectPath, exportPath, formatName;
        float tailSeconds = 3.0f;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--export" && i + 1 < argc)      exportPath = argv[++i];
            else if (arg == "--format" && i + 1 < argc) formatName = argv[++i];
            else if (arg == "--tail" && i + 1 < argc)   tailSeconds = std::strtof(argv[++i], nullptr);
            else if (!arg.starts_with("--") && projectPath.empty()) projectPath = arg;
        }
        if (!exportPath.empty()) {
            if (projectPath.empty()) {
                std::cerr << "--export requires a project file: AudioSequencer.exe project.adx --export out.mp3\n";
                return 1;
            }
            return RunHeadlessExport(projectPath, exportPath, formatName, tailSeconds);
        }
    }

    // 1. Initialize GLFW and ImGui
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow *window = glfwCreateWindow(1280, 720, "Audio Sequencer & Synthesizer", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // VS Code-style modular UI: every panel is a dockable window the user can
    // tear off, re-snap, or stack as tabs. Layout persists via imgui.ini.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = false; // drag title bars to dock directly

    ImGui::StyleColorsDark();
    SetupImGuiStyle();

    // Default proportional font first (index 0, what every other widget
    // keeps using), then a monospace font on top for numeric readouts (BPM,
    // playhead) so tabular numerals don't jitter width as digits change —
    // matches the reference's tabular-numeral time display. Missing TTF
    // (e.g. a build that skipped the post-build copy step) just falls back
    // to the default font everywhere.
    ImGuiIO &fontIo = ImGui::GetIO();
    fontIo.Fonts->AddFontDefault();
    ImFont *monoFont = fontIo.Fonts->AddFontFromFileTTF(GetMonoFontPath().c_str(), 16.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // 2. Initialize RtAudio
    RtAudio dac;
    std::vector<unsigned int> deviceIds = dac.getDeviceIds();
    if (deviceIds.empty())
    {
        std::cerr << "No audio devices found!\n";
        // Continue anyway for the UI loop
    }

    RtAudio::StreamParameters parameters;
    parameters.deviceId = dac.getDefaultOutputDevice();
    parameters.nChannels = OUT_CHANNELS;
    parameters.firstChannel = 0;

    // Create the lock-free queue and AudioEngine
    moodycamel::ReaderWriterQueue<AudioEvent> eventQueue(1024);
    AudioEngine engine(eventQueue, SAMPLE_RATE, state.playheadPositionBeats);

    unsigned int bufferFrames = BUFFER_FRAMES;

    if (!deviceIds.empty())
    {
        if (dac.openStream(&parameters, nullptr, RTAUDIO_FLOAT32,
                           SAMPLE_RATE, &bufferFrames, &AudioEngine::audioCallback, &engine) != 0)
        {
            std::cerr << "RtAudio error: failed to open stream.\n";
        }
        else if (dac.startStream() != 0)
        {
            std::cerr << "RtAudio error: failed to start stream.\n";
        }
        else
        {
            std::cout << "Audio Stream Started: " << SAMPLE_RATE << "Hz, "
                      << bufferFrames << " frames.\n";
        }
    }

    // Initialize the initial patch's envelopes and register it. No dispatch
    // yet — no track references it until a project loads or the sequencer UI
    // creates its default track, either of which calls DispatchSequenceUpdate.
    updateDraftPatchEnvelopes();
    state.patches[draftPatch.name] = draftPatch;

    // Command-line project load: `AudioSequencer.exe path/to/project.adx`
    if (argc > 1)
    {
        if (LoadProjectAndSync(argv[1], eventQueue))
        {
            std::cout << "Loaded project from command line: " << argv[1] << "\n";
        }
        else
        {
            std::cerr << "Failed to load project from command line: " << argv[1] << "\n";
        }
    }

    // 3. Main Application Loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Live-coding hot reload: poll the loaded project file for external
        // changes at most every ~500ms (cheap enough to not gate on more, but
        // no need to stat() every single frame either).
        static auto s_lastPollTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - s_lastPollTime >= std::chrono::milliseconds(500))
        {
            s_lastPollTime = now;
            CheckForHotReload(eventQueue);
        }

        // Apply a finished melody-extraction job, if any. running/done are both
        // still true here only once the background thread has fully finished
        // (see the "Extract Melody..." button handler below).
        if (g_melodyJob.running.load() && g_melodyJob.done.load())
        {
            if (g_melodyJob.success)
            {
                Track newTrack;
                newTrack.patchName = g_melodyJob.patchName;
                newTrack.notes = std::move(g_melodyJob.resultNotes);
                state.tracks.push_back(newTrack);
                SelectTrack(static_cast<int>(state.tracks.size()) - 1);
                DispatchSequenceUpdate(state, eventQueue);
            }
            else
            {
                std::cerr << "Melody extraction failed (see prior errors).\n";
            }
            g_melodyJob.resultNotes.clear();
            g_melodyJob.running.store(false);
            g_melodyJob.done.store(false);
        }

        // Apply a finished MIDI-import job, if any (same handoff as g_melodyJob).
        if (g_midiJob.running.load() && g_midiJob.done.load())
        {
            if (g_midiJob.success)
            {
                std::string patchForTracks = g_midiJob.patchName;
                if (g_midiJob.hasAnalyzedPatch)
                {
                    // The analyzed patch becomes the draft patch (and a new
                    // entry in the registry the imported tracks below will
                    // reference by name), mirroring what loading a factory
                    // patch does — including syncing the envelope editor's
                    // scalar state.
                    draftPatch = g_midiJob.analyzedPatch;
                    attackMs = draftPatch.attackMs;
                    decayMs = draftPatch.decayMs;
                    releaseMs = draftPatch.releaseMs;
                    sustainLvl = draftPatch.sustainLevel;
                    state.patches[draftPatch.name] = draftPatch;
                    patchForTracks = draftPatch.name;
                }
                if (g_midiJob.suggestedBpm > 0.0f)
                {
                    state.bpm.store(g_midiJob.suggestedBpm);
                    AudioEvent evt{};
                    evt.type = AudioEventType::BpmChange;
                    evt.data.bpmState.bpm = g_midiJob.suggestedBpm;
                    eventQueue.try_enqueue(evt);
                }
                for (auto& imported : g_midiJob.resultTracks)
                {
                    Track t;
                    t.patchName = patchForTracks;
                    t.notes = std::move(imported.notes);
                    state.tracks.push_back(std::move(t));
                }
                if (!g_midiJob.resultTracks.empty())
                {
                    SelectTrack(static_cast<int>(state.tracks.size()) - 1);
                    DispatchSequenceUpdate(state, eventQueue);
                }
            }
            else
            {
                std::cerr << "MIDI import failed (see prior errors).\n";
            }
            g_midiJob.resultTracks.clear();
            g_midiJob.hasAnalyzedPatch = false;
            g_midiJob.suggestedBpm = 0.0f;
            g_midiJob.running.store(false);
            g_midiJob.done.store(false);
        }

        // Apply a finished export job, if any (same handoff as g_melodyJob).
        if (g_exportJob.running.load() && g_exportJob.done.load())
        {
            g_lastExportMessage = g_exportJob.result.message;
            if (!g_exportJob.result.success)
            {
                std::cerr << "Export failed: " << g_exportJob.result.message << "\n";
            }
            g_exportJob.running.store(false);
            g_exportJob.done.store(false);
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Fullscreen borderless HOST window: holds the transport bar and the
        // dockspace every other panel snaps into (it is itself not dockable).
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNavFocus |
                                       ImGuiWindowFlags_NoDocking;

        // UI rendering
        ImGui::Begin("MainCanvas", nullptr, windowFlags);

        // --- Top Bar: Transport Controls ---
        ImGui::BeginGroup();

        bool isPlaying = state.isPlaying.load();
        {
            // Compact square icon button (play triangle / stop square) in
            // place of the old "PLAY"/"STOP" text button — the reference
            // toolbar's icon-first transport cluster.
            const float kBtnSize = 32.0f;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1 = ImVec2(p0.x + kBtnSize, p0.y + kBtnSize);
            ImGui::InvisibleButton("##PlayStop", ImVec2(kBtnSize, kBtnSize));
            bool clicked = ImGui::IsItemClicked();
            bool hovered = ImGui::IsItemHovered();

            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, hovered ? IM_COL32(45, 45, 45, 255) : IM_COL32(20, 20, 20, 255), 2.0f);
            dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40), 2.0f);
            ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
            if (isPlaying)
            {
                float half = kBtnSize * 0.16f;
                dl->AddRectFilled(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), IM_COL32(255, 255, 255, 255));
            }
            else
            {
                float h = kBtnSize * 0.26f;
                float w = kBtnSize * 0.22f;
                dl->AddTriangleFilled(ImVec2(center.x - w * 0.5f, center.y - h), ImVec2(center.x - w * 0.5f, center.y + h), ImVec2(center.x + w, center.y), IM_COL32(255, 255, 255, 255));
            }

            if (clicked)
            {
                isPlaying = !isPlaying;
                state.isPlaying.store(isPlaying);

                AudioEvent evt{};
                evt.type = AudioEventType::PlayStateChange;
                evt.data.playState.isPlaying = isPlaying;
                eventQueue.try_enqueue(evt);
            }
            if (hovered) ImGui::SetTooltip(isPlaying ? "Stop" : "Play");
        }

        ImGui::SameLine();
        if (monoFont) ImGui::PushFont(monoFont);
        ImGui::Text("PLAYHEAD: %07.2f BEATS", state.playheadPositionBeats.load(std::memory_order_relaxed));
        if (monoFont) ImGui::PopFont();

        ImGui::SameLine();
        float currentBpm = state.bpm.load();
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderFloat("BPM", &currentBpm, 20.0f, 300.0f, "%.1f"))
        {
            state.bpm.store(currentBpm);
            AudioEvent evt{};
            evt.type = AudioEventType::BpmChange;
            evt.data.bpmState.bpm = currentBpm;
            eventQueue.try_enqueue(evt);
        }

        ImGui::SameLine();
        ImGui::Text(" | AUDIO ENGINE: %s", dac.isStreamRunning() ? "RUNNING" : "STOPPED");

        // Reserved width for InputText + LOAD + SAVE AS NEW + EXTRACT MELODY...
        // + IMPORT MIDI...; widened 400 -> 600 -> 740 as each button was added,
        // since anything past the budget gets clipped at the window edge.
        ImGui::SameLine(ImGui::GetWindowWidth() - 740);
        static char filepath[256] = "project.adx";
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("##File", filepath, IM_ARRAYSIZE(filepath));

        ImGui::SameLine();
        if (ImGui::Button("LOAD"))
        {
            auto sel = pfd::open_file("Open ADX Project", ".", {"ADX Files", "*.adx", "All Files", "*"}).result();
            if (!sel.empty())
            {
                LoadProjectAndSync(sel[0], eventQueue);
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("SAVE AS NEW"))
        {
            // Update the state's patch before saving
            state.patches[draftPatch.name] = draftPatch;
            if (AdxParser::SaveProject(filepath, state))
            {
                // Keep watching whatever we just wrote (live-coding hot reload)
                g_loadedProjectPath = filepath;
                std::error_code ec;
                g_lastKnownWriteTime = std::filesystem::last_write_time(g_loadedProjectPath, ec);
                g_liveCodeEditor.SyncFromDisk(g_loadedProjectPath);
            }
        }

        ImGui::SameLine();
        bool extractionRunning = g_melodyJob.running.load();
        ImGui::BeginDisabled(extractionRunning);
        if (ImGui::Button(extractionRunning ? "EXTRACTING..." : "EXTRACT MELODY..."))
        {
            auto sel = pfd::open_file("Extract Melody From Song", ".", {"Audio Files", "*.wav *.mp3", "All Files", "*"}).result();
            if (!sel.empty())
            {
                std::string path = sel[0];
                float bpm = state.bpm.load();
                g_melodyJob.patchName = draftPatch.name; // captured now, not at completion — see MelodyExtractionJob's comment
                g_melodyJob.running.store(true);
                g_melodyJob.done.store(false);
                std::thread([path, bpm]() {
                    auto result = g_melodyExtractor.ExtractMelody(path, bpm);
                    g_melodyJob.success = result.has_value();
                    if (result) g_melodyJob.resultNotes = std::move(*result);
                    g_melodyJob.done.store(true);
                }).detach();
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        bool midiImportRunning = g_midiJob.running.load();
        ImGui::BeginDisabled(midiImportRunning);
        if (ImGui::Button(midiImportRunning ? "IMPORTING..." : "IMPORT MIDI..."))
        {
            auto sel = pfd::open_file("Import MIDI File", ".", {"MIDI Files", "*.mid *.midi", "All Files", "*"}).result();
            if (!sel.empty())
            {
                std::string midiPath = sel[0];
                // Second picker: the reference mixdown of the same song, used
                // to clone the synth's timbre/envelope onto the imported
                // notes. Cancelling just skips analysis — the notes still load.
                auto audioSel = pfd::open_file("Reference Audio For Synth Analysis (Cancel To Skip)", ".",
                                               {"Audio Files", "*.wav *.mp3", "All Files", "*"}).result();
                std::string audioPath = audioSel.empty() ? std::string() : audioSel[0];
                float fallbackBpm = state.bpm.load();
                float tuning = state.tuning.load();
                g_midiJob.patchName = draftPatch.name; // captured now — see MidiImportJob's comment
                g_midiJob.running.store(true);
                g_midiJob.done.store(false);
                std::thread([midiPath, audioPath, fallbackBpm, tuning]() {
                    auto imported = MidiImporter::ImportFile(midiPath);
                    g_midiJob.success = imported.has_value();
                    if (imported)
                    {
                        g_midiJob.resultTracks = std::move(imported->tracks);
                        g_midiJob.suggestedBpm = imported->bpm;
                        if (!audioPath.empty())
                        {
                            // Note times in seconds must use the .mid's own
                            // tempo (that's what the mp3 was rendered at), not
                            // the project BPM — those only match after the
                            // suggestedBpm is applied on completion.
                            float bpmForSeconds = imported->bpm > 0.0f ? imported->bpm : fallbackBpm;
                            auto patch = TimbreAnalyzer::AnalyzePatch(audioPath, g_midiJob.resultTracks, bpmForSeconds, tuning);
                            if (patch)
                            {
                                g_midiJob.analyzedPatch = std::move(*patch);
                                g_midiJob.hasAnalyzedPatch = true;
                            }
                        }
                    }
                    g_midiJob.done.store(true);
                }).detach();
            }
        }
        ImGui::EndDisabled();

        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();

        // --- Dockspace: everything below the transport bar ---
        ImGuiID dockspaceId = ImGui::GetID("adXDockSpace");
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
        {
            BuildDefaultDockLayout(dockspaceId, ImGui::GetContentRegionAvail());
        }
        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f));
        ImGui::End(); // MainCanvas host

        // --- PATCH EDITOR: dockable window (envelopes + harmonics) ---
        ImGui::SetNextWindowSize(ImVec2(680.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("PATCH EDITOR");

        ImGui::Text("PATCH EDITOR: ENVELOPE (ADSR)");

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasSize(200.0f, 200.0f);
        bool envelopeChanged = false;

        // Helper Lambda for Bezier Grid Box
        auto drawBezierGrid = [&](const char *label, ADSRControlPoints &pts, ImU32 color,
                                  float startY, float endY, float *outMsVal)
        {
            ImGui::BeginGroup();
            ImGui::Text("%s", label);

            ImVec2 canvasPos = ImGui::GetCursorScreenPos();

            // Background & Grid
            drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(0, 0, 0, 255));
            int gridCount = 10;
            for (int i = 0; i <= gridCount; ++i)
            {
                float t = static_cast<float>(i) / gridCount;
                float x = canvasPos.x + t * canvasSize.x;
                float y = canvasPos.y + t * canvasSize.y;
                drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y), COLOR_GRID_LINES);
                drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y), COLOR_GRID_LINES);
            }

            // The invisible button
            ImGui::PushID(label);
            ImGui::InvisibleButton("##Canvas", canvasSize);

            // Interaction state
            ImGuiID dragPointId = ImGui::GetID("draggingPoint");
            int draggingPoint = ImGui::GetStateStorage()->GetInt(dragPointId, -1);

            ImVec2 mousePos = ImGui::GetIO().MousePos;
            ImVec2 p1Pos = ImVec2(canvasPos.x + pts.p1.x * canvasSize.x, canvasPos.y + pts.p1.y * canvasSize.y);
            ImVec2 p2Pos = ImVec2(canvasPos.x + pts.p2.x * canvasSize.x, canvasPos.y + pts.p2.y * canvasSize.y);

            float handleRadius = 6.0f;
            if (ImGui::IsItemActive())
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    // Check which point we clicked
                    if (std::hypot(mousePos.x - p1Pos.x, mousePos.y - p1Pos.y) < handleRadius * 2.0f)
                    {
                        draggingPoint = 1;
                    }
                    else if (std::hypot(mousePos.x - p2Pos.x, mousePos.y - p2Pos.y) < handleRadius * 2.0f)
                    {
                        draggingPoint = 2;
                    }
                    else
                    {
                        // If click empty space, optionally snap nearest. We'll skip for now.
                        draggingPoint = -1;
                    }
                    ImGui::GetStateStorage()->SetInt(dragPointId, draggingPoint);
                }

                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && draggingPoint != -1)
                {
                    ImVec2 delta = ImGui::GetIO().MouseDelta;
                    if (draggingPoint == 1)
                    {
                        pts.p1.x = std::clamp(pts.p1.x + delta.x / canvasSize.x, 0.0f, 1.0f);
                        pts.p1.y = std::clamp(pts.p1.y + delta.y / canvasSize.y, 0.0f, 1.0f);
                    }
                    else
                    {
                        pts.p2.x = std::clamp(pts.p2.x + delta.x / canvasSize.x, 0.0f, 1.0f);
                        pts.p2.y = std::clamp(pts.p2.y + delta.y / canvasSize.y, 0.0f, 1.0f);
                    }
                    envelopeChanged = true;
                }
            }
            else
            {
                ImGui::GetStateStorage()->SetInt(dragPointId, -1);
            }
            ImGui::PopID();

            // Recalculate positions based on updated normalized coordinates
            p1Pos = ImVec2(canvasPos.x + pts.p1.x * canvasSize.x, canvasPos.y + pts.p1.y * canvasSize.y);
            p2Pos = ImVec2(canvasPos.x + pts.p2.x * canvasSize.x, canvasPos.y + pts.p2.y * canvasSize.y);
            ImVec2 p0 = ImVec2(canvasPos.x, canvasPos.y + (1.0f - startY) * canvasSize.y);
            ImVec2 p3 = ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + (1.0f - endY) * canvasSize.y);

            // Draw Curve
            drawList->AddBezierCubic(p0, p1Pos, p2Pos, p3, color, 2.0f);

            // Draw Handles and connecting lines
            drawList->AddLine(p0, p1Pos, IM_COL32(100, 100, 100, 200), 1.0f);
            drawList->AddLine(p3, p2Pos, IM_COL32(100, 100, 100, 200), 1.0f);
            drawList->AddCircle(p1Pos, handleRadius, COLOR_TEXT, 0, 2.0f);
            drawList->AddCircle(p2Pos, handleRadius, COLOR_TEXT, 0, 2.0f);

            // Duration input
            ImGui::PushItemWidth(canvasSize.x);
            ImGui::PushID(label);
            if (ImGui::DragFloat("##Dur", outMsVal, 1.0f, 1.0f, 5000.0f, "%.0f ms"))
            {
                envelopeChanged = true;
            }
            ImGui::PopID();
            ImGui::PopItemWidth();

            ImGui::EndGroup();
            return canvasPos;
        };

        ImGui::BeginGroup();
        // Attack Box (Y starts at 0.0, ends at 1.0)
        drawBezierGrid("ATTACK", attackPts, COLOR_ATTACK, 0.0f, 1.0f, &attackMs);
        ImGui::SameLine();

        // Decay Box (Y starts at 1.0, ends at sustainLvl)
        ImVec2 decayPos = drawBezierGrid("DECAY", decayPts, COLOR_DECAY, 1.0f, sustainLvl, &decayMs);

        // Render Sustain Handle on the right edge of Decay Box
        ImVec2 sustainHandlePos(decayPos.x + canvasSize.x, decayPos.y + (1.0f - sustainLvl) * canvasSize.y);
        drawList->AddCircleFilled(sustainHandlePos, 5.0f, COLOR_TEXT);

        // Simple Interaction for Sustain Handle
        ImGui::SetCursorScreenPos(ImVec2(sustainHandlePos.x - 10.0f, decayPos.y));
        ImGui::PushID("Sustain");
        ImGui::InvisibleButton("##SustainHandle", ImVec2(20.0f, canvasSize.y));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            sustainLvl -= ImGui::GetIO().MouseDelta.y / canvasSize.y;
            sustainLvl = std::clamp(sustainLvl, 0.0f, 1.0f);
            envelopeChanged = true;
        }
        ImGui::PopID();

        ImGui::SameLine();

        // Release Box (Y starts at sustainLvl, ends at 0.0)
        drawBezierGrid("RELEASE", releasePts, COLOR_RELEASE, sustainLvl, 0.0f, &releaseMs);
        ImGui::EndGroup();

        // Combined ADSR View
        ImGui::Spacing();
        ImGui::Text("COMBINED ENVELOPE PREVIEW");
        ImVec2 combinedPos = ImGui::GetCursorScreenPos();
        ImVec2 combinedSize(canvasSize.x * 3.0f + 20.0f, 100.0f); // Roughly match width
        drawList->AddRectFilled(combinedPos, ImVec2(combinedPos.x + combinedSize.x, combinedPos.y + combinedSize.y), IM_COL32(20, 20, 20, 255));

        // Draw standard preview using the generated tables
        if (draftPatch.attackTable.size() > 0 && draftPatch.decayTable.size() > 0 && draftPatch.releaseTable.size() > 0)
        {
            float totalMs = attackMs + decayMs + 1000.0f /* fake sustain width */ + releaseMs;
            float currentX = combinedPos.x;

            auto drawTablePreview = [&](const std::vector<float> &table, float widthMs, ImU32 col)
            {
                float widthPx = (widthMs / totalMs) * combinedSize.x;
                if (table.empty() || widthPx < 1.0f)
                    return;

                ImVec2 prevP(currentX, combinedPos.y + (1.0f - table[0]) * combinedSize.y);
                for (size_t i = 1; i < table.size(); i += std::max<size_t>(1, table.size() / 100))
                { // Downsample for drawing
                    float t = static_cast<float>(i) / static_cast<float>(table.size() - 1);
                    ImVec2 p(currentX + t * widthPx, combinedPos.y + (1.0f - table[i]) * combinedSize.y);
                    drawList->AddLine(prevP, p, col, 1.5f);
                    prevP = p;
                }
                currentX += widthPx;
            };

            drawTablePreview(draftPatch.attackTable, attackMs, COLOR_ATTACK);
            drawTablePreview(draftPatch.decayTable, decayMs, COLOR_DECAY);

            // Draw Sustain segment
            float sustainWidthPx = (1000.0f / totalMs) * combinedSize.x;
            ImVec2 susStart(currentX, combinedPos.y + (1.0f - sustainLvl) * combinedSize.y);
            ImVec2 susEnd(currentX + sustainWidthPx, combinedPos.y + (1.0f - sustainLvl) * combinedSize.y);
            drawList->AddLine(susStart, susEnd, IM_COL32(100, 100, 100, 255), 1.5f);
            currentX += sustainWidthPx;

            drawTablePreview(draftPatch.releaseTable, releaseMs, COLOR_RELEASE);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("PATCH EDITOR: HARMONICS & TIMBRE");

        bool timbreChanged = false;
        ImVec2 harmonicsCanvasSize(620.0f, 200.0f); // Wide enough for 16 bars
        ImVec2 harmonicsCanvasPos = ImGui::GetCursorScreenPos();

        // Background
        drawList->AddRectFilled(harmonicsCanvasPos, ImVec2(harmonicsCanvasPos.x + harmonicsCanvasSize.x, harmonicsCanvasPos.y + harmonicsCanvasSize.y), IM_COL32(0, 0, 0, 255));

        // Waveform Preview (Above bars)
        ImVec2 waveformPos = harmonicsCanvasPos;
        ImVec2 waveformSize(harmonicsCanvasSize.x, 60.0f);
        drawList->AddRectFilled(waveformPos, ImVec2(waveformPos.x + waveformSize.x, waveformPos.y + waveformSize.y), IM_COL32(15, 15, 20, 255));

        // Summation of 16 sines
        if (!draftPatch.timbreKeyframes.empty())
        {
            auto &harmonics = draftPatch.timbreKeyframes[0].harmonics;
            int waveSamples = 200;
            ImVec2 prevP;
            for (int i = 0; i < waveSamples; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(waveSamples - 1);
                float phase = t * 2.0f * static_cast<float>(M_PI);

                float sum = 0.0f;
                for (size_t h = 0; h < 16 && h < harmonics.size(); ++h)
                {
                    sum += std::sin(phase * static_cast<float>(h + 1)) * harmonics[h];
                }

                // Scale and offset (assuming max amplitude roughly 2.0-3.0 for typical shapes)
                float normalizedSum = std::clamp(sum * 0.3f, -1.0f, 1.0f);
                float y = waveformPos.y + waveformSize.y * 0.5f - (normalizedSum * waveformSize.y * 0.45f);
                float x = waveformPos.x + t * waveformSize.x;

                ImVec2 p(x, y);
                if (i > 0)
                {
                    drawList->AddLine(prevP, p, COLOR_TEXT, 1.5f);
                }
                prevP = p;
            }
        }

        // Grid lines for bars
        ImVec2 barsPos = ImVec2(harmonicsCanvasPos.x, harmonicsCanvasPos.y + waveformSize.y + 10.0f);
        ImVec2 barsSize = ImVec2(harmonicsCanvasSize.x, harmonicsCanvasSize.y - waveformSize.y - 10.0f);

        int hLines = 4;
        for (int i = 0; i <= hLines; ++i)
        {
            float y = barsPos.y + (static_cast<float>(i) / hLines) * barsSize.y;
            drawList->AddLine(ImVec2(barsPos.x, y), ImVec2(barsPos.x + barsSize.x, y), COLOR_GRID_LINES);
        }

        // Invisible button for interaction
        ImGui::SetCursorScreenPos(barsPos);
        ImGui::InvisibleButton("##HarmonicsCanvas", barsSize);

        float barWidth = barsSize.x / 16.0f;
        float spacing = 4.0f;

        if (!draftPatch.timbreKeyframes.empty())
        {
            auto &harmonics = draftPatch.timbreKeyframes[0].harmonics;
            if (harmonics.size() < 16)
                harmonics.resize(16, 0.0f);

            // Handle Interaction
            if (ImGui::IsItemActive() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
            {
                ImVec2 mousePos = ImGui::GetIO().MousePos;

                // Determine which bar we are in
                int barIndex = static_cast<int>((mousePos.x - barsPos.x) / barWidth);
                if (barIndex >= 0 && barIndex < 16)
                {
                    // Map Y to amplitude 0.0 to 1.0
                    float newAmp = 1.0f - ((mousePos.y - barsPos.y) / barsSize.y);
                    newAmp = std::clamp(newAmp, 0.0f, 1.0f);

                    if (std::abs(harmonics[barIndex] - newAmp) > 0.001f)
                    {
                        harmonics[barIndex] = newAmp;
                        timbreChanged = true;
                    }
                }
            }

            // Draw Bars
            for (int i = 0; i < 16; ++i)
            {
                float x = barsPos.x + static_cast<float>(i) * barWidth + spacing * 0.5f;
                float w = barWidth - spacing;
                float h = harmonics[i] * barsSize.y;
                float y = barsPos.y + barsSize.y - h;

                ImU32 barCol = IM_COL32(200, 200, 200, 255); // Default color
                // Emphasize odd/even or fundamental for visual flavor
                if (i == 0)
                    barCol = COLOR_ATTACK;
                else if (i % 2 != 0)
                    barCol = COLOR_DECAY;
                else
                    barCol = COLOR_RELEASE;

                drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + w, barsPos.y + barsSize.y), barCol);

                // Draw Harmonic Index
                char labelBuf[4];
                snprintf(labelBuf, sizeof(labelBuf), "%d", i + 1);
                drawList->AddText(ImVec2(x + w * 0.5f - ImGui::CalcTextSize(labelBuf).x * 0.5f, barsPos.y + barsSize.y + 2.0f), COLOR_TEXT, labelBuf);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(barsPos.x, barsPos.y + barsSize.y + 25.0f));

        // UI control to apply to all keyframes
        static bool applyToAllKeyframes = true;
        ImGui::Checkbox("APPLY HARMONICS TO ALL KEYFRAMES", &applyToAllKeyframes);
        if (timbreChanged && applyToAllKeyframes && draftPatch.timbreKeyframes.size() > 1)
        {
            auto &sourceHarmonics = draftPatch.timbreKeyframes[0].harmonics;
            for (size_t i = 1; i < draftPatch.timbreKeyframes.size(); ++i)
            {
                draftPatch.timbreKeyframes[i].harmonics = sourceHarmonics;
            }
        }

        // Push update to Audio Thread if envelope or timbre changed
        if (envelopeChanged || timbreChanged)
        {
            if (envelopeChanged)
            {
                updateDraftPatchEnvelopes();
            }

            state.patches[draftPatch.name] = draftPatch;
            DispatchSequenceUpdate(state, eventQueue);
        }

        ImGui::End(); // PATCH EDITOR

        // --- SEQUENCER: dockable window ---
        ImGui::SetNextWindowSize(ImVec2(900.0f, 380.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("SEQUENCER");
        DrawSequencerUI(state, eventQueue, engine);
        ImGui::End();

        // --- LIVE CODE: dockable window (live-PLAN L3) ---
        g_liveCodeEditor.Draw(state, eventQueue);

        // --- Dockable side panels: browser, patch suite, FX, export ---
        DrawSampleBrowser(state, eventQueue);
        DrawPatchSuiteWindow(eventQueue);
        DrawMasterFxWindow(eventQueue);
        DrawExportWindow();
        DrawSpectrumWindow(engine.GetMasterTap());
        DrawScopeWindow(engine.GetMasterTap());
        DrawVisualizerWindow(engine.GetMasterTap());
        DrawWaterfallWindow(engine.GetMasterTap());

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        // Safe Garbage Collection on the Main Thread
        // The Audio Thread has pushed old pointers to this bin to safely surrender ownership.
        if (!engine.m_sequenceGarbageBin.empty())
        {
            // Note: In a stricter lock-free architecture we would pull from a moodycamel concurrent queue.
            // But since the project currently uses a std::vector populated by the audio thread,
            // the main thread can carefully clear it when it knows it's safe (e.g. between frames).
            // This is a minimal stopgap. A proper implementation would use `try_dequeue` on a reverse queue.
            engine.m_sequenceGarbageBin.clear();
        }
    }

    // 4. Cleanup
    if (dac.isStreamOpen())
    {
        dac.closeStream();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

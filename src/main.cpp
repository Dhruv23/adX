#include "AudioData.h"
#include "AudioEngine.h"
#include "SequencerUI.h"
#include "SampleBrowser.h"
#include "AdxParser.h"
#include "MelodyExtractor.h"
#include <portable-file-dialogs.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <filesystem>
#include <system_error>

#include <RtAudio.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
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

void SetupImGuiStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();

    // Pure black backgrounds
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    // Text color
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    // Remove all borders and rounding
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
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

static void DispatchPatchUpdate(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, const Patch& patch) {
    AudioEvent evt{};
    evt.type = AudioEventType::PatchUpdate;
    evt.data.patch = new Patch(patch);
    if (!eventQueue.try_enqueue(evt)) delete evt.data.patch;
}

static void DispatchTrackUpdate(moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, const std::vector<Track>& tracks) {
    AudioEvent evt{};
    evt.type = AudioEventType::SequenceUpdate;
    evt.data.tracks = new std::vector<Track>(tracks);
    if (!eventQueue.try_enqueue(evt)) delete evt.data.tracks;
}

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
                DispatchPatchUpdate(eventQueue, draftPatch);
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
        DispatchPatchUpdate(eventQueue, draftPatch);
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

    // Patch: only the one actually wired into the UI/engine (matches the single-draft-patch model)
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
            DispatchPatchUpdate(eventQueue, draftPatch);
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

    if (tracksChanged)
    {
        DispatchTrackUpdate(eventQueue, state.tracks);
    }
}

// --- Main Application ---
int main()
{
    initializeTestPatch();

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

    ImGui::StyleColorsDark();
    SetupImGuiStyle();

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

    // Initialize initial patch envelopes and submit
    updateDraftPatchEnvelopes();
    {
        AudioEvent patchUpdateEvt{};
        patchUpdateEvt.type = AudioEventType::PatchUpdate;
        patchUpdateEvt.data.patch = new Patch(draftPatch); // Audio thread will own it
        if (!eventQueue.try_enqueue(patchUpdateEvt))
        {
            delete patchUpdateEvt.data.patch;
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
                DispatchTrackUpdate(eventQueue, state.tracks);
            }
            else
            {
                std::cerr << "Melody extraction failed (see prior errors).\n";
            }
            g_melodyJob.resultNotes.clear();
            g_melodyJob.running.store(false);
            g_melodyJob.done.store(false);
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Make the main window fullscreen and borderless
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoCollapse;

        // UI rendering
        ImGui::Begin("MainCanvas", nullptr, windowFlags);

        // --- Top Bar: Transport Controls ---
        ImGui::BeginGroup();

        bool isPlaying = state.isPlaying.load();
        if (ImGui::Button(isPlaying ? "STOP" : "PLAY", ImVec2(100, 30)))
        {
            isPlaying = !isPlaying;
            state.isPlaying.store(isPlaying);

            AudioEvent evt{};
            evt.type = AudioEventType::PlayStateChange;
            evt.data.playState.isPlaying = isPlaying;
            eventQueue.try_enqueue(evt);
        }

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
        ImGui::SameLine();
        ImGui::Text(" | AUDIO ENGINE: %s | PLAYHEAD: %.2f BEATS",
                    dac.isStreamRunning() ? "RUNNING" : "STOPPED",
                    state.playheadPositionBeats.load(std::memory_order_relaxed));

        // Reserved width for InputText + LOAD + SAVE AS NEW + EXTRACT MELODY...;
        // widened from 400 when the EXTRACT MELODY... button was added, which
        // no longer fit in the original budget and was clipped at the window edge.
        ImGui::SameLine(ImGui::GetWindowWidth() - 600);
        static char filepath[256] = "project.adx";
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("##File", filepath, IM_ARRAYSIZE(filepath));

        ImGui::SameLine();
        if (ImGui::Button("LOAD"))
        {
            auto sel = pfd::open_file("Open ADX Project", ".", {"ADX Files", "*.adx", "All Files", "*"}).result();
            if (!sel.empty())
            {
                std::string firstPatchName;
                if (AdxParser::LoadProject(sel[0], state, firstPatchName))
                {
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

                        // Send patch update
                        DispatchPatchUpdate(eventQueue, draftPatch);
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

                    // Send track update if available
                    if (!state.tracks.empty())
                    {
                        DispatchTrackUpdate(eventQueue, state.tracks);
                    }

                    // Start watching this file for external changes (live-coding hot reload)
                    g_loadedProjectPath = sel[0];
                    std::error_code ec;
                    g_lastKnownWriteTime = std::filesystem::last_write_time(g_loadedProjectPath, ec);
                }
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

        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Split screen: Patch Editor (Top) & Sequencer (Bottom) ---
        float windowHeight = ImGui::GetContentRegionAvail().y;
        float patchEditorHeight = windowHeight * 0.4f;

        ImGui::BeginChild("PatchEditor", ImVec2(0, patchEditorHeight), false, ImGuiWindowFlags_NoScrollbar);

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

            AudioEvent patchUpdateEvt{};
            patchUpdateEvt.type = AudioEventType::PatchUpdate;
            patchUpdateEvt.data.patch = new Patch(draftPatch);

            if (!eventQueue.try_enqueue(patchUpdateEvt))
            {
                std::cerr << "Event Queue Full while pushing patch update!\n";
                delete patchUpdateEvt.data.patch; // Prevent memory leak
            }
        }

        ImGui::EndChild();

        ImGui::Separator();

        // --- Bottom Half: Sequencer ---
        DrawSequencerUI(state, eventQueue);

        ImGui::End();

        // --- Sample Browser (Phase 5): separate floating window ---
        DrawSampleBrowser(state, eventQueue);

        // --- Genre suites: patch library/extras + master-bus FX windows ---
        DrawPatchSuiteWindow(eventQueue);
        DrawMasterFxWindow(eventQueue);

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
        // The Audio Thread has pushed old pointers to these bins to safely surrender ownership.
        if (!engine.m_patchGarbageBin.empty() || !engine.m_sequenceGarbageBin.empty())
        {
            // Note: In a stricter lock-free architecture we would pull from a moodycamel concurrent queue.
            // But since the project currently uses a std::vector populated by the audio thread,
            // the main thread can carefully clear it when it knows it's safe (e.g. between frames).
            // This is a minimal stopgap. A proper implementation would use `try_dequeue` on a reverse queue.
            engine.m_patchGarbageBin.clear();
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

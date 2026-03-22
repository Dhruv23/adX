#include "AudioData.h"

#include "AudioEngine.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>

#include <RtAudio.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <readerwriterqueue.h>
#include <cmath>
#include <algorithm>

// --- Custom Styling Constants ---
const ImU32 COLOR_ATTACK     = IM_COL32(255, 60, 60, 255);
const ImU32 COLOR_DECAY      = IM_COL32(255, 200, 50, 255);
const ImU32 COLOR_RELEASE    = IM_COL32(50, 200, 255, 255);
const ImU32 COLOR_GRID_LINES = IM_COL32(26, 26, 36, 255);
const ImU32 COLOR_TEXT       = IM_COL32(255, 255, 255, 255);

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Pure black backgrounds
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ChildBg]  = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBg]  = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    // Text color
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    // Remove all borders and rounding
    style.WindowRounding = 0.0f;
    style.ChildRounding  = 0.0f;
    style.FrameRounding  = 0.0f;
    style.PopupRounding  = 0.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize  = 0.0f;
    style.FrameBorderSize  = 0.0f;
    style.PopupBorderSize  = 0.0f;
}

// --- Globals ---
constexpr unsigned int SAMPLE_RATE = 44100;
constexpr unsigned int BUFFER_FRAMES = 512;
constexpr unsigned int OUT_CHANNELS = 2;

// The global SequencerState managed by the Main Thread
SequencerState state;

// Helper to generate a basic lookup table for testing
std::vector<float> generateLinearTable(float start, float end, float timeSeconds, unsigned int sampleRate) {
    size_t samples = static_cast<size_t>(timeSeconds * sampleRate);
    std::vector<float> table(samples);
    for (size_t i = 0; i < samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(samples);
        table[i] = std::lerp(start, end, t);
    }
    return table;
}

// Global default patch for testing
Patch globalTestPatch;

void initializeTestPatch() {
    globalTestPatch.name = "Test Additive Patch";
    globalTestPatch.sustainLevel = 0.7f;

    // Generate dummy ADSR tables
    globalTestPatch.attackTable = generateLinearTable(0.0f, 1.0f, 0.1f, SAMPLE_RATE);
    globalTestPatch.decayTable = generateLinearTable(1.0f, 0.7f, 0.1f, SAMPLE_RATE);
    globalTestPatch.releaseTable = generateLinearTable(1.0f, 0.0f, 0.5f, SAMPLE_RATE); // Normalized, assume scale by current level at NoteOff

    // Generate Timbre Keyframes
    TimbreKeyframe lowKeyframe;
    lowKeyframe.midiNote = 36; // C2
    lowKeyframe.harmonics.resize(16, 0.0f);
    // Square-ish wave for low notes (odd harmonics, 1/n amplitude)
    for (size_t i = 0; i < 16; i += 2) {
        lowKeyframe.harmonics[i] = 1.0f / static_cast<float>(i + 1);
    }

    TimbreKeyframe highKeyframe;
    highKeyframe.midiNote = 84; // C6
    highKeyframe.harmonics.resize(16, 0.0f);
    // Sine-ish wave for high notes (mostly fundamental)
    highKeyframe.harmonics[0] = 1.0f;
    highKeyframe.harmonics[1] = 0.2f;

    globalTestPatch.timbreKeyframes.push_back(lowKeyframe);
    globalTestPatch.timbreKeyframes.push_back(highKeyframe);
}

// --- Main Application ---
int main() {
    initializeTestPatch();

    // 1. Initialize GLFW and ImGui
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Audio Sequencer & Synthesizer", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    SetupImGuiStyle();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // 2. Initialize RtAudio
    RtAudio dac;
    std::vector<unsigned int> deviceIds = dac.getDeviceIds();
    if (deviceIds.empty()) {
        std::cerr << "No audio devices found!\n";
        // Continue anyway for the UI loop
    }

    RtAudio::StreamParameters parameters;
    parameters.deviceId = dac.getDefaultOutputDevice();
    parameters.nChannels = OUT_CHANNELS;
    parameters.firstChannel = 0;

    // Create the lock-free queue and AudioEngine
    moodycamel::ReaderWriterQueue<AudioEvent> eventQueue(1024);
    AudioEngine engine(eventQueue, SAMPLE_RATE);

    unsigned int bufferFrames = BUFFER_FRAMES;

    if (!deviceIds.empty()) {
        if (dac.openStream(&parameters, nullptr, RTAUDIO_FLOAT32,
                       SAMPLE_RATE, &bufferFrames, &AudioEngine::audioCallback, &engine) != 0) {
            std::cerr << "RtAudio error: failed to open stream.\n";
        } else if (dac.startStream() != 0) {
            std::cerr << "RtAudio error: failed to start stream.\n";
        } else {
            std::cout << "Audio Stream Started: " << SAMPLE_RATE << "Hz, "
                      << bufferFrames << " frames.\n";
        }
    }

    // 3. Main Application Loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Make the main window fullscreen and borderless
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoCollapse;

        // UI rendering
        ImGui::Begin("MainCanvas", nullptr, windowFlags);
        ImGui::Text("AUDIO ENGINE STATUS: %s", dac.isStreamRunning() ? "RUNNING" : "STOPPED");
        ImGui::Text("PLAYHEAD: %.2f BEATS", state.playheadPositionBeats.load());

        // Example: Sending a test Note On event from Main Thread to Audio Thread
        if (ImGui::Button("TRIGGER TEST NOTE ON (MIDDLE C)")) {
            AudioEvent evt{};
            evt.type = AudioEventType::NoteOn;
            evt.pitch = 60; // Middle C
            evt.velocity = 100;

            // Assign the read-only patch pointer
            evt.data.patch = &globalTestPatch;

            // Push lock-free
            if (!eventQueue.try_enqueue(evt)) {
                std::cerr << "Event Queue Full!\n";
            }
        }

        if (ImGui::Button("TRIGGER TEST NOTE ON (LOW C)")) {
            AudioEvent evt{};
            evt.type = AudioEventType::NoteOn;
            evt.pitch = 36; // C2
            evt.velocity = 100;
            evt.data.patch = &globalTestPatch;

            if (!eventQueue.try_enqueue(evt)) {
                std::cerr << "Event Queue Full!\n";
            }
        }

        if (ImGui::Button("TRIGGER TEST NOTE ON (HIGH C)")) {
            AudioEvent evt{};
            evt.type = AudioEventType::NoteOn;
            evt.pitch = 84; // C6
            evt.velocity = 100;
            evt.data.patch = &globalTestPatch;

            if (!eventQueue.try_enqueue(evt)) {
                std::cerr << "Event Queue Full!\n";
            }
        }

        if (ImGui::Button("TRIGGER TEST NOTE OFF (MIDDLE C)")) {
            AudioEvent evt{};
            evt.type = AudioEventType::NoteOff;
            evt.pitch = 60; // Middle C
            evt.velocity = 100;

            // Push lock-free
            if (!eventQueue.try_enqueue(evt)) {
                std::cerr << "Event Queue Full!\n";
            }
        }

        if (ImGui::Button("TRIGGER TEST NOTE OFF (LOW C)")) {
            AudioEvent evt{};
            evt.type = AudioEventType::NoteOff;
            evt.pitch = 36;
            evt.velocity = 100;
            if (!eventQueue.try_enqueue(evt)) {
                std::cerr << "Event Queue Full!\n";
            }
        }

        if (ImGui::Button("TRIGGER TEST NOTE OFF (HIGH C)")) {
            AudioEvent evt{};
            evt.type = AudioEventType::NoteOff;
            evt.pitch = 84;
            evt.velocity = 100;
            if (!eventQueue.try_enqueue(evt)) {
                std::cerr << "Event Queue Full!\n";
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("CUSTOM BEZIER TEST WIDGET");

        // --- Custom Bezier Test Widget ---
        static ImVec2 controlPoint(0.5f, 0.5f); // Normalized coordinates [0, 1]

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize(300.0f, 300.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background / Grid
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(0, 0, 0, 255));

        // Grid lines (10x10)
        int gridCount = 10;
        for (int i = 0; i <= gridCount; ++i) {
            float t = static_cast<float>(i) / gridCount;
            float x = canvasPos.x + t * canvasSize.x;
            float y = canvasPos.y + t * canvasSize.y;
            drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y), COLOR_GRID_LINES);
            drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y), COLOR_GRID_LINES);
        }

        // The InvisibleButton area
        ImGui::InvisibleButton("##BezierTestCanvas", canvasSize);

        // Interaction
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            controlPoint.x += delta.x / canvasSize.x;
            controlPoint.y += delta.y / canvasSize.y; // Y goes down in screen space

            // Clamp to [0, 1]
            controlPoint.x = std::clamp(controlPoint.x, 0.0f, 1.0f);
            controlPoint.y = std::clamp(controlPoint.y, 0.0f, 1.0f);
        }

        // Calculate actual pixel positions for Bezier
        ImVec2 p0 = ImVec2(canvasPos.x, canvasPos.y + canvasSize.y); // Bottom Left (start)
        ImVec2 p3 = ImVec2(canvasPos.x + canvasSize.x, canvasPos.y); // Top Right (end)

        // Control points
        // p1 and p2 use the interactive control point. We'll map the single interactive handle
        // to define tension/shape. Let's make p1 purely horizontal from start, and p2 the handle.
        ImVec2 p1 = ImVec2(canvasPos.x + controlPoint.x * canvasSize.x, canvasPos.y + canvasSize.y);
        ImVec2 p2 = ImVec2(canvasPos.x + controlPoint.x * canvasSize.x, canvasPos.y + controlPoint.y * canvasSize.y);

        // Draw Bezier
        drawList->AddBezierCubic(p0, p1, p2, p3, COLOR_ATTACK, 2.0f);

        // Draw Control Handle
        drawList->AddCircle(p2, 6.0f, COLOR_TEXT, 0, 2.0f);

        ImGui::End();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // 4. Cleanup
    if (dac.isStreamOpen()) {
        dac.closeStream();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

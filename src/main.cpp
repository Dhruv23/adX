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

// --- Globals ---
constexpr unsigned int SAMPLE_RATE = 44100;
constexpr unsigned int BUFFER_FRAMES = 512;
constexpr unsigned int OUT_CHANNELS = 2;

// The global SequencerState managed by the Main Thread
SequencerState state;

// --- Main Application ---
int main() {
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

        // UI rendering
        ImGui::Begin("Sequencer Controls");
        ImGui::Text("Audio Engine Status: %s", dac.isStreamRunning() ? "Running" : "Stopped");
        ImGui::Text("Playhead: %.2f Beats", state.playheadPositionBeats.load());

        // Example: Sending a test Note On event from Main Thread to Audio Thread
        if (ImGui::Button("Trigger Test Note On (Middle C)")) {
            AudioEvent evt{};
            evt.type = AudioEventType::NoteOn;
            evt.pitch = 60; // Middle C
            evt.velocity = 100;

            // Populate the patch data for the envelope
            evt.data.patch.attackTime = 0.1f;
            evt.data.patch.decayTime = 0.1f;
            evt.data.patch.sustainLevel = 0.7f;
            evt.data.patch.releaseTime = 0.5f;

            // Push lock-free
            if (!eventQueue.try_enqueue(evt)) {
                std::cerr << "Event Queue Full!\n";
            }
        }

        if (ImGui::Button("Trigger Test Note Off (Middle C)")) {
            AudioEvent evt{};
            evt.type = AudioEventType::NoteOff;
            evt.pitch = 60; // Middle C
            evt.velocity = 100;

            // Push lock-free
            if (!eventQueue.try_enqueue(evt)) {
                std::cerr << "Event Queue Full!\n";
            }
        }

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

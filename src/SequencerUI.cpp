#include "SequencerUI.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>

const ImU32 COLOR_NOTE           = IM_COL32(180, 50, 255, 255);
const ImU32 COLOR_NOTE_HOVERED   = IM_COL32(200, 100, 255, 255);
const ImU32 COLOR_PLAYHEAD       = IM_COL32(255, 255, 150, 255);
const ImU32 COLOR_GRID_LINE_BEAT = IM_COL32(40, 40, 50, 255);
const ImU32 COLOR_GRID_LINE_BAR  = IM_COL32(80, 80, 100, 255);
const ImU32 COLOR_KEY_BLACK      = IM_COL32(20, 20, 20, 255);
const ImU32 COLOR_KEY_WHITE      = IM_COL32(200, 200, 200, 255);

// Sequencer View State
static float s_pixelsPerBeat = 100.0f;
static float s_pixelsPerPitch = 20.0f;
static float s_scrollX = 0.0f;
static float s_scrollY = (127.0f - 60.0f) * s_pixelsPerPitch - 100.0f; // Center around C4 (60) initially

bool IsBlackKey(int midiNote) {
    int noteInOctave = midiNote % 12;
    return noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 || noteInOctave == 8 || noteInOctave == 10;
}

std::string GetNoteName(int midiNote) {
    static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int octave = (midiNote / 12) - 1;
    return std::string(noteNames[midiNote % 12]) + std::to_string(octave);
}

void DispatchSequenceUpdate(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    if (state.tracks.empty()) return;

    AudioEvent evt{};
    evt.type = AudioEventType::SequenceUpdate;
    evt.data.track = new Track(state.tracks[0]); // Create a copy for the audio thread to own
    if (!eventQueue.try_enqueue(evt)) {
        delete evt.data.track; // prevent leak if queue full
    }
}

void DrawSequencerUI(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("SequencerUI", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    float gutterWidth = 60.0f;
    ImVec2 gridPos = ImVec2(canvasPos.x + gutterWidth, canvasPos.y);
    ImVec2 gridSize = ImVec2(canvasSize.x - gutterWidth, canvasSize.y);

    // Ensure we have a track
    if (state.tracks.empty()) {
        Track newTrack;
        newTrack.patchName = "Additive Patch";
        state.tracks.push_back(newTrack);
        DispatchSequenceUpdate(state, eventQueue);
    }
    Track& track = state.tracks[0];

    // --- Interaction State ---
    ImGuiID sequencerId = ImGui::GetID("SequencerGrid");
    ImGui::ItemAdd(ImRect(gridPos, ImVec2(gridPos.x + gridSize.x, gridPos.y + gridSize.y)), sequencerId);

    // Keep track of what we are doing
    static int draggingNoteIndex = -1;
    static bool isResizing = false;
    static ImVec2 dragOffset;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;

    bool isHoveringGrid = ImGui::IsMouseHoveringRect(gridPos, ImVec2(gridPos.x + gridSize.x, gridPos.y + gridSize.y));

    // Middle mouse pan or horizontal scroll wheel
    if (isHoveringGrid && io.MouseDown[2]) { // Middle click
        s_scrollX -= io.MouseDelta.x;
        s_scrollY -= io.MouseDelta.y;
    }
    if (isHoveringGrid && io.MouseWheel != 0.0f && io.KeyShift) {
        s_scrollX -= io.MouseWheel * 50.0f;
    } else if (isHoveringGrid && io.MouseWheel != 0.0f) {
        s_scrollY -= io.MouseWheel * 50.0f;
    }

    s_scrollX = std::max(0.0f, s_scrollX);
    s_scrollY = std::clamp(s_scrollY, 0.0f, 128.0f * s_pixelsPerPitch - gridSize.y);

    // Background
    drawList->AddRectFilled(gridPos, ImVec2(gridPos.x + gridSize.x, gridPos.y + gridSize.y), IM_COL32(10, 10, 10, 255));

    // --- Draw Grid Lines ---
    float startBeat = s_scrollX / s_pixelsPerBeat;
    float endBeat = (s_scrollX + gridSize.x) / s_pixelsPerBeat;
    int firstBeat = static_cast<int>(startBeat);
    int lastBeat = static_cast<int>(endBeat) + 1;

    for (int i = firstBeat; i <= lastBeat; ++i) {
        float x = gridPos.x + (i * s_pixelsPerBeat) - s_scrollX;
        bool isBar = (i % 4 == 0);
        drawList->AddLine(ImVec2(x, gridPos.y), ImVec2(x, gridPos.y + gridSize.y), isBar ? COLOR_GRID_LINE_BAR : COLOR_GRID_LINE_BEAT);
    }

    int startPitch = 127 - static_cast<int>(s_scrollY / s_pixelsPerPitch);
    int endPitch = 127 - static_cast<int>((s_scrollY + gridSize.y) / s_pixelsPerPitch) - 1;
    startPitch = std::clamp(startPitch, 0, 127);
    endPitch = std::clamp(endPitch, 0, 127);

    for (int i = startPitch; i >= endPitch; --i) {
        float y = gridPos.y + ((127 - i) * s_pixelsPerPitch) - s_scrollY;
        drawList->AddLine(ImVec2(gridPos.x, y), ImVec2(gridPos.x + gridSize.x, y), COLOR_GRID_LINE_BEAT);
    }

    // --- Process Mouse Interaction ---
    bool noteChanged = false;
    static bool draggingOccurred = false;

    // Map mouse to pitch and beat
    float mouseBeat = (mousePos.x - gridPos.x + s_scrollX) / s_pixelsPerBeat;
    int mousePitch = 127 - static_cast<int>((mousePos.y - gridPos.y + s_scrollY) / s_pixelsPerPitch);

    // Snapping (16th note)
    float snapBeat = std::round(mouseBeat * 4.0f) / 4.0f;

    if (isHoveringGrid && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Find if we clicked on a note
        draggingNoteIndex = -1;
        isResizing = false;

        // Search in reverse order to click the "top" note if overlapping
        for (int i = static_cast<int>(track.notes.size()) - 1; i >= 0; --i) {
            auto& n = track.notes[i];
            float nx = gridPos.x + (n.startBeat * s_pixelsPerBeat) - s_scrollX;
            float ny = gridPos.y + ((127 - n.pitch) * s_pixelsPerPitch) - s_scrollY;
            float nw = n.lengthBeats * s_pixelsPerBeat;
            float nh = s_pixelsPerPitch;

            ImRect noteRect(ImVec2(nx, ny), ImVec2(nx + nw, ny + nh));
            if (noteRect.Contains(mousePos)) {
                draggingNoteIndex = i;

                // Check if clicking right edge for resizing
                if (mousePos.x > nx + nw - 10.0f) {
                    isResizing = true;
                } else {
                    dragOffset = ImVec2(mousePos.x - nx, mousePos.y - ny);
                }
                break;
            }
        }

        // If clicked empty space, add a new note
        if (draggingNoteIndex == -1 && mousePitch >= 0 && mousePitch <= 127) {
            Note newNote;
            newNote.startBeat = std::max(0.0f, snapBeat);
            newNote.lengthBeats = 0.25f; // 16th note default
            newNote.pitch = static_cast<uint8_t>(mousePitch);
            newNote.velocity = 100;
            track.notes.push_back(newNote);

            draggingNoteIndex = static_cast<int>(track.notes.size()) - 1;
            isResizing = true; // Drag to size immediately
            noteChanged = true;
            draggingOccurred = true;
        }
    } else if (isHoveringGrid && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        // Delete Note
        for (int i = static_cast<int>(track.notes.size()) - 1; i >= 0; --i) {
            auto& n = track.notes[i];
            float nx = gridPos.x + (n.startBeat * s_pixelsPerBeat) - s_scrollX;
            float ny = gridPos.y + ((127 - n.pitch) * s_pixelsPerPitch) - s_scrollY;
            float nw = n.lengthBeats * s_pixelsPerBeat;
            float nh = s_pixelsPerPitch;

            if (ImRect(ImVec2(nx, ny), ImVec2(nx + nw, ny + nh)).Contains(mousePos)) {
                track.notes.erase(track.notes.begin() + i);
                noteChanged = true;
                break;
            }
        }
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && draggingNoteIndex != -1) {
        auto& n = track.notes[draggingNoteIndex];

        if (isResizing) {
            float newEndBeat = mouseBeat;
            float newLength = newEndBeat - n.startBeat;
            float snappedLength = std::max(0.25f, std::round(newLength * 4.0f) / 4.0f);
            if (snappedLength != n.lengthBeats) {
                n.lengthBeats = snappedLength;
                draggingOccurred = true;
            }
        } else {
            // Move note
            float rawBeat = (mousePos.x - dragOffset.x - gridPos.x + s_scrollX) / s_pixelsPerBeat;
            float newStartBeat = std::max(0.0f, std::round(rawBeat * 4.0f) / 4.0f);
            int newPitch = 127 - static_cast<int>((mousePos.y - dragOffset.y - gridPos.y + s_scrollY + s_pixelsPerPitch * 0.5f) / s_pixelsPerPitch);
            newPitch = std::clamp(newPitch, 0, 127);

            if (newStartBeat != n.startBeat || newPitch != n.pitch) {
                n.startBeat = newStartBeat;
                n.pitch = static_cast<uint8_t>(newPitch);
                draggingOccurred = true;
            }
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (draggingNoteIndex != -1 && draggingOccurred) {
            noteChanged = true;
            draggingOccurred = false;
        }
        draggingNoteIndex = -1;
    }

    // --- Draw Notes ---
    for (int i = 0; i < static_cast<int>(track.notes.size()); ++i) {
        const auto& n = track.notes[i];

        // Culling
        if (n.pitch > startPitch || n.pitch < endPitch) continue;
        if (n.startBeat + n.lengthBeats < startBeat || n.startBeat > endBeat) continue;

        float nx = gridPos.x + (n.startBeat * s_pixelsPerBeat) - s_scrollX;
        float ny = gridPos.y + ((127 - n.pitch) * s_pixelsPerPitch) - s_scrollY;
        float nw = n.lengthBeats * s_pixelsPerBeat;
        float nh = s_pixelsPerPitch;

        ImRect noteRect(ImVec2(nx, ny), ImVec2(nx + nw, ny + nh));

        ImU32 color = (i == draggingNoteIndex || noteRect.Contains(mousePos)) ? COLOR_NOTE_HOVERED : COLOR_NOTE;

        drawList->AddRectFilled(noteRect.Min, noteRect.Max, color, 2.0f);
        drawList->AddRect(noteRect.Min, noteRect.Max, IM_COL32(0, 0, 0, 255), 2.0f); // Outline
    }

    // --- Draw Playhead ---
    float currentPlayheadBeat = state.playheadPositionBeats.load(std::memory_order_relaxed);
    if (currentPlayheadBeat >= startBeat && currentPlayheadBeat <= endBeat) {
        float px = gridPos.x + (currentPlayheadBeat * s_pixelsPerBeat) - s_scrollX;
        drawList->AddLine(ImVec2(px, gridPos.y), ImVec2(px, gridPos.y + gridSize.y), COLOR_PLAYHEAD, 2.0f);
    }

    // --- Draw Piano Gutter ---
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + gutterWidth, canvasPos.y + canvasSize.y), IM_COL32(0, 0, 0, 255));
    for (int i = startPitch; i >= endPitch; --i) {
        float y = canvasPos.y + ((127 - i) * s_pixelsPerPitch) - s_scrollY;

        bool isBlack = IsBlackKey(i);
        ImU32 color = isBlack ? COLOR_KEY_BLACK : COLOR_KEY_WHITE;

        drawList->AddRectFilled(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + gutterWidth - 1.0f, y + s_pixelsPerPitch), color);
        drawList->AddRect(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + gutterWidth - 1.0f, y + s_pixelsPerPitch), IM_COL32(50, 50, 50, 255));

        if (i % 12 == 0) { // C notes
            std::string name = GetNoteName(i);
            drawList->AddText(ImVec2(canvasPos.x + 5.0f, y + 2.0f), isBlack ? COLOR_KEY_WHITE : COLOR_KEY_BLACK, name.c_str());
        }
    }

    // Update audio thread if notes changed
    if (noteChanged) {
        DispatchSequenceUpdate(state, eventQueue);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}
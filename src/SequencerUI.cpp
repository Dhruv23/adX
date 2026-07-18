#include "SequencerUI.h"
#include "AudioFileLoader.h"
#include "AudioClipProcessor.h"
#include "AudioEffect.h"
#include "BpmDetector.h"
#include <imgui.h>
#include <imgui_internal.h>
#ifndef NOMINMAX
#define NOMINMAX // portable-file-dialogs.h pulls in <windows.h>; must precede it to keep std::min/std::max unshadowed
#endif
#include <portable-file-dialogs.h>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>

const ImU32 COLOR_NOTE           = IM_COL32(180, 50, 255, 255);
const ImU32 COLOR_NOTE_HOVERED   = IM_COL32(200, 100, 255, 255);
const ImU32 COLOR_CLIP           = IM_COL32(50, 170, 160, 255);
const ImU32 COLOR_CLIP_HOVERED   = IM_COL32(90, 210, 200, 255);
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
static bool s_arrangerMode = false;
static int s_selectedClipIndex = -1; // persists across mouse release, unlike draggingClipIndex
static int s_selectedTrackIndex = 0; // which track the Piano Roll/Arranger is currently showing/editing
static int s_forceSelectTrackIndex = -1; // one-shot: force this tab index active on its next appearance in the tab bar (e.g. a just-added track), then cleared

void SelectTrack(int index) {
    s_forceSelectTrackIndex = index;
}

static std::string GetFileBasename(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

static float GetClipDurationSeconds(const AudioClip& clip) {
    if (!clip.pcmData || clip.channels == 0 || clip.sampleRate == 0) return 0.0f;
    return static_cast<float>(clip.pcmData->size() / clip.channels) / static_cast<float>(clip.sampleRate);
}

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
    evt.data.tracks = new std::vector<Track>(state.tracks); // Create a copy for the audio thread to own
    if (!eventQueue.try_enqueue(evt)) {
        delete evt.data.tracks; // prevent leak if queue full
    }
}

void DrawSequencerUI(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("SequencerUI", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Dummy(ImVec2(4.0f, 4.0f));
    ImGui::SameLine();
    if (ImGui::RadioButton("Piano Roll", !s_arrangerMode)) s_arrangerMode = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Arranger", s_arrangerMode)) s_arrangerMode = true;

    bool sequenceChanged = false;

    // Keep track of what we are doing (declared here, above the track selector,
    // so switching/adding/deleting a track can reset state that's meaningless
    // once applied to a different track's note/clip list)
    static int draggingNoteIndex = -1;
    static bool isResizing = false;
    static ImVec2 dragOffset;
    static bool draggingOccurred = false;
    static int draggingClipIndex = -1;

    auto resetPerTrackUIState = [&]() {
        draggingNoteIndex = -1;
        isResizing = false;
        draggingClipIndex = -1;
        draggingOccurred = false;
        s_selectedClipIndex = -1;
    };

    // Ensure we have a track
    if (state.tracks.empty()) {
        Track newTrack;
        newTrack.patchName = "Additive Patch";
        state.tracks.push_back(newTrack);
        DispatchSequenceUpdate(state, eventQueue);
    }
    if (s_selectedTrackIndex < 0 || s_selectedTrackIndex >= static_cast<int>(state.tracks.size())) {
        s_selectedTrackIndex = static_cast<int>(state.tracks.size()) - 1;
    }

    // Captured BEFORE any of this frame's mutations (a SelectTrack() request
    // consumed via s_forceSelectTrackIndex in the tab loop below, tab clicks,
    // or a deletion's index-shift correction) so the comparison against
    // s_selectedTrackIndex at the end of this block can detect a change from
    // ANY of those sources, not just tab-bar clicks.
    int previousSelectedTrackIndex = s_selectedTrackIndex;

    // --- Track Selector ---
    // ImGui's tab bar owns "which tab is active" internally for user-driven
    // clicks — BeginTabItem's return value is mirrored into s_selectedTrackIndex
    // for that case. But deleting an EARLIER tab shifts every later track's index
    // down by one, and ImGui has no way to know that (our tab IDs are positional,
    // "###trackN" — there's no stable per-Track identity to key off), so after a
    // deletion we correct s_selectedTrackIndex ourselves via explicit index math
    // rather than trusting ImGui to keep pointing at the same logical track.
    int trackToDelete = -1;
    if (ImGui::BeginTabBar("Tracks")) {
        for (int i = 0; i < static_cast<int>(state.tracks.size()); ++i) {
            bool tabOpen = true;
            std::string label = "Track " + std::to_string(i + 1) + ": " + state.tracks[i].patchName + "###track" + std::to_string(i);
            ImGuiTabItemFlags itemFlags = ImGuiTabItemFlags_None;
            if (i == s_forceSelectTrackIndex) {
                itemFlags = ImGuiTabItemFlags_SetSelected;
                s_forceSelectTrackIndex = -1; // consumed — clear now, not before this tab ever saw it
            }
            if (ImGui::BeginTabItem(label.c_str(), state.tracks.size() > 1 ? &tabOpen : nullptr, itemFlags)) {
                s_selectedTrackIndex = i;
                ImGui::EndTabItem();
            }
            if (!tabOpen) {
                trackToDelete = i;
            }
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) {
            Track newTrack;
            newTrack.patchName = state.tracks[s_selectedTrackIndex].patchName;
            state.tracks.push_back(newTrack);
            s_forceSelectTrackIndex = static_cast<int>(state.tracks.size()) - 1; // force it active next frame's tab loop
            s_selectedTrackIndex = s_forceSelectTrackIndex; // show it immediately this frame too
            sequenceChanged = true;
        }
        ImGui::EndTabBar();
    }

    if (trackToDelete != -1 && state.tracks.size() > 1) {
        state.tracks.erase(state.tracks.begin() + trackToDelete);
        // Keep pointing at the same logical track: anything after the deleted
        // slot just shifted down by one index.
        if (trackToDelete < s_selectedTrackIndex) {
            s_selectedTrackIndex--;
        }
        if (s_selectedTrackIndex >= static_cast<int>(state.tracks.size())) {
            s_selectedTrackIndex = static_cast<int>(state.tracks.size()) - 1;
        }
        resetPerTrackUIState(); // unconditional: stale drag/selection indices are never safe to keep after a delete
        sequenceChanged = true;
    }

    if (s_selectedTrackIndex != previousSelectedTrackIndex) {
        resetPerTrackUIState();
    }

    Track& track = state.tracks[s_selectedTrackIndex];

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    float gutterWidth = 60.0f;
    ImVec2 gridPos = ImVec2(canvasPos.x + gutterWidth, canvasPos.y);
    ImVec2 gridSize = ImVec2(canvasSize.x - gutterWidth, canvasSize.y);

    // --- Interaction State ---
    ImGuiID sequencerId = ImGui::GetID("SequencerGrid");
    ImGui::ItemAdd(ImRect(gridPos, ImVec2(gridPos.x + gridSize.x, gridPos.y + gridSize.y)), sequencerId);

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

    // Map mouse to pitch and beat
    float mouseBeat = (mousePos.x - gridPos.x + s_scrollX) / s_pixelsPerBeat;
    int mousePitch = 127 - static_cast<int>((mousePos.y - gridPos.y + s_scrollY) / s_pixelsPerPitch);

    // Snapping (16th note)
    float snapBeat = std::round(mouseBeat * 4.0f) / 4.0f;

    float bpm = state.bpm.load(std::memory_order_relaxed);
    auto beatToSeconds = [bpm](float beat) { return beat * 60.0f / bpm; };
    auto secondsToBeat = [bpm](float seconds) { return seconds * bpm / 60.0f; };

    float clipLaneY = gridPos.y + 20.0f;
    float clipLaneHeight = 80.0f;

    if (!s_arrangerMode) {
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
            sequenceChanged = true;
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
                sequenceChanged = true;
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
            sequenceChanged = true;
            draggingOccurred = false;
        }
        draggingNoteIndex = -1;
    }
    } else { // Arranger mode: click/drag audio clips instead of notes
        if (isHoveringGrid && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            draggingClipIndex = -1;
            s_selectedClipIndex = -1;

            for (int i = static_cast<int>(track.audioClips.size()) - 1; i >= 0; --i) {
                auto& clip = track.audioClips[i];
                float cx = gridPos.x + (secondsToBeat(clip.startTimeSeconds) * s_pixelsPerBeat) - s_scrollX;
                float cw = secondsToBeat(GetClipDurationSeconds(clip)) * s_pixelsPerBeat;

                ImRect clipRect(ImVec2(cx, clipLaneY), ImVec2(cx + cw, clipLaneY + clipLaneHeight));
                if (clipRect.Contains(mousePos)) {
                    draggingClipIndex = i;
                    s_selectedClipIndex = i;
                    dragOffset = ImVec2(mousePos.x - cx, 0.0f);
                    break;
                }
            }

            // Clicked empty space: import a new audio clip at this position
            if (draggingClipIndex == -1 && mousePos.y >= clipLaneY && mousePos.y <= clipLaneY + clipLaneHeight) {
                auto selection = pfd::open_file("Import Audio Clip", ".", {"Audio Files", "*.wav *.mp3", "All Files", "*"}).result();
                if (!selection.empty()) {
                    float startTimeSeconds = beatToSeconds(std::max(0.0f, snapBeat));
                    auto clip = AudioFileLoader::LoadAudioClip(selection[0], startTimeSeconds);
                    if (clip) {
                        track.audioClips.push_back(std::move(*clip));
                        s_selectedClipIndex = static_cast<int>(track.audioClips.size()) - 1;
                        sequenceChanged = true;
                    }
                }
            }
        } else if (isHoveringGrid && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            for (int i = static_cast<int>(track.audioClips.size()) - 1; i >= 0; --i) {
                auto& clip = track.audioClips[i];
                float cx = gridPos.x + (secondsToBeat(clip.startTimeSeconds) * s_pixelsPerBeat) - s_scrollX;
                float cw = secondsToBeat(GetClipDurationSeconds(clip)) * s_pixelsPerBeat;

                if (ImRect(ImVec2(cx, clipLaneY), ImVec2(cx + cw, clipLaneY + clipLaneHeight)).Contains(mousePos)) {
                    track.audioClips.erase(track.audioClips.begin() + i);
                    if (s_selectedClipIndex == i) s_selectedClipIndex = -1;
                    else if (s_selectedClipIndex > i) s_selectedClipIndex--;
                    sequenceChanged = true;
                    break;
                }
            }
        }

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && draggingClipIndex != -1) {
            auto& clip = track.audioClips[draggingClipIndex];
            float rawBeat = (mousePos.x - dragOffset.x - gridPos.x + s_scrollX) / s_pixelsPerBeat;
            float newStartBeat = std::max(0.0f, std::round(rawBeat * 4.0f) / 4.0f);
            float newStartTimeSeconds = beatToSeconds(newStartBeat);
            if (newStartTimeSeconds != clip.startTimeSeconds) {
                clip.startTimeSeconds = newStartTimeSeconds;
                draggingOccurred = true;
            }
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (draggingClipIndex != -1 && draggingOccurred) {
                sequenceChanged = true;
                draggingOccurred = false;
            }
            draggingClipIndex = -1;
        }
    }

    // --- Draw Notes / Clips ---
    if (!s_arrangerMode) {
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
    } else {
        for (int i = 0; i < static_cast<int>(track.audioClips.size()); ++i) {
            const auto& clip = track.audioClips[i];

            float clipStartBeat = secondsToBeat(clip.startTimeSeconds);
            float clipEndBeat = clipStartBeat + secondsToBeat(GetClipDurationSeconds(clip));
            if (clipEndBeat < startBeat || clipStartBeat > endBeat) continue;

            float cx = gridPos.x + (clipStartBeat * s_pixelsPerBeat) - s_scrollX;
            float cw = (clipEndBeat - clipStartBeat) * s_pixelsPerBeat;

            ImRect clipRect(ImVec2(cx, clipLaneY), ImVec2(cx + cw, clipLaneY + clipLaneHeight));
            ImU32 color = (i == draggingClipIndex || clipRect.Contains(mousePos)) ? COLOR_CLIP_HOVERED : COLOR_CLIP;

            drawList->AddRectFilled(clipRect.Min, clipRect.Max, color, 2.0f);
            drawList->AddRect(clipRect.Min, clipRect.Max, IM_COL32(0, 0, 0, 255), 2.0f);
            drawList->AddText(ImVec2(clipRect.Min.x + 4.0f, clipRect.Min.y + 4.0f), IM_COL32(0, 0, 0, 255), GetFileBasename(clip.filePath).c_str());
        }
    }

    // --- Draw Playhead ---
    float currentPlayheadBeat = state.playheadPositionBeats.load(std::memory_order_relaxed);
    if (currentPlayheadBeat >= startBeat && currentPlayheadBeat <= endBeat) {
        float px = gridPos.x + (currentPlayheadBeat * s_pixelsPerBeat) - s_scrollX;
        drawList->AddLine(ImVec2(px, gridPos.y), ImVec2(px, gridPos.y + gridSize.y), COLOR_PLAYHEAD, 2.0f);
    }

    // --- Draw Gutter ---
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + gutterWidth, canvasPos.y + canvasSize.y), IM_COL32(0, 0, 0, 255));
    if (!s_arrangerMode) {
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
    } else {
        drawList->AddText(ImVec2(canvasPos.x + 5.0f, clipLaneY + 4.0f), COLOR_KEY_WHITE, track.patchName.c_str());
    }

    // --- Sample Browser drag-and-drop target (Phase 5) ---
    // Accepts "ADX_SAMPLE_PATH" payloads dropped anywhere on the grid and
    // imports the file as an AudioClip on the selected track at the snapped
    // drop position. Loops with a confidently detected BPM are automatically
    // time-stretched to the project tempo so they align with snapBeat.
    if (ImGui::BeginDragDropTargetCustom(ImRect(gridPos, ImVec2(gridPos.x + gridSize.x, gridPos.y + gridSize.y)), sequencerId)) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ADX_SAMPLE_PATH")) {
            std::string droppedPath(static_cast<const char*>(payload->Data));
            float dropBeat = std::max(0.0f, snapBeat);
            auto clip = AudioFileLoader::LoadAudioClip(droppedPath, beatToSeconds(dropBeat));
            if (clip) {
                float durationSec = GetClipDurationSeconds(*clip);
                if (durationSec >= 2.0f) {
                    // Potential loop: BPM-match it to the project tempo.
                    auto mono = AudioFileLoader::DecodeMono(droppedPath, kEngineSampleRate);
                    float detectedBpm = mono ? BpmDetector::EstimateBpm(*mono, kEngineSampleRate) : 0.0f;
                    if (detectedBpm > 0.0f && std::abs(detectedBpm - bpm) > 0.5f) {
                        clip->timeStretchFactor = detectedBpm / bpm;
                        AudioClipProcessor::ReprocessClip(*clip);
                        std::cout << "SequencerUI: BPM-matched '" << droppedPath << "' ("
                                  << detectedBpm << " -> " << bpm << " BPM)\n";
                    }
                }
                track.audioClips.push_back(std::move(*clip));
                s_selectedClipIndex = static_cast<int>(track.audioClips.size()) - 1;
                if (!s_arrangerMode) s_arrangerMode = true; // clips live in Arranger view — show what was just dropped
                sequenceChanged = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // --- Track FX Panel (Phase 5): insert-effect chain of the selected track ---
    {
        ImGui::SetNextWindowPos(ImVec2(canvasPos.x + gutterWidth + 10.0f, canvasPos.y + 30.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.9f);
        if (ImGui::Begin("TRACK FX", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("Track %d: %s", s_selectedTrackIndex + 1, track.patchName.c_str());
            ImGui::Separator();

            int effectToRemove = -1;
            for (int i = 0; i < static_cast<int>(track.effects.size()); ++i) {
                auto& fx = track.effects[i];
                if (!fx) continue;
                ImGui::PushID(i);
                ImGui::Text("%s", fx->typeName());
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) effectToRemove = i;

                // Parameter edits write straight into the shared atomics — the
                // audio thread's aliased instance hears them immediately, no
                // sequence re-dispatch needed.
                if (auto* rev = dynamic_cast<ReverbEffect*>(fx.get())) {
                    float mix = rev->mix.load();
                    float room = rev->roomSize.load();
                    float damp = rev->damping.load();
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f")) rev->mix.store(mix);
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Room Size", &room, 0.0f, 1.0f, "%.2f")) rev->roomSize.store(room);
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Damping", &damp, 0.0f, 1.0f, "%.2f")) rev->damping.store(damp);
                } else if (auto* dist = dynamic_cast<DistortionEffect*>(fx.get())) {
                    float drive = dist->drive.load();
                    float mix = dist->mix.load();
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Drive", &drive, 1.0f, 30.0f, "%.1f")) dist->drive.store(drive);
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f")) dist->mix.store(mix);
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (effectToRemove >= 0) {
                track.effects.erase(track.effects.begin() + effectToRemove);
                sequenceChanged = true;
            }

            if (ImGui::Button("+ REVERB")) {
                track.effects.push_back(std::make_shared<ReverbEffect>());
                sequenceChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("+ DISTORTION")) {
                track.effects.push_back(std::make_shared<DistortionEffect>());
                sequenceChanged = true;
            }
        }
        ImGui::End();
    }

    // --- Selected Clip Pitch/Stretch Panel (Arranger mode only) ---
    // Bounds-checked every frame: the clip list can shrink from a delete,
    // hot-reload, or project reload, invalidating a stale selected index.
    if (s_arrangerMode && s_selectedClipIndex >= 0 && s_selectedClipIndex < static_cast<int>(track.audioClips.size())) {
        AudioClip& selectedClip = track.audioClips[s_selectedClipIndex];

        ImGui::SetNextWindowPos(ImVec2(canvasPos.x + canvasSize.x - 280.0f, canvasPos.y + 30.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("ClipParams", nullptr, panelFlags)) {
            ImGui::Text("%s", GetFileBasename(selectedClip.filePath).c_str());
            ImGui::Separator();

            float pitch = selectedClip.pitchShiftSemitones;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Pitch (semitones)", &pitch, -24.0f, 24.0f, "%.1f")) {
                selectedClip.pitchShiftSemitones = pitch;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                AudioClipProcessor::ReprocessClip(selectedClip);
                sequenceChanged = true;
            }

            float stretch = selectedClip.timeStretchFactor;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Time Stretch", &stretch, 0.25f, 4.0f, "%.2fx")) {
                selectedClip.timeStretchFactor = stretch;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                AudioClipProcessor::ReprocessClip(selectedClip);
                sequenceChanged = true;
            }
        }
        ImGui::End();
    }

    // Update audio thread if notes/clips changed
    if (sequenceChanged) {
        DispatchSequenceUpdate(state, eventQueue);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}
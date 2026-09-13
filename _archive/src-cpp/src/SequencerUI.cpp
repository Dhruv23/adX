#include "SequencerUI.h"
#include "AudioFileLoader.h"
#include "AudioClipProcessor.h"
#include "ClipPeakCache.h"
#include "AudioEffect.h"
#include "BpmDetector.h"
#include "TrackCleaner.h"
#include "AdxParser.h"
#include <imgui.h>
#include <imgui_internal.h>
#ifndef NOMINMAX
#define NOMINMAX // portable-file-dialogs.h pulls in <windows.h>; must precede it to keep std::min/std::max unshadowed
#endif
#include <portable-file-dialogs.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

const ImU32 COLOR_NOTE           = IM_COL32(180, 50, 255, 255);
const ImU32 COLOR_NOTE_HOVERED   = IM_COL32(200, 100, 255, 255);
const ImU32 COLOR_CLIP           = IM_COL32(50, 170, 160, 255);
const ImU32 COLOR_CLIP_HOVERED   = IM_COL32(90, 210, 200, 255);
const ImU32 COLOR_CLIP_BG        = IM_COL32(8, 8, 8, 255);
const ImU32 COLOR_CLIP_TITLE_BG  = IM_COL32(48, 48, 48, 255);
const ImU32 COLOR_CLIP_TITLE_TXT = IM_COL32(230, 230, 230, 255);
const ImU32 COLOR_CLIP_WAVEFORM  = IM_COL32(235, 235, 235, 255);
const ImU32 COLOR_CLIP_HOVER_TINT= IM_COL32(90, 210, 200, 50);
const ImU32 COLOR_CLIP_SELECTED  = IM_COL32(255, 255, 255, 255);
const ImU32 COLOR_PLAYHEAD       = IM_COL32(255, 255, 150, 255);
const ImU32 COLOR_GRID_LINE_BEAT = IM_COL32(40, 40, 50, 255);
const ImU32 COLOR_GRID_LINE_BAR  = IM_COL32(80, 80, 100, 255);
const ImU32 COLOR_KEY_BLACK      = IM_COL32(20, 20, 20, 255);
const ImU32 COLOR_KEY_WHITE      = IM_COL32(200, 200, 200, 255);
const ImU32 COLOR_MARKER         = IM_COL32(255, 200, 60, 255);
const ImU32 COLOR_AUTOMATION_BG  = IM_COL32(15, 15, 20, 255);
const ImU32 COLOR_AUTOMATION_GRID= IM_COL32(40, 40, 50, 255);
const ImU32 COLOR_AUTOMATION_LINE= IM_COL32(80, 200, 255, 255);
const ImU32 COLOR_AUTOMATION_PT  = IM_COL32(255, 255, 255, 255);
const ImU32 COLOR_AUTOMATION_PT_HOVER = IM_COL32(255, 220, 80, 255);

// Sequencer View State
static float s_pixelsPerBeat = 100.0f;
static float s_pixelsPerPitch = 20.0f;
static float s_scrollX = 0.0f;
static float s_scrollY = (127.0f - 60.0f) * s_pixelsPerPitch - 100.0f; // Center around C4 (60) initially
static bool s_arrangerMode = false;
static int s_selectedClipIndex = -1; // persists across mouse release, unlike draggingClipIndex
static int s_selectedTrackIndex = 0; // which track the Piano Roll/Arranger is currently showing/editing
static int s_forceSelectTrackIndex = -1; // one-shot: force this tab index active on its next appearance in the tab bar (e.g. a just-added track), then cleared

// --- live-PLAN L1: loop-region bracket drag state ---
static int s_draggingLoopBracket = -1; // 0 = start bracket, 1 = end bracket, -1 = none

// --- Phase 2: Automation Lane Editor state ---
static bool s_showAutomation = false;          // reserves a strip below the grid when true
static int s_selectedAutomationLaneIndex = -1;  // index into state.automation
static int s_draggingAutomationPointIndex = -1; // index into the selected lane's points
static bool s_automationDragOccurred = false;
static char s_newAutomationParamBuf[128] = "mix.volume";
static int s_newAutomationTrackCombo = 0; // index into tracks; state.tracks.size() means "MASTER"

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

// Draws `clip`'s cached min/max peak envelope (UI-Refactor Phase 2) across
// [rectMin, rectMax], vertically centered, using whichever cached mip tier
// is closest to the rect's on-screen pixel density — but only the columns
// falling within [visMinX, visMaxX), so a clip that's mostly scrolled off
// the visible grid doesn't walk its full on-screen width every frame. A
// no-op if the clip has no decoded audio yet (e.g. mid-import).
static void DrawClipWaveform(ImDrawList* drawList, const AudioClip& clip, ImVec2 rectMin, ImVec2 rectMax,
                              ImU32 color, float visMinX, float visMaxX) {
    const ClipPeaks* peaks = GetOrBuildClipPeaks(clip);
    if (!peaks || peaks->tiers.empty() || peaks->frameCount == 0) return;

    float widthPx = rectMax.x - rectMin.x;
    if (widthPx < 1.0f) return;
    float midY = (rectMin.y + rectMax.y) * 0.5f;
    float halfH = (rectMax.y - rectMin.y) * 0.5f;

    double samplesPerPixel = static_cast<double>(peaks->frameCount) / static_cast<double>(widthPx);
    const ClipPeaks::Tier* tier = &peaks->tiers.front();
    for (const auto& t : peaks->tiers) {
        if (static_cast<double>(t.samplesPerBucket) <= samplesPerPixel) tier = &t;
        else break;
    }
    int bucketCount = static_cast<int>(tier->mins.size());
    if (bucketCount == 0) return;

    int firstCol = std::clamp(static_cast<int>(std::floor(visMinX - rectMin.x)), 0, static_cast<int>(widthPx));
    int lastCol = std::clamp(static_cast<int>(std::ceil(visMaxX - rectMin.x)), firstCol, static_cast<int>(widthPx));
    for (int px = firstCol; px < lastCol; ++px) {
        double frameStart = (static_cast<double>(px) / widthPx) * peaks->frameCount;
        double frameEnd = (static_cast<double>(px + 1) / widthPx) * peaks->frameCount;
        int bStart = std::clamp(static_cast<int>(frameStart / tier->samplesPerBucket), 0, bucketCount - 1);
        int bEnd = std::clamp(static_cast<int>(frameEnd / tier->samplesPerBucket), bStart, bucketCount - 1);
        float mn = tier->mins[bStart], mx = tier->maxs[bStart];
        for (int b = bStart + 1; b <= bEnd; ++b) {
            mn = std::min(mn, tier->mins[b]);
            mx = std::max(mx, tier->maxs[b]);
        }
        float y0 = midY - mx * halfH;
        float y1 = midY - mn * halfH;
        if (y1 - y0 < 1.0f) { y0 -= 0.5f; y1 += 0.5f; }
        float x = rectMin.x + px + 0.5f;
        drawList->AddLine(ImVec2(x, y0), ImVec2(x, y1), color, 1.0f);
    }
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

    // Bundle tracks + the patch registry together (Phase 1) so the audio
    // thread can resolve each track's Patch* itself.
    auto* snapshot = new SequenceSnapshot();
    snapshot->tracks = state.tracks;
    snapshot->patches = state.patches;
    snapshot->automation = state.automation; // Phase 2

    AudioEvent evt{};
    evt.type = AudioEventType::SequenceUpdate;
    evt.data.sequence = snapshot;
    if (!eventQueue.try_enqueue(evt)) {
        delete snapshot; // prevent leak if queue full
    }
}

void DrawSequencerUI(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, const AudioEngine& engine) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("SequencerUI", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Dummy(ImVec2(4.0f, 4.0f));
    ImGui::SameLine();
    if (ImGui::RadioButton("Piano Roll", !s_arrangerMode)) s_arrangerMode = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Arranger", s_arrangerMode)) s_arrangerMode = true;
    ImGui::SameLine();
    ImGui::Checkbox("Automation", &s_showAutomation);
    ImGui::SameLine();
    bool loopEnabledCheckbox = state.loopEnabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Loop", &loopEnabledCheckbox)) {
        state.loopEnabled.store(loopEnabledCheckbox);
        AudioEvent evt{};
        evt.type = AudioEventType::LoopChange;
        evt.data.loopState.enabled = loopEnabledCheckbox;
        evt.data.loopState.startBeat = state.loopStartBeat.load(std::memory_order_relaxed);
        evt.data.loopState.endBeat = state.loopEndBeat.load(std::memory_order_relaxed);
        eventQueue.try_enqueue(evt);
    }

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
    // Monochrome/hard-edged re-skin of the tab row (UI-Refactor Phase 1) —
    // colors only, the underlying BeginTabBar/BeginTabItem add/close/select
    // behavior below is untouched.
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.16f, 0.16f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabUnfocused, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    if (ImGui::BeginTabBar("Tracks")) {
        for (int i = 0; i < static_cast<int>(state.tracks.size()); ++i) {
            bool tabOpen = true;
            std::string label = "Track " + std::to_string(i + 1) + ": " + state.tracks[i].patchName + "###track" + std::to_string(i);
            ImGuiTabItemFlags itemFlags = ImGuiTabItemFlags_None;
            if (i == s_forceSelectTrackIndex) {
                itemFlags = ImGuiTabItemFlags_SetSelected;
                s_forceSelectTrackIndex = -1; // consumed — clear now, not before this tab ever saw it
            }
            bool tabActive = ImGui::BeginTabItem(label.c_str(), state.tracks.size() > 1 ? &tabOpen : nullptr, itemFlags);
            ImVec2 tabMin = ImGui::GetItemRectMin();
            ImVec2 tabMax = ImGui::GetItemRectMax();
            if (tabActive) {
                s_selectedTrackIndex = i;
                ImGui::EndTabItem();
            }
            if (!tabOpen) {
                trackToDelete = i;
            }
            // "Mix level" sliver (UI-Refactor Phase 2 item 3): a faint
            // condensed peak preview along the tab's bottom edge, using the
            // track's first audio clip as a representative sample of what's
            // on this track, so there's a waveform cue even when Arranger
            // mode is only showing one track's full lane at a time.
            if (!state.tracks[i].audioClips.empty() && tabMax.y - 5.0f > tabMin.y) {
                DrawClipWaveform(ImGui::GetWindowDrawList(), state.tracks[i].audioClips[0],
                                  ImVec2(tabMin.x + 2.0f, tabMax.y - 5.0f), ImVec2(tabMax.x - 2.0f, tabMax.y - 1.0f),
                                  IM_COL32(255, 255, 255, 70), tabMin.x + 2.0f, tabMax.x - 2.0f);
            }
            // live-PLAN L4: per-track peak-hold meter, a thin fill bar along
            // the tab's TOP edge (the bottom edge is already the Mix-level
            // waveform sliver above) -- green/yellow/red like a
            // conventional DAW channel meter, width scaled by the engine's
            // decaying peak-hold for this bus.
            float peak = std::clamp(engine.GetTrackPeak(static_cast<size_t>(i)), 0.0f, 1.0f);
            if (peak > 0.001f && tabMax.x - 2.0f > tabMin.x + 2.0f) {
                float fullWidth = (tabMax.x - 2.0f) - (tabMin.x + 2.0f);
                float meterWidth = fullWidth * peak;
                ImU32 meterColor = peak > 0.85f ? IM_COL32(255, 60, 60, 220)
                                  : peak > 0.6f ? IM_COL32(255, 200, 50, 220)
                                                : IM_COL32(80, 220, 120, 220);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(tabMin.x + 2.0f, tabMin.y + 1.0f),
                    ImVec2(tabMin.x + 2.0f + meterWidth, tabMin.y + 3.0f),
                    meterColor);
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
    ImGui::PopStyleColor(5);

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
    // live-PLAN L1: thin ruler strip reserved above the note/clip grid for
    // the loop-region brackets below — shrinks the grid rather than drawing
    // over it, so a bracket drag can never be mistaken for a note click even
    // when scrolled to the top pitch row. UI-Refactor M10 (the consolidated
    // ruler pass, INTERLEAVED-PLAN.md collision point #2) builds its
    // bar:beat tick labels into this same strip below, on top of the loop
    // band/brackets rather than in a second parallel strip — one ruler,
    // shared by both features. Widened from L1's original 14px to fit a
    // small bar-number label alongside the loop brackets it already draws.
    float rulerHeight = 18.0f;
    float automationStripHeight = s_showAutomation ? std::min(160.0f, canvasSize.y * 0.35f) : 0.0f;
    ImVec2 rulerPos = ImVec2(canvasPos.x + gutterWidth, canvasPos.y);
    ImVec2 rulerSize = ImVec2(canvasSize.x - gutterWidth, rulerHeight);
    ImVec2 gridPos = ImVec2(canvasPos.x + gutterWidth, canvasPos.y + rulerHeight);
    ImVec2 gridSize = ImVec2(canvasSize.x - gutterWidth, canvasSize.y - automationStripHeight - rulerHeight);
    ImVec2 automationStripPos = ImVec2(gridPos.x, gridPos.y + gridSize.y);
    ImVec2 automationStripSize = ImVec2(gridSize.x, automationStripHeight);

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

    // --- Draw + Interact: Loop Region (live-PLAN L1) ---
    // Two draggable brackets on the ruler strip above the grid; the region
    // between them is shaded across the grid's full height so it reads at a
    // glance even while Loop is toggled off (bounds persist, just dimmed).
    {
        bool loopEnabledNow = state.loopEnabled.load(std::memory_order_relaxed);
        float loopStartBeatVal = state.loopStartBeat.load(std::memory_order_relaxed);
        float loopEndBeatVal = state.loopEndBeat.load(std::memory_order_relaxed);
        float loopStartX = gridPos.x + (loopStartBeatVal * s_pixelsPerBeat) - s_scrollX;
        float loopEndX = gridPos.x + (loopEndBeatVal * s_pixelsPerBeat) - s_scrollX;

        drawList->AddRectFilled(rulerPos, ImVec2(rulerPos.x + rulerSize.x, rulerPos.y + rulerSize.y), IM_COL32(18, 18, 22, 255));

        ImU32 bandColor = loopEnabledNow ? IM_COL32(90, 200, 140, 40) : IM_COL32(100, 100, 100, 20);
        ImU32 bracketColor = loopEnabledNow ? IM_COL32(90, 220, 150, 255) : IM_COL32(120, 120, 120, 180);
        if (loopEndBeatVal > loopStartBeatVal && loopEndX >= gridPos.x && loopStartX <= gridPos.x + gridSize.x) {
            float bandL = std::max(loopStartX, gridPos.x);
            float bandR = std::min(loopEndX, gridPos.x + gridSize.x);
            drawList->AddRectFilled(ImVec2(bandL, rulerPos.y), ImVec2(bandR, gridPos.y + gridSize.y), bandColor);
        }
        if (loopStartX >= rulerPos.x - 8.0f && loopStartX <= rulerPos.x + rulerSize.x + 8.0f) {
            drawList->AddLine(ImVec2(loopStartX, rulerPos.y), ImVec2(loopStartX, gridPos.y + gridSize.y), bracketColor, 2.0f);
            drawList->AddTriangleFilled(ImVec2(loopStartX, rulerPos.y), ImVec2(loopStartX + 7.0f, rulerPos.y), ImVec2(loopStartX, rulerPos.y + rulerSize.y), bracketColor);
        }
        if (loopEndX >= rulerPos.x - 8.0f && loopEndX <= rulerPos.x + rulerSize.x + 8.0f) {
            drawList->AddLine(ImVec2(loopEndX, rulerPos.y), ImVec2(loopEndX, gridPos.y + gridSize.y), bracketColor, 2.0f);
            drawList->AddTriangleFilled(ImVec2(loopEndX, rulerPos.y), ImVec2(loopEndX - 7.0f, rulerPos.y), ImVec2(loopEndX, rulerPos.y + rulerSize.y), bracketColor);
        }

        bool isHoveringRuler = ImGui::IsMouseHoveringRect(rulerPos, ImVec2(rulerPos.x + rulerSize.x, rulerPos.y + rulerSize.y));
        if (isHoveringRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (std::abs(mousePos.x - loopStartX) <= 7.0f) s_draggingLoopBracket = 0;
            else if (std::abs(mousePos.x - loopEndX) <= 7.0f) s_draggingLoopBracket = 1;
        }
        if (s_draggingLoopBracket != -1 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float rulerBeat = (mousePos.x - gridPos.x + s_scrollX) / s_pixelsPerBeat;
            float snappedBeat = std::max(0.0f, std::round(rulerBeat * 4.0f) / 4.0f);
            float newStart = loopStartBeatVal;
            float newEnd = loopEndBeatVal;
            if (s_draggingLoopBracket == 0) {
                newStart = std::min(snappedBeat, loopEndBeatVal - 0.25f);
            } else {
                newEnd = std::max(snappedBeat, loopStartBeatVal + 0.25f);
            }
            if (newStart != loopStartBeatVal || newEnd != loopEndBeatVal) {
                state.loopStartBeat.store(newStart);
                state.loopEndBeat.store(newEnd);
                AudioEvent evt{};
                evt.type = AudioEventType::LoopChange;
                evt.data.loopState.enabled = loopEnabledNow;
                evt.data.loopState.startBeat = newStart;
                evt.data.loopState.endBeat = newEnd;
                eventQueue.try_enqueue(evt);
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            s_draggingLoopBracket = -1;
        }
    }

    // --- Bar:beat ruler tick labels (UI-Refactor M10) ---
    // Drawn on top of the loop band/brackets above (same strip, same
    // beat->pixel mapping) rather than a second ruler pass — small,
    // shrunk-font bar numbers at each bar line, matching the reference's
    // ruler header. The running time readout goes in the gutter's top-left
    // corner cell instead, drawn further below (after "--- Draw Gutter
    // ---"), since that corner is repainted black by the gutter fill.
    {
        ImGui::SetWindowFontScale(0.75f);
        for (int i = firstBeat; i <= lastBeat; ++i) {
            if (i % 4 != 0 || i < 0) continue; // one label per bar (4/4), matching the bar/beat gridline split above
            float x = gridPos.x + (i * s_pixelsPerBeat) - s_scrollX;
            if (x < gridPos.x - 20.0f || x > gridPos.x + gridSize.x) continue;
            char barLabel[8];
            std::snprintf(barLabel, sizeof(barLabel), "%d", i / 4 + 1);
            drawList->AddText(ImVec2(x + 2.0f, rulerPos.y + 1.0f), IM_COL32(210, 210, 210, 220), barLabel);
        }
        ImGui::SetWindowFontScale(1.0f);
    }

    // --- Draw Arrangement Markers (Phase 2) ---
    for (const auto& marker : state.markers) {
        if (marker.beat < startBeat || marker.beat > endBeat) continue;
        float mx = gridPos.x + (marker.beat * s_pixelsPerBeat) - s_scrollX;
        drawList->AddLine(ImVec2(mx, gridPos.y), ImVec2(mx, gridPos.y + gridSize.y), COLOR_MARKER, 1.0f);
        drawList->AddText(ImVec2(mx + 3.0f, gridPos.y + 2.0f), COLOR_MARKER, marker.name.c_str());
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
        constexpr float kClipTitleHeight = 14.0f;
        for (int i = 0; i < static_cast<int>(track.audioClips.size()); ++i) {
            const auto& clip = track.audioClips[i];

            float clipStartBeat = secondsToBeat(clip.startTimeSeconds);
            float clipEndBeat = clipStartBeat + secondsToBeat(GetClipDurationSeconds(clip));
            if (clipEndBeat < startBeat || clipStartBeat > endBeat) continue;

            float cx = gridPos.x + (clipStartBeat * s_pixelsPerBeat) - s_scrollX;
            float cw = (clipEndBeat - clipStartBeat) * s_pixelsPerBeat;

            ImRect clipRect(ImVec2(cx, clipLaneY), ImVec2(cx + cw, clipLaneY + clipLaneHeight));
            bool hovered = (i == draggingClipIndex || clipRect.Contains(mousePos));
            ImU32 outlineColor = hovered ? COLOR_CLIP_HOVERED : COLOR_CLIP;

            // Black clip body + waveform fill (UI-Refactor Phase 2) — the
            // solid-color rectangle is now just the outline/hover tint.
            drawList->AddRectFilled(clipRect.Min, clipRect.Max, COLOR_CLIP_BG, 2.0f);

            ImVec2 waveMin(clipRect.Min.x, clipRect.Min.y + kClipTitleHeight);
            float visMinX = std::max(clipRect.Min.x, gridPos.x);
            float visMaxX = std::min(clipRect.Max.x, gridPos.x + gridSize.x);
            if (visMaxX > visMinX) {
                DrawClipWaveform(drawList, clip, waveMin, clipRect.Max, COLOR_CLIP_WAVEFORM, visMinX, visMaxX);
            }

            if (hovered) {
                drawList->AddRectFilled(clipRect.Min, clipRect.Max, COLOR_CLIP_HOVER_TINT, 2.0f);
            }

            drawList->AddRectFilled(clipRect.Min, ImVec2(clipRect.Max.x, clipRect.Min.y + kClipTitleHeight), COLOR_CLIP_TITLE_BG);
            drawList->AddText(ImVec2(clipRect.Min.x + 4.0f, clipRect.Min.y + 1.0f), COLOR_CLIP_TITLE_TXT, GetFileBasename(clip.filePath).c_str());

            drawList->AddRect(clipRect.Min, clipRect.Max, outlineColor, 2.0f);
            if (i == s_selectedClipIndex) {
                drawList->AddRect(clipRect.Min, clipRect.Max, COLOR_CLIP_SELECTED, 2.0f, 0, 2.0f);
            }
        }
    }

    // --- Draw Playhead ---
    float currentPlayheadBeat = state.playheadPositionBeats.load(std::memory_order_relaxed);
    if (currentPlayheadBeat >= startBeat && currentPlayheadBeat <= endBeat) {
        float px = gridPos.x + (currentPlayheadBeat * s_pixelsPerBeat) - s_scrollX;
        drawList->AddLine(ImVec2(px, gridPos.y), ImVec2(px, gridPos.y + gridSize.y), COLOR_PLAYHEAD, 2.0f);
    }

    // --- Automation Lane Strip (Phase 2) ---
    // Shares the main grid's beat<->x mapping (same s_scrollX/s_pixelsPerBeat)
    // so breakpoints line up visually under the notes/clips they modulate.
    if (s_showAutomation) {
        if (s_selectedAutomationLaneIndex < 0 || s_selectedAutomationLaneIndex >= static_cast<int>(state.automation.size())) {
            s_selectedAutomationLaneIndex = -1;
        }

        drawList->AddRectFilled(automationStripPos, ImVec2(automationStripPos.x + automationStripSize.x, automationStripPos.y + automationStripSize.y), COLOR_AUTOMATION_BG);
        drawList->AddLine(automationStripPos, ImVec2(automationStripPos.x + automationStripSize.x, automationStripPos.y), COLOR_GRID_LINE_BAR);
        for (int i = firstBeat; i <= lastBeat; ++i) {
            float x = gridPos.x + (i * s_pixelsPerBeat) - s_scrollX;
            bool isBar = (i % 4 == 0);
            drawList->AddLine(ImVec2(x, automationStripPos.y), ImVec2(x, automationStripPos.y + automationStripSize.y),
                               isBar ? COLOR_GRID_LINE_BAR : COLOR_AUTOMATION_GRID);
        }
        if (currentPlayheadBeat >= startBeat && currentPlayheadBeat <= endBeat) {
            float px = gridPos.x + (currentPlayheadBeat * s_pixelsPerBeat) - s_scrollX;
            drawList->AddLine(ImVec2(px, automationStripPos.y), ImVec2(px, automationStripPos.y + automationStripSize.y), COLOR_PLAYHEAD, 2.0f);
        }

        bool isHoveringAutomationStrip = ImGui::IsMouseHoveringRect(automationStripPos, ImVec2(automationStripPos.x + automationStripSize.x, automationStripPos.y + automationStripSize.y));

        if (s_selectedAutomationLaneIndex >= 0) {
            AutomationLane& lane = state.automation[s_selectedAutomationLaneIndex];

            // Auto-range the value axis from the lane's own points, padded so
            // extremes aren't drawn flush against the strip edges.
            float valueMin = 0.0f, valueMax = 1.0f;
            if (!lane.points.empty()) {
                valueMin = valueMax = lane.points[0].value;
                for (const auto& p : lane.points) {
                    valueMin = std::min(valueMin, p.value);
                    valueMax = std::max(valueMax, p.value);
                }
                if (valueMax - valueMin < 1e-4f) valueMax = valueMin + 1.0f;
                float pad = (valueMax - valueMin) * 0.15f;
                valueMin -= pad;
                valueMax += pad;
            }
            auto valueToY = [&](float v) {
                float t = (v - valueMin) / (valueMax - valueMin);
                return automationStripPos.y + automationStripSize.y * (1.0f - t);
            };
            auto yToValue = [&](float y) {
                float t = 1.0f - (y - automationStripPos.y) / automationStripSize.y;
                return valueMin + t * (valueMax - valueMin);
            };

            std::string laneLabel = lane.trackTarget + ": " + lane.paramTarget;
            drawList->AddText(ImVec2(automationStripPos.x + 4.0f, automationStripPos.y + 2.0f), COLOR_AUTOMATION_LINE, laneLabel.c_str());

            // Curve preview: sample across the visible beat window using the
            // SAME evaluator the audio thread uses, so what's drawn is what
            // plays.
            if (!lane.points.empty()) {
                constexpr int kSamples = 96;
                ImVec2 prev{};
                for (int s = 0; s <= kSamples; ++s) {
                    float beat = startBeat + (endBeat - startBeat) * (static_cast<float>(s) / kSamples);
                    float value = EvaluateAutomationLane(lane, beat);
                    ImVec2 pt(gridPos.x + beat * s_pixelsPerBeat - s_scrollX, valueToY(value));
                    if (s > 0) drawList->AddLine(prev, pt, COLOR_AUTOMATION_LINE, 1.5f);
                    prev = pt;
                }
            }

            // Hit-test existing points (reverse order = topmost first)
            int hoveredPointIndex = -1;
            for (int i = static_cast<int>(lane.points.size()) - 1; i >= 0; --i) {
                float px = gridPos.x + lane.points[i].beat * s_pixelsPerBeat - s_scrollX;
                float py = valueToY(lane.points[i].value);
                if (std::abs(mousePos.x - px) < 7.0f && std::abs(mousePos.y - py) < 7.0f) {
                    hoveredPointIndex = i;
                    break;
                }
            }

            for (int i = 0; i < static_cast<int>(lane.points.size()); ++i) {
                float px = gridPos.x + lane.points[i].beat * s_pixelsPerBeat - s_scrollX;
                float py = valueToY(lane.points[i].value);
                if (px < automationStripPos.x - 8.0f || px > automationStripPos.x + automationStripSize.x + 8.0f) continue;
                bool hot = (i == hoveredPointIndex || i == s_draggingAutomationPointIndex);
                drawList->AddCircleFilled(ImVec2(px, py), 5.0f, hot ? COLOR_AUTOMATION_PT_HOVER : COLOR_AUTOMATION_PT);
                drawList->AddCircle(ImVec2(px, py), 5.0f, IM_COL32(0, 0, 0, 255));
            }

            // Left click: ctrl+click an existing point cycles its curve type;
            // plain click on a point starts a drag; click on empty space
            // creates a new point there (and immediately starts dragging it,
            // matching the piano roll's click-to-create-then-size pattern).
            if (isHoveringAutomationStrip && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (hoveredPointIndex >= 0 && io.KeyCtrl) {
                    Breakpoint& bp = lane.points[hoveredPointIndex];
                    bp.curve = static_cast<Curve>((static_cast<int>(bp.curve) + 1) % 4);
                    sequenceChanged = true;
                } else if (hoveredPointIndex >= 0) {
                    s_draggingAutomationPointIndex = hoveredPointIndex;
                } else {
                    Breakpoint bp;
                    bp.beat = std::max(0.0f, snapBeat);
                    bp.value = yToValue(mousePos.y);
                    bp.curve = Curve::Linear;
                    auto insertPos = std::upper_bound(lane.points.begin(), lane.points.end(), bp,
                        [](const Breakpoint& a, const Breakpoint& b) { return a.beat < b.beat; });
                    auto it = lane.points.insert(insertPos, bp);
                    s_draggingAutomationPointIndex = static_cast<int>(it - lane.points.begin());
                    sequenceChanged = true;
                }
            } else if (isHoveringAutomationStrip && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hoveredPointIndex >= 0) {
                lane.points.erase(lane.points.begin() + hoveredPointIndex);
                if (s_draggingAutomationPointIndex == hoveredPointIndex) s_draggingAutomationPointIndex = -1;
                sequenceChanged = true;
            }

            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && s_draggingAutomationPointIndex >= 0 &&
                s_draggingAutomationPointIndex < static_cast<int>(lane.points.size())) {
                Breakpoint& bp = lane.points[s_draggingAutomationPointIndex];
                float newBeat = std::max(0.0f, std::round(((mousePos.x - gridPos.x + s_scrollX) / s_pixelsPerBeat) * 4.0f) / 4.0f);
                float newValue = yToValue(mousePos.y);
                if (newBeat != bp.beat || newValue != bp.value) {
                    bp.beat = newBeat;
                    bp.value = newValue;
                    s_automationDragOccurred = true;
                }
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && s_draggingAutomationPointIndex >= 0) {
                if (s_automationDragOccurred) {
                    // Dragging a point past a neighbor can reorder the beat
                    // sequence — EvaluateAutomationLane assumes ascending order.
                    std::sort(lane.points.begin(), lane.points.end(),
                              [](const Breakpoint& a, const Breakpoint& b) { return a.beat < b.beat; });
                    sequenceChanged = true;
                    s_automationDragOccurred = false;
                }
                s_draggingAutomationPointIndex = -1;
            }
        } else {
            drawList->AddText(ImVec2(automationStripPos.x + 4.0f, automationStripPos.y + 2.0f), COLOR_KEY_WHITE,
                               "No lane selected - pick or create one in TRACK FX > Automation");
        }
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

    // --- Running time readout (UI-Refactor M10) ---
    // The gutter's top-left corner, where the ruler and the track-header
    // column meet -- drawn AFTER "--- Draw Gutter ---" above, since that
    // section's black background fill (canvasPos.y downward) would
    // otherwise paint over it. "m:ss.s/bar", matching the reference's
    // "0:08:38 / bar" ruler-corner readout.
    {
        float bpmNow = std::max(1.0f, state.bpm.load(std::memory_order_relaxed));
        float playheadBeats = state.playheadPositionBeats.load(std::memory_order_relaxed);
        float playheadSeconds = playheadBeats * (60.0f / bpmNow);
        int playheadBar = static_cast<int>(playheadBeats / 4.0f) + 1;
        int mins = static_cast<int>(playheadSeconds) / 60;
        float secs = playheadSeconds - static_cast<float>(mins * 60);
        char timeLabel[32];
        std::snprintf(timeLabel, sizeof(timeLabel), "%d:%04.1f/%d", mins, secs, playheadBar);
        ImGui::SetWindowFontScale(0.75f);
        ImVec2 timeLabelSize = ImGui::CalcTextSize(timeLabel);
        // A small backing rect guarantees contrast regardless of whatever
        // the gutter's own per-pitch-row key coloring puts behind this
        // corner (the piano-key rows above start at canvasPos.y, not
        // gridPos.y, so the topmost key row can extend up into this cell).
        drawList->AddRectFilled(ImVec2(canvasPos.x, canvasPos.y), ImVec2(canvasPos.x + timeLabelSize.x + 4.0f, canvasPos.y + rulerHeight), IM_COL32(0, 0, 0, 200));
        drawList->AddText(ImVec2(canvasPos.x + 2.0f, canvasPos.y + 1.0f), IM_COL32(210, 210, 210, 255), timeLabel);
        ImGui::SetWindowFontScale(1.0f);
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
            ImGui::TextDisabled("Track %d", s_selectedTrackIndex + 1);
            ImGui::Separator();

            // --- Phase 1: per-track patch select + volume/pan ---
            {
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::BeginCombo("Patch", track.patchName.c_str())) {
                    for (const auto& [name, patch] : state.patches) {
                        bool isSelected = (name == track.patchName);
                        if (ImGui::Selectable(name.c_str(), isSelected)) {
                            track.patchName = name;
                            sequenceChanged = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat("Volume", &track.volume, 0.0f, 2.0f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) sequenceChanged = true;

                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat("Pan", &track.pan, -1.0f, 1.0f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) sequenceChanged = true;
            }
            ImGui::Separator();

            // --- Phase 2: automation lane picker + creator. The strip drawn
            // under the grid (when "Automation" is toggled on) edits whichever
            // lane is selected here. ---
            {
                ImGui::TextDisabled("Automation Lanes");

                std::string currentLaneLabel = "(none)";
                if (s_selectedAutomationLaneIndex >= 0 && s_selectedAutomationLaneIndex < static_cast<int>(state.automation.size())) {
                    const auto& sel = state.automation[s_selectedAutomationLaneIndex];
                    currentLaneLabel = sel.trackTarget + ": " + sel.paramTarget;
                }
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::BeginCombo("Lane", currentLaneLabel.c_str())) {
                    for (int i = 0; i < static_cast<int>(state.automation.size()); ++i) {
                        const auto& lane = state.automation[i];
                        std::string label = lane.trackTarget + ": " + lane.paramTarget;
                        if (ImGui::Selectable(label.c_str(), i == s_selectedAutomationLaneIndex)) {
                            s_selectedAutomationLaneIndex = i;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (s_selectedAutomationLaneIndex >= 0 && s_selectedAutomationLaneIndex < static_cast<int>(state.automation.size())) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete Lane")) {
                        state.automation.erase(state.automation.begin() + s_selectedAutomationLaneIndex);
                        s_selectedAutomationLaneIndex = -1;
                        sequenceChanged = true;
                    }
                }

                int trackCount = static_cast<int>(state.tracks.size());
                s_newAutomationTrackCombo = std::clamp(s_newAutomationTrackCombo, 0, trackCount);
                std::string trackComboLabel = (s_newAutomationTrackCombo == trackCount)
                    ? "MASTER" : state.tracks[s_newAutomationTrackCombo].patchName;
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::BeginCombo("Track##NewAutomation", trackComboLabel.c_str())) {
                    for (int i = 0; i < trackCount; ++i) {
                        if (ImGui::Selectable(state.tracks[i].patchName.c_str(), i == s_newAutomationTrackCombo)) {
                            s_newAutomationTrackCombo = i;
                        }
                    }
                    if (ImGui::Selectable("MASTER", trackCount == s_newAutomationTrackCombo)) {
                        s_newAutomationTrackCombo = trackCount;
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetNextItemWidth(220.0f);
                ImGui::InputText("Target##NewAutomation", s_newAutomationParamBuf, sizeof(s_newAutomationParamBuf));
                if (ImGui::SmallButton("+ Add Lane") && s_newAutomationParamBuf[0] != '\0') {
                    AutomationLane lane;
                    lane.trackTarget = (s_newAutomationTrackCombo == trackCount)
                        ? "MASTER" : state.tracks[s_newAutomationTrackCombo].patchName;
                    lane.paramTarget = s_newAutomationParamBuf;
                    Breakpoint seed;
                    lane.points.push_back(seed); // beat 0, value 0, Linear — a visible starting point to drag
                    state.automation.push_back(std::move(lane));
                    s_selectedAutomationLaneIndex = static_cast<int>(state.automation.size()) - 1;
                    s_showAutomation = true;
                    sequenceChanged = true;
                }
            }
            ImGui::Separator();

            // --- Clean & Structurize: algorithmic form analysis + rebuild ---
            // Reruns TrackCleaner over the messy extracted notes: segments them
            // into phrases, labels recurring sections (A/B/C...), best-guesses
            // the intended macro-form with a Markov heuristic, then rewrites the
            // track in strict time and saves a fresh .adx. Guarded on note count
            // so it can't run against an empty (or clip-only) track.
            static std::string s_cleanupSummary; // persists to show the last result
            const bool hasNotes = !track.notes.empty();
            ImGui::BeginDisabled(!hasNotes);
            if (ImGui::Button("CLEANUP TRACK")) {
                TrackCleaner::FormAnalysis analysis;
                std::vector<Note> cleaned = TrackCleaner::CleanNotes(
                    track.notes, /*beatsPerPhrase=*/16.0f, /*grid=*/0.25f, &analysis);
                if (!cleaned.empty()) {
                    track.notes = std::move(cleaned);
                    sequenceChanged = true; // triggers DispatchSequenceUpdate at frame end

                    // Persist the cleaned arrangement to its own .adx so the raw
                    // extraction isn't clobbered.
                    const std::string outPath = "cleaned_track.adx";
                    bool saved = AdxParser::SaveProject(outPath, state);

                    s_cleanupSummary = "Form: " + analysis.algorithmicForm +
                                       (analysis.usedMlBlueprint
                                            ? " -> " + analysis.targetBlueprint + " (ML)"
                                            : std::string()) +
                                       "  [" + analysis.matchedFormName + "]" +
                                       (saved ? "  saved " + outPath : "  (save failed)");
                    std::cout << "TrackCleaner: " << s_cleanupSummary << "\n";
                } else {
                    s_cleanupSummary = "Cleanup produced no notes (nothing to structure).";
                }
            }
            ImGui::EndDisabled();
            if (!hasNotes) {
                ImGui::SameLine();
                ImGui::TextDisabled("(no notes)");
            }
            if (!s_cleanupSummary.empty()) {
                ImGui::TextWrapped("%s", s_cleanupSummary.c_str());
            }
            ImGui::Separator();

            // --- C418 suite: arpeggiator (held chords become cascading steps) ---
            {
                const char* arpModes[] = {"Off", "Up", "Down", "Up-Down"};
                int mode = track.arp.mode;
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::Combo("Arpeggiator", &mode, arpModes, 4)) {
                    track.arp.mode = mode;
                    sequenceChanged = true;
                }
                if (track.arp.mode != 0) {
                    const char* rateLabels[] = {"1/4", "1/8", "1/16"};
                    const float rateValues[] = {1.0f, 0.5f, 0.25f};
                    int rateIdx = track.arp.rateBeats >= 0.75f ? 0 : (track.arp.rateBeats >= 0.375f ? 1 : 2);
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::Combo("Rate", &rateIdx, rateLabels, 3)) {
                        track.arp.rateBeats = rateValues[rateIdx];
                        sequenceChanged = true;
                    }
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::SliderInt("Octaves", &track.arp.octaves, 1, 4);
                    if (ImGui::IsItemDeactivatedAfterEdit()) sequenceChanged = true;
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::SliderFloat("Gate", &track.arp.gate, 0.1f, 0.95f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit()) sequenceChanged = true;
                }
                ImGui::Separator();
            }

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

            // STAKILLAZ suite: extreme ranges for hyperpop-style repitching
            float pitch = selectedClip.pitchShiftSemitones;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Pitch (semitones)", &pitch, -48.0f, 48.0f, "%.1f")) {
                selectedClip.pitchShiftSemitones = pitch;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                AudioClipProcessor::ReprocessClip(selectedClip);
                sequenceChanged = true;
            }

            float stretch = selectedClip.timeStretchFactor;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Time Stretch", &stretch, 0.1f, 8.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)) {
                selectedClip.timeStretchFactor = stretch;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                AudioClipProcessor::ReprocessClip(selectedClip);
                sequenceChanged = true;
            }

            // Quick-chop presets (vocal-chop staples): relative shifts
            auto chopButton = [&](const char* label, float deltaSemitones) {
                if (ImGui::SmallButton(label)) {
                    selectedClip.pitchShiftSemitones =
                        std::clamp(selectedClip.pitchShiftSemitones + deltaSemitones, -48.0f, 48.0f);
                    AudioClipProcessor::ReprocessClip(selectedClip);
                    sequenceChanged = true;
                }
            };
            chopButton("+12", 12.0f); ImGui::SameLine();
            chopButton("-12", -12.0f); ImGui::SameLine();
            chopButton("+7", 7.0f); ImGui::SameLine();
            chopButton("+19", 19.0f); // "chipmunk" octave+fifth

            bool reversed = selectedClip.reversed;
            if (ImGui::Checkbox("REVERSE", &reversed)) {
                selectedClip.reversed = reversed;
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
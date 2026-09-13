#include "LiveCodeEditor.h"
#include "PatternCompiler.h"
#include "SequencerUI.h" // DispatchSequenceUpdate

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

std::string_view TrimView(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

} // namespace

void LiveCodeEditor::SyncFromDisk(const std::string& path) {
    if (m_diskDirty) return; // don't clobber unsaved local edits
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return;
    std::ostringstream ss;
    ss << file.rdbuf();
    m_text = ss.str();
    m_filePath = path;
    m_diskDirty = false;
    m_diagnostics.clear();
}

// Scans for `[TRACK <name>]` sections and the `PATTERN=` lines inside them.
// Deliberately minimal compared to AdxParser::LoadProject -- this panel only
// needs enough structure to know which track a given PATTERN= line belongs
// to, not the whole .adx grammar.
std::vector<LiveCodeEditor::PatternLineRef> LiveCodeEditor::ScanPatternLines() const {
    std::vector<PatternLineRef> refs;
    std::istringstream iss(m_text);
    std::string rawLine;
    std::string currentTrack;
    bool inTrack = false;
    int lineIdx = 0;
    while (std::getline(iss, rawLine)) {
        std::string_view line = TrimView(rawLine);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            std::string_view header = line.substr(1, line.size() - 2);
            size_t sp = header.find(' ');
            if (sp != std::string_view::npos && header.substr(0, sp) == "TRACK") {
                currentTrack = std::string(header.substr(sp + 1));
                inTrack = true;
            } else {
                inTrack = false;
            }
        } else if (inTrack && line.size() >= 8 && line.substr(0, 8) == "PATTERN=") {
            refs.push_back(PatternLineRef{lineIdx, currentTrack, std::string(line.substr(8))});
        }
        lineIdx++;
    }
    return refs;
}

// Dry-run compile of every PATTERN= line -- never touches `state`. Refreshes
// both the panel's error display and (for successfully-compiled lines) the
// notes/spans Draw() uses for playhead-synced step highlighting (live-PLAN
// L4). Cheap enough (a handful of short strings, Main-Thread-only) to just
// run every frame rather than tracking edit-dirtiness -- which also means a
// freshly-loaded project gets highlighting immediately, with no "type
// something once to prime it" gap. Compiles at the current live cycle index
// (live-PLAN L5) so the step-highlight readout matches what
// CheckCycleBoundary actually applied to `state`, not always cycle 0.
void LiveCodeEditor::RescanDiagnostics(float loopStart, float loopEnd) {
    m_diagnostics.clear();
    m_compiledPatterns.clear();
    for (const auto& ref : ScanPatternLines()) {
        PatternCompiler::CompileResult compiled = PatternCompiler::Compile(ref.patternText, loopStart, loopEnd, m_cycleIndex);
        if (!compiled.ok) {
            m_diagnostics.push_back(Diagnostic{ref.line, "TRACK " + ref.trackName + ": " + compiled.error});
            continue;
        }
        CompiledPatternInfo info;
        info.line = ref.line;
        info.trackName = ref.trackName;
        info.patternText = ref.patternText;
        info.notes = std::move(compiled.notes);
        info.spans = std::move(compiled.spans);
        m_compiledPatterns.push_back(std::move(info));
    }
}

// Ctrl+Enter: the real evaluate. A track carrying a PATTERN= line treats
// that line as its single source of truth -- matching it to the existing
// Track by patchName and replacing its notes wholesale, the same way
// re-evaluating a Tidal/Strudel pattern replaces what's playing rather than
// accumulating. A compile error (or an unrecognized track name -- this
// panel only edits existing tracks, not authors brand-new ones) leaves that
// track's last-good notes untouched and is surfaced as a diagnostic instead
// of ever applying a partial/broken snapshot. Compiles at the CURRENT live
// cycle index (live-PLAN L5), so evaluating mid-playback on, say, cycle 5
// of an `every 4 rev` pattern reflects cycle 5's actual state rather than
// silently resetting it back to cycle 0's.
void LiveCodeEditor::CompileAndApply(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    float loopStart = state.loopStartBeat.load();
    float loopEnd = state.loopEndBeat.load();
    if (loopEnd <= loopStart) loopEnd = loopStart + 4.0f;

    m_diagnostics.clear();
    bool anyApplied = false;
    for (const auto& ref : ScanPatternLines()) {
        PatternCompiler::CompileResult compiled = PatternCompiler::Compile(ref.patternText, loopStart, loopEnd, m_cycleIndex);
        if (!compiled.ok) {
            m_diagnostics.push_back(Diagnostic{ref.line, "TRACK " + ref.trackName + ": " + compiled.error});
            continue;
        }
        auto it = std::find_if(state.tracks.begin(), state.tracks.end(),
                                [&](const Track& t) { return t.patchName == ref.trackName; });
        if (it == state.tracks.end()) {
            m_diagnostics.push_back(Diagnostic{ref.line, "TRACK " + ref.trackName +
                                                ": no matching track in the project (existing tracks only)"});
            continue;
        }
        it->notes = std::move(compiled.notes);
        anyApplied = true;
    }
    if (anyApplied) {
        DispatchSequenceUpdate(state, eventQueue);
    }
}

// live-PLAN L5 (stretch): re-evaluates cycle-dependent PATTERN= lines at
// real loop-wrap boundaries. No AudioEngine changes needed for this --
// state.playheadPositionBeats already wraps back down to loopStartBeat
// sample-accurately every cycle (live-PLAN L1/M2), so a same-frame decrease
// is itself proof a wrap just fired; a genuine sample-accurate cycle
// counter on the engine side would be more precise but is unnecessary for
// a UI-thread cosmetic/live-coding feature that's explicitly not meant to
// run per-sample.
void LiveCodeEditor::CheckCycleBoundary(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    bool isPlaying = state.isPlaying.load(std::memory_order_relaxed);
    if (isPlaying && !m_wasPlaying) {
        m_cycleIndex = 0; // fresh play session: "every N" counts from here
        m_prevPlayheadBeats = -1.0f;
    }
    m_wasPlaying = isPlaying;
    if (!isPlaying) return;

    float playhead = state.playheadPositionBeats.load(std::memory_order_relaxed);
    bool wrapped = m_prevPlayheadBeats >= 0.0f && playhead < m_prevPlayheadBeats - 0.01f;
    m_prevPlayheadBeats = playhead;
    if (!wrapped) return;
    m_cycleIndex++;

    float loopStart = state.loopStartBeat.load();
    float loopEnd = state.loopEndBeat.load();
    if (loopEnd <= loopStart) return;

    bool anyApplied = false;
    for (const auto& ref : ScanPatternLines()) {
        if (ref.patternText.find("every") == std::string::npos) continue; // cheap pre-filter: nothing else varies by cycleIndex
        PatternCompiler::CompileResult compiled = PatternCompiler::Compile(ref.patternText, loopStart, loopEnd, m_cycleIndex);
        if (!compiled.ok) continue; // surfaced by the per-frame diagnostics scan already; don't touch state on failure
        auto it = std::find_if(state.tracks.begin(), state.tracks.end(),
                                [&](const Track& t) { return t.patchName == ref.trackName; });
        if (it == state.tracks.end()) continue;
        it->notes = std::move(compiled.notes);
        anyApplied = true;
    }
    if (anyApplied) {
        DispatchSequenceUpdate(state, eventQueue);
    }
}

void LiveCodeEditor::SaveToDisk() {
    if (m_filePath.empty()) return;
    std::ofstream file(m_filePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return;
    file << m_text;
    file.close();
    m_diskDirty = false;
    // CheckForHotReload's existing poll (main.cpp) picks up this write on
    // its own within ~500ms and folds it into `state` -- deliberately not
    // AdxParser::SaveProject, which only ever serializes Track::notes back
    // out as individual NOTE lines and would silently destroy every
    // PATTERN= line on the next save (SequenceSnapshot/Track has no memory
    // of which notes came from a compiled pattern vs. a hand-authored NOTE
    // line). Writing this panel's own buffer verbatim is what actually
    // keeps "external editors and the in-app panel work against the same
    // file" true.
}

// ImGuiInputTextFlags_CallbackAlways companion for Draw()'s InputTextMultiline:
// ImGui's multiline widget inserts a newline on ANY Enter keypress,
// including Ctrl+Enter, since it has no built-in notion of "evaluate"
// shortcuts. Draw() arms m_pendingCtrlEnterStrip right before the call
// whenever Ctrl+Enter was pressed this frame; this callback then removes
// the newline ImGui just inserted at the cursor so evaluating a pattern
// never corrupts the line the cursor was sitting on.
int LiveCodeEditor::InputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* self = static_cast<LiveCodeEditor*>(data->UserData);
    if (self->m_pendingCtrlEnterStrip) {
        self->m_pendingCtrlEnterStrip = false;
        if (data->CursorPos > 0 && data->Buf[data->CursorPos - 1] == '\n') {
            data->DeleteChars(data->CursorPos - 1, 1);
        }
    }
    return 0;
}

void LiveCodeEditor::Draw(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue) {
    ImGui::SetNextWindowSize(ImVec2(520.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LIVE CODE")) {
        ImGui::End();
        return;
    }

    if (m_filePath.empty()) {
        ImGui::TextDisabled("Load or save a project to start live-coding its PATTERN= tracks.");
        ImGui::End();
        return;
    }

    ImGui::TextColored(m_diskDirty ? ImVec4(1.0f, 0.75f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                        "%s", m_diskDirty ? "* unsaved" : "saved");
    ImGui::SameLine();
    ImGui::TextDisabled("| %s | Ctrl+Enter: evaluate    Ctrl+S: save", m_filePath.c_str());

    ImGuiIO& io = ImGui::GetIO();
    bool ctrlEnterThisFrame = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    bool ctrlSThisFrame = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
    m_pendingCtrlEnterStrip = ctrlEnterThisFrame;

    // live-PLAN L5: advance/apply cycle-dependent transforms before the
    // dry-run rescan below, so step-highlighting reflects what this frame
    // actually applied rather than lagging a frame behind.
    CheckCycleBoundary(state, eventQueue);

    // Diagnostics + compiled patterns (for step highlighting below) are
    // refreshed every frame -- see RescanDiagnostics's comment for why that's
    // cheap enough not to gate on edits.
    RescanDiagnostics(state.loopStartBeat.load(), state.loopEndBeat.load());

    float diagLines = m_diagnostics.empty() ? 1.0f : static_cast<float>(std::min<size_t>(m_diagnostics.size(), 4)) + 1.0f;
    diagLines += 1.0f; // reserve one more line for the step-highlight status below
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= ImGui::GetTextLineHeightWithSpacing() * diagLines;

    bool changed = ImGui::InputTextMultiline("##livecode_buf", &m_text, avail,
                                              ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways,
                                              &LiveCodeEditor::InputTextCallback, this);
    bool focused = ImGui::IsItemFocused();

    if (changed) {
        m_diskDirty = true;
    }

    if (m_diagnostics.empty()) {
        ImGui::TextDisabled("No pattern errors.");
    } else {
        size_t shown = std::min<size_t>(m_diagnostics.size(), 4);
        for (size_t i = 0; i < shown; ++i) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Line %d: %s",
                                m_diagnostics[i].line + 1, m_diagnostics[i].message.c_str());
        }
    }

    // live-PLAN L4: playhead-synced step highlight. InputTextMultiline has
    // no per-glyph color hook (see the syntax-highlighting deviation noted
    // in INTERLEAVED-PLAN.md's M5 entry), so the "currently sounding step"
    // is surfaced as a status readout rather than an inline underline --
    // same reasoning, same tradeoff.
    float playhead = state.playheadPositionBeats.load(std::memory_order_relaxed);
    const CompiledPatternInfo* activeInfo = nullptr;
    size_t activeStepIdx = 0;
    for (const auto& info : m_compiledPatterns) {
        for (size_t i = 0; i < info.notes.size(); ++i) {
            const Note& n = info.notes[i];
            if (playhead >= n.startBeat && playhead < n.startBeat + n.lengthBeats) {
                activeInfo = &info;
                activeStepIdx = i;
                break;
            }
        }
        if (activeInfo) break;
    }
    if (activeInfo) {
        const PatternCompiler::StepSpan& span = activeInfo->spans[activeStepIdx];
        std::string stepText = activeInfo->patternText.substr(span.sourceStart, span.sourceLen);
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "-> Line %d, TRACK %s: \"%s\" @ beat %.2f",
                            activeInfo->line + 1, activeInfo->trackName.c_str(), stepText.c_str(),
                            activeInfo->notes[activeStepIdx].startBeat);
    } else {
        ImGui::TextDisabled("(no pattern step active)");
    }

    if (focused && ctrlEnterThisFrame) CompileAndApply(state, eventQueue);
    if (focused && ctrlSThisFrame) SaveToDisk();

    ImGui::End();
}

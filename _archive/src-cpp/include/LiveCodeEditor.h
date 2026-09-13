#pragma once

#include "AudioData.h"
#include "PatternCompiler.h"
#include <readerwriterqueue.h>
#include <string>
#include <vector>

struct ImGuiInputTextCallbackData;

// live-PLAN Phase L3: an in-app "LIVE CODE" panel that closes the loop
// entirely inside adX -- type, Ctrl+Enter, hear it -- instead of alt-tabbing
// to an external editor and saving, which is what CheckForHotReload
// (main.cpp) already supports on its own. This is additive to that
// pipeline, never a fork of it: the in-memory buffer mirrors the SAME file
// CheckForHotReload watches, and Ctrl+S writes straight back to it, so
// external editors and this panel stay interchangeable (see main.cpp's
// LoadProjectAndSync / CheckForHotReload, which call SyncFromDisk to keep
// the two in step).
class LiveCodeEditor {
public:
    // (Re)loads the buffer from `path` on disk. Called after a project
    // LOAD/command-line load, after "SAVE AS NEW", and by CheckForHotReload
    // whenever it applies an externally-made edit. A no-op while the panel
    // has unsaved local edits of its own (m_diskDirty) so an external change
    // landing mid-edit can't clobber work in progress.
    void SyncFromDisk(const std::string& path);

    // Draws the "LIVE CODE" panel. `state` is the live, authoritative
    // SequencerState every other panel already mutates directly.
    // Ctrl+Enter compiles this buffer's PATTERN= lines and ships a
    // SequenceUpdate immediately (PatternCompiler -> Track::notes ->
    // DispatchSequenceUpdate, the exact path Phase L2 built). Ctrl+S writes
    // the buffer to disk; CheckForHotReload's existing ~500ms poll then
    // folds that write into `state` on its own, so this panel never needs
    // its own second dispatch path for persistence. Also shows a
    // playhead-synced "now playing" status line (live-PLAN L4) naming the
    // pattern step currently sounding.
    void Draw(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue);

private:
    struct Diagnostic {
        int line = -1; // 0-based line index into m_text
        std::string message;
    };
    struct PatternLineRef {
        int line = 0;
        std::string trackName;   // from the enclosing [TRACK <name>]
        std::string patternText; // raw text after "PATTERN="
    };
    // live-PLAN L4: a successfully-compiled PATTERN= line, kept around so
    // Draw() can map the live playhead back to the step currently sounding
    // (see the step-highlight status line in Draw()).
    struct CompiledPatternInfo {
        int line = 0;
        std::string trackName;
        std::string patternText;
        std::vector<Note> notes;
        std::vector<PatternCompiler::StepSpan> spans; // parallel to notes
    };

    std::string m_filePath;
    std::string m_text;       // authoritative buffer; also InputTextMultiline's backing store
    bool m_diskDirty = false; // edited locally since the last successful SyncFromDisk/SaveToDisk
    bool m_pendingCtrlEnterStrip = false; // see Draw()'s InputTextCallback

    std::vector<Diagnostic> m_diagnostics;
    std::vector<CompiledPatternInfo> m_compiledPatterns;

    // live-PLAN L5 (stretch): a live, wrap-detected loop-cycle counter so
    // `every N rev` (and similar cycle-dependent transforms) actually vary
    // per real playback cycle rather than always compiling cycleIndex 0 --
    // see CheckCycleBoundary().
    int m_cycleIndex = 0;
    float m_prevPlayheadBeats = -1.0f;
    bool m_wasPlaying = false;

    std::vector<PatternLineRef> ScanPatternLines() const;
    void RescanDiagnostics(float loopStart, float loopEnd);
    void CompileAndApply(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue);
    void SaveToDisk();

    // Detects loop wraps from state.playheadPositionBeats (a decrease means
    // the engine's own sample-accurate wrap, from live-PLAN L1/M2, just
    // fired) and, on each new cycle, recompiles+applies only the PATTERN=
    // lines that actually mention "every" (a cheap textual pre-filter --
    // patterns without a cycle-dependent transform compile identically at
    // every cycleIndex, so re-evaluating them on every wrap would be pure
    // waste). Resets to cycle 0 whenever playback (re)starts, so "every 4"
    // counts from the start of this play session, not some arbitrary
    // wall-clock-since-launch offset.
    void CheckCycleBoundary(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue);

    static int InputTextCallback(ImGuiInputTextCallbackData* data);
};

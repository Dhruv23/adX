#pragma once

#include "AudioData.h"
#include <string>
#include <vector>

// Algorithmic "Clean & Structurize" pass for a messy extracted melody.
//
// The pipeline is deliberately split into small, side-effect-free functions so
// each stage (segmentation, similarity, clustering, form matching, the Markov
// ML heuristic, quantization, reconstruction) can be unit-tested in isolation
// against hand-built Note vectors without pulling in the audio engine or UI.
//
// Nothing here touches the audio thread or ImGui; callers own dispatching the
// resulting notes (SequencerUI does this after CleanTrack()).
namespace TrackCleaner {

// A fixed-length window of the timeline plus the notes whose onsets fall in it.
// Notes keep their ABSOLUTE startBeat here; rebasing to phrase-local time only
// happens when a phrase is promoted to a quantized "master section".
struct Phrase {
    int index = 0;             // 0-based position of this window on the timeline
    float startBeat = 0.0f;    // absolute beat where the window begins
    std::vector<Note> notes;   // notes with onset inside [startBeat, startBeat+len)
    char label = '?';          // 'A','B','C'... assigned by ClusterPhrases()
};

// Result of the full analysis stage, before any track mutation.
struct FormAnalysis {
    float beatsPerPhrase = 16.0f;      // window length used for segmentation
    std::vector<Phrase> phrases;       // segmented + labelled windows
    std::string algorithmicForm;       // e.g. "ABABB" (canonical relabelling)
    std::string matchedFormName;       // "Verse-Chorus", "Rondo", ... or "Freeform"
    std::string mlBlueprint;           // Markov+edit-distance best-guess canonical form
    std::string targetBlueprint;       // the form the rebuild will actually follow
    bool usedMlBlueprint = false;      // true when the ML guess replaced the raw form
};

// --- Stage 1: segmentation ---------------------------------------------------
// Split notes into consecutive windows of `beatsPerPhrase`. The number of
// windows is derived from the last note's end; empty trailing windows are not
// produced.
std::vector<Phrase> SegmentPhrases(const std::vector<Note>& notes, float beatsPerPhrase);

// --- Stage 2: similarity -----------------------------------------------------
// Grid-quantized rhythm+pitch similarity in [0,1]. 1.0 == identical content
// (or both silent), 0.0 == nothing in common. `grid` is the onset resolution in
// beats (0.25 == 16th notes). Transposition is NOT collapsed: a literally
// repeated section scores high, a transposed restatement scores lower (which is
// what we want for A/B/C labelling).
float PhraseSimilarity(const Phrase& a, const Phrase& b, float grid = 0.25f);

// --- Stage 3: clustering -----------------------------------------------------
// Greedy first-match clustering: each phrase joins the first existing cluster
// whose representative it matches at >= `threshold`, else starts a new one.
// Mutates `phrases[i].label` in place ('A' for the first cluster, 'B' next...).
void ClusterPhrases(std::vector<Phrase>& phrases, float threshold = 0.7f, float grid = 0.25f);

// Concatenate the phrase labels into a form string, e.g. "AABA".
std::string FormString(const std::vector<Phrase>& phrases);

// Relabel a form so its first distinct letter is 'A', the next new one 'B', etc.
// ("BCBD" -> "ABAC"). Lets templates and observations be compared regardless of
// which raw cluster happened to appear first.
std::string CanonicalizeForm(const std::string& form);

// --- Stage 4: form matching --------------------------------------------------
// Name of the foundational form `form` matches exactly (after canonicalization),
// or "Freeform" if none. Handles Strophic / Binary / Ternary / Rondo /
// Verse-Chorus and their common length variants.
std::string MatchKnownForm(const std::string& form);

// --- Stage 5: ML heuristic ---------------------------------------------------
// Best-guess canonical blueprint for a noisy form, using a first-order Markov
// model trained on a small corpus of standard structures combined with an
// edit-distance prior. Recovers dropped/added sections (e.g. "ABABB" ->
// "ABABCB").
std::string MlBestGuessBlueprint(const std::string& observedForm);

// --- Full analysis (stages 1-5) ----------------------------------------------
FormAnalysis Analyze(const std::vector<Note>& notes, float beatsPerPhrase = 16.0f);

// --- Stage 6: master-section extraction + quantization -----------------------
// Pick the most representative phrase carrying `label`, rebase it to start at
// beat 0, and snap every startBeat/lengthBeats to the `grid`. Returns {} if no
// phrase carries that label.
std::vector<Note> BuildMasterSection(const FormAnalysis& analysis, char label, float grid = 0.25f);

// --- Stage 7: reconstruction -------------------------------------------------
// Rebuild a full note list that follows `analysis.targetBlueprint`, cloning the
// quantized master section for each blueprint letter onto sequential windows.
// A blueprint letter with no captured master (a section the extractor dropped)
// is synthesized as a transposed variation of section 'A'.
std::vector<Note> Rebuild(const FormAnalysis& analysis, float grid = 0.25f);

// Convenience: Analyze + Rebuild in one call. `outAnalysis` (optional) receives
// the intermediate analysis for display/logging.
std::vector<Note> CleanNotes(const std::vector<Note>& notes,
                             float beatsPerPhrase = 16.0f,
                             float grid = 0.25f,
                             FormAnalysis* outAnalysis = nullptr);

} // namespace TrackCleaner

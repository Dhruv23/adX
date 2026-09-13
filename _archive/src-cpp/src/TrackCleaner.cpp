#include "TrackCleaner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>
#ifdef TRACKCLEANER_DEBUG
#include <cstdio>
#endif

namespace TrackCleaner {

namespace {

// Snap a beat value to the nearest multiple of `grid`.
float SnapToGrid(float beat, float grid) {
    if (grid <= 0.0f) return beat;
    return std::round(beat / grid) * grid;
}

// Total musical length of a note list = the latest note end, in beats.
float TotalBeats(const std::vector<Note>& notes) {
    float end = 0.0f;
    for (const auto& n : notes) end = std::max(end, n.startBeat + n.lengthBeats);
    return end;
}

// A single note onset reduced to its (grid slot, pitch). One entry per occupied
// slot — the highest pitch wins a collision so the melody line dominates over
// polyphonic residue.
struct Onset {
    int slot;
    int pitch;
};

// Phrase -> sorted list of onsets, quantized to `grid` relative to the phrase
// start. This is a LOOSE pre-quantization: it collapses most timing jitter into
// a slot, and the ±tolerance matching in PhraseSimilarity() forgives the rest
// (an onset that jittered across a slot boundary still matches its neighbour).
std::vector<Onset> PhraseOnsets(const Phrase& p, float grid) {
    std::map<int, int> bySlot; // slot -> highest pitch
    for (const auto& n : p.notes) {
        float rel = n.startBeat - p.startBeat;
        int slot = static_cast<int>(std::lround(rel / grid));
        auto it = bySlot.find(slot);
        if (it == bySlot.end() || n.pitch > it->second) bySlot[slot] = n.pitch;
    }
    std::vector<Onset> onsets;
    onsets.reserve(bySlot.size());
    for (auto& kv : bySlot) onsets.push_back({kv.first, kv.second});
    return onsets; // already sorted by slot (std::map is ordered)
}

// Levenshtein edit distance between two label strings.
int EditDistance(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// True when every letter of `sub` appears in `super` in order (i.e. `super`
// can be produced from `sub` by INSERTIONS only). Models the extractor having
// DROPPED whole sections: the observed form is intact but incomplete.
bool IsSubsequence(const std::string& sub, const std::string& super) {
    size_t j = 0;
    for (char c : super) {
        if (j < sub.size() && sub[j] == c) ++j;
    }
    return j == sub.size();
}

// The distinct canonical structures the ML heuristic considers as candidate
// blueprints (first-letter == 'A' labelling).
const std::vector<std::string>& Corpus() {
    static const std::vector<std::string> corpus = {
        "AAA", "AAAA",        // Strophic
        "AB", "AABB",         // Binary
        "ABA", "ABABA",       // Ternary (and its extension)
        "ABACA", "ABACABA",   // Rondo
        "ABAB", "ABABCB",     // Verse / Verse-Chorus (standard pop)
        "ABABCAB", "AABA",    // pop with bridge / 32-bar song form
    };
    return corpus;
}

// Weighted training set for the Markov model. Weights approximate how common
// each structure is in the pop-oriented material this tool targets, so the
// verse-chorus-with-bridge grammar (…B C B…) is learnt as normal rather than
// treated as rare noise. This is the "trained on standard pop structures" part
// of the heuristic.
const std::vector<std::pair<std::string, int>>& TrainingSet() {
    static const std::vector<std::pair<std::string, int>> training = {
        {"ABABCB", 5},   // verse-chorus-verse-chorus-BRIDGE-chorus (the pop default)
        {"ABABCAB", 3},  // …bridge into a final verse+chorus
        {"ABAB", 3},     // verse-chorus doubled
        {"AABA", 2},     // 32-bar song form
        {"ABA", 2},      // ternary
        {"ABACA", 2},    // rondo
        {"ABACABA", 1},  // extended rondo
        {"ABABA", 1},    // extended ternary
        {"AAAA", 1},     // strophic
        {"AB", 1},       // binary
        {"AABB", 1},     // binary (repeated)
    };
    return training;
}

// First-order Markov model over the label alphabet, trained on TrainingSet()
// with add-one (Laplace) smoothing. Trained once, reused.
struct MarkovModel {
    static constexpr int kAlphabet = 6; // A..F
    std::array<double, kAlphabet> start{};
    std::array<std::array<double, kAlphabet>, kAlphabet> trans{};

    MarkovModel() {
        std::array<double, kAlphabet> startCount{};
        std::array<std::array<double, kAlphabet>, kAlphabet> transCount{};
        for (auto& row : transCount) row.fill(1.0); // Laplace smoothing
        startCount.fill(1.0);

        for (const auto& [form, weight] : TrainingSet()) {
            if (form.empty()) continue;
            int first = form[0] - 'A';
            if (first >= 0 && first < kAlphabet) startCount[first] += weight;
            for (size_t i = 1; i < form.size(); ++i) {
                int a = form[i - 1] - 'A';
                int b = form[i] - 'A';
                if (a >= 0 && a < kAlphabet && b >= 0 && b < kAlphabet)
                    transCount[a][b] += weight;
            }
        }

        double startTot = std::accumulate(startCount.begin(), startCount.end(), 0.0);
        for (int i = 0; i < kAlphabet; ++i) start[i] = startCount[i] / startTot;
        for (int i = 0; i < kAlphabet; ++i) {
            double tot = std::accumulate(transCount[i].begin(), transCount[i].end(), 0.0);
            for (int j = 0; j < kAlphabet; ++j) trans[i][j] = transCount[i][j] / tot;
        }
    }

    // Average per-letter likelihood in [0,1] (geometric mean of step
    // probabilities). Length-normalised so it doesn't bias toward short forms.
    double Score(const std::string& form) const {
        if (form.empty()) return 0.0;
        int first = form[0] - 'A';
        if (first < 0 || first >= kAlphabet) return 0.0;
        double logLik = std::log(start[first]);
        for (size_t i = 1; i < form.size(); ++i) {
            int a = form[i - 1] - 'A', b = form[i] - 'A';
            if (a < 0 || a >= kAlphabet || b < 0 || b >= kAlphabet) return 0.0;
            logLik += std::log(trans[a][b]);
        }
        return std::exp(logLik / static_cast<double>(form.size()));
    }
};

const MarkovModel& Markov() {
    static const MarkovModel model;
    return model;
}

// Onset resolution (in grid slots) that two phrases may differ by and still be
// considered the "same" onset. ±1 slot (a 16th note at grid=0.25) forgives the
// timing jitter of a humanised / unquantized extraction.
constexpr int kOnsetTolerance = 1;

// Weight split between rhythm and pitch in PhraseSimilarity. Rhythm dominates:
// a repeated verse/chorus keeps its rhythmic profile even when the melody is
// varied or transposed, so rhythm is the reliable structural fingerprint.
constexpr float kRhythmWeight = 0.65f;
constexpr float kPitchWeight = 0.35f;

// How many distinct sections we tolerate before the clusterer is considered to
// have "exploded" and must be re-run at a looser threshold.
constexpr int kMaxLabels = 6;

// One clustering pass at a fixed threshold. Greedy first-match: each phrase
// joins the first existing cluster whose representative it matches at
// >= threshold, else starts a new one. Returns the number of clusters formed
// and writes labels into phrases[i].label.
int ClusterAtThreshold(std::vector<Phrase>& phrases, float threshold, float grid) {
    std::vector<int> representatives; // index of the first phrase in each cluster
    for (size_t i = 0; i < phrases.size(); ++i) {
        int assigned = -1;
        for (size_t c = 0; c < representatives.size(); ++c) {
            if (PhraseSimilarity(phrases[i], phrases[representatives[c]], grid) >= threshold) {
                assigned = static_cast<int>(c);
                break;
            }
        }
        if (assigned < 0) {
            assigned = static_cast<int>(representatives.size());
            representatives.push_back(static_cast<int>(i));
        }
        phrases[i].label = static_cast<char>('A' + std::min(assigned, 25));
    }
    return static_cast<int>(representatives.size());
}

} // namespace

std::vector<Phrase> SegmentPhrases(const std::vector<Note>& notes, float beatsPerPhrase) {
    std::vector<Phrase> phrases;
    if (beatsPerPhrase <= 0.0f || notes.empty()) return phrases;

    float total = TotalBeats(notes);
    int count = std::max(1, static_cast<int>(std::ceil(total / beatsPerPhrase)));
    phrases.resize(count);
    for (int i = 0; i < count; ++i) {
        phrases[i].index = i;
        phrases[i].startBeat = i * beatsPerPhrase;
    }
    for (const auto& n : notes) {
        int idx = static_cast<int>(n.startBeat / beatsPerPhrase);
        idx = std::clamp(idx, 0, count - 1);
        phrases[idx].notes.push_back(n);
    }
    return phrases;
}

float PhraseSimilarity(const Phrase& a, const Phrase& b, float grid) {
    std::vector<Onset> oa = PhraseOnsets(a, grid);
    std::vector<Onset> ob = PhraseOnsets(b, grid);

    if (oa.empty() && ob.empty()) return 1.0f;   // both silent -> same section
    if (oa.empty() || ob.empty()) return 0.0f;   // one silent -> unrelated

    // --- Tolerant one-to-one onset matching -------------------------------
    // Pair each onset in A with the nearest still-unused onset in B whose slot
    // is within ±kOnsetTolerance. Both lists are slot-sorted, so a forward scan
    // with a moving lower bound is enough. Matched pairs feed BOTH the rhythm
    // score (how many onsets line up) and the pitch score (do the aligned
    // onsets agree, up to a global transpose).
    std::vector<bool> usedB(ob.size(), false);
    std::vector<std::pair<int, int>> matchedPairs; // (pitchA, pitchB)
    size_t searchStart = 0;
    for (const Onset& na : oa) {
        int bestJ = -1;
        int bestDiff = kOnsetTolerance + 1;
        for (size_t j = searchStart; j < ob.size(); ++j) {
            int diff = ob[j].slot - na.slot;
            if (diff < -kOnsetTolerance) { // b still well before a; skip permanently
                if (j == searchStart) ++searchStart;
                continue;
            }
            if (diff > kOnsetTolerance) break; // b past the tolerance window; done
            if (usedB[j]) continue;
            int adiff = std::abs(diff);
            if (adiff < bestDiff) {
                bestDiff = adiff;
                bestJ = static_cast<int>(j);
            }
        }
        if (bestJ >= 0) {
            usedB[bestJ] = true;
            matchedPairs.emplace_back(na.pitch, ob[bestJ].pitch);
        }
    }

    int matches = static_cast<int>(matchedPairs.size());

    // Rhythm agreement: tolerant Jaccard over occupied slots.
    int unionCount = static_cast<int>(oa.size() + ob.size()) - matches;
    float rhythmSim = unionCount > 0 ? static_cast<float>(matches) / static_cast<float>(unionCount) : 1.0f;

    // --- Transposition-invariant pitch agreement --------------------------
    // Over the aligned pairs, find the single transpose offset (pitchA - pitchB)
    // that reconciles the most pairs, then score by that fraction. A literal
    // repeat scores 1.0; a transposed restatement also scores 1.0 (one dominant
    // offset); an unrelated melody scatters offsets and scores low. This makes
    // choruses that were re-voiced or shifted still read as the same section.
    float pitchSim = 0.0f;
    if (matches > 0) {
        std::map<int, int> offsetHistogram;
        int bestOffsetCount = 0;
        for (const auto& [pa, pb] : matchedPairs) {
            int count = ++offsetHistogram[pa - pb];
            bestOffsetCount = std::max(bestOffsetCount, count);
        }
        pitchSim = static_cast<float>(bestOffsetCount) / static_cast<float>(matches);
    }

    return kRhythmWeight * rhythmSim + kPitchWeight * pitchSim;
}

void ClusterPhrases(std::vector<Phrase>& phrases, float threshold, float grid) {
    // Adaptive thresholding: start at the requested threshold and, if the pass
    // fragments the song into more than kMaxLabels sections (the "alphabet soup"
    // failure on fuzzy extractions), progressively loosen it until the section
    // count is reasonable or we hit the floor. Greedy first-match clustering is
    // monotone in the threshold, so lowering it only ever merges clusters.
    constexpr float kThresholdFloor = 0.30f;
    constexpr float kThresholdStep = 0.05f;

    float t = threshold;
    int count = ClusterAtThreshold(phrases, t, grid);
    while (count > kMaxLabels && t - kThresholdStep >= kThresholdFloor - 1e-6f) {
        t -= kThresholdStep;
        count = ClusterAtThreshold(phrases, t, grid);
    }
#ifdef TRACKCLEANER_DEBUG
    std::fprintf(stderr, "ClusterPhrases: settled at threshold=%.2f -> %d sections\n", t, count);
#endif
}

std::string FormString(const std::vector<Phrase>& phrases) {
    std::string s;
    s.reserve(phrases.size());
    for (const auto& p : phrases) s.push_back(p.label);
    return s;
}

std::string CanonicalizeForm(const std::string& form) {
    std::unordered_map<char, char> remap;
    char next = 'A';
    std::string out;
    out.reserve(form.size());
    for (char c : form) {
        auto it = remap.find(c);
        if (it == remap.end()) {
            remap[c] = next;
            out.push_back(next);
            if (next < 'Z') ++next;
        } else {
            out.push_back(it->second);
        }
    }
    return out;
}

std::string MatchKnownForm(const std::string& form) {
    std::string c = CanonicalizeForm(form);
    if (c.empty()) return "Freeform";

    // Structural rules first (cover length variants a fixed table would miss).
    // A single section, or several identical ones, is Strophic — critically this
    // also stops a lone phrase from being "recovered" into a fabricated Binary
    // form by the ML stage (there's no evidence for a second section).
    bool allSame = std::all_of(c.begin(), c.end(), [&](char ch) { return ch == c[0]; });
    if (allSame) return "Strophic";

    static const std::map<std::string, std::string> table = {
        {"AB", "Binary"},        {"AABB", "Binary"},
        {"ABA", "Ternary"},      {"ABABA", "Ternary"},
        {"ABACA", "Rondo"},      {"ABACABA", "Rondo"},
        {"ABAB", "Verse-Chorus"},{"ABABCB", "Verse-Chorus"},
        {"ABABCAB", "Verse-Chorus"}, {"AABA", "Song Form (AABA)"},
    };
    auto it = table.find(c);
    if (it != table.end()) return it->second;
    return "Freeform";
}

std::string MlBestGuessBlueprint(const std::string& observedForm) {
    std::string obs = CanonicalizeForm(observedForm);
    if (obs.empty()) return obs;

    const auto& markov = Markov();
    std::string best = obs;
    double bestScore = -1.0;
    for (const auto& cand : Corpus()) {
        int dist = EditDistance(obs, cand);
        int denom = static_cast<int>(std::max(obs.size(), cand.size()));
        double editSim = denom == 0 ? 1.0 : 1.0 - static_cast<double>(dist) / denom;
        double markovScore = markov.Score(cand);

        double score;
        if (IsSubsequence(obs, cand)) {
            // Section-recovery band: `cand` contains everything we observed, in
            // order, and only ADDS sections — exactly the "extractor dropped a
            // section" case (e.g. "ABABB" is a subsequence of "ABABCB"). Always
            // preferred over the noisy-match band below; among recovery
            // candidates, fewest fabricated sections wins, Markov breaks ties.
            int insertions = static_cast<int>(cand.size() - obs.size());
            score = 2.0 - 0.1 * insertions + 0.01 * markovScore;
        } else {
            // Noisy-match band (substitutions/deletions): observed content had
            // to be altered to reach `cand`, so this can't be pure recovery.
            // A candidate SHORTER than observed only matches by discarding real
            // sections, so penalise that.
            double lengthPenalty = 0.15 * std::max<double>(
                0.0, static_cast<double>(obs.size()) - static_cast<double>(cand.size()));
            score = 0.6 * editSim + 0.4 * markovScore - lengthPenalty;
        }
#ifdef TRACKCLEANER_DEBUG
        std::fprintf(stderr, "  cand=%-8s sub=%d edit=%.3f markov=%.4f -> %.4f\n",
                     cand.c_str(), (int)IsSubsequence(obs, cand), editSim, markovScore, score);
#endif
        if (score > bestScore) {
            bestScore = score;
            best = cand;
        }
    }
    return best;
}

FormAnalysis Analyze(const std::vector<Note>& notes, float beatsPerPhrase) {
    FormAnalysis fa;
    fa.beatsPerPhrase = beatsPerPhrase;
    fa.phrases = SegmentPhrases(notes, beatsPerPhrase);
    ClusterPhrases(fa.phrases);

    std::string raw = FormString(fa.phrases);
    fa.algorithmicForm = CanonicalizeForm(raw);
    fa.matchedFormName = MatchKnownForm(fa.algorithmicForm);
    fa.mlBlueprint = MlBestGuessBlueprint(fa.algorithmicForm);

    // Trust the raw analysis when it already IS a canonical form; otherwise the
    // extractor likely dropped/added a section, so follow the ML blueprint.
    if (fa.matchedFormName != "Freeform") {
        fa.targetBlueprint = fa.algorithmicForm;
        fa.usedMlBlueprint = false;
    } else {
        fa.targetBlueprint = fa.mlBlueprint;
        fa.usedMlBlueprint = (fa.mlBlueprint != fa.algorithmicForm);
        fa.matchedFormName = MatchKnownForm(fa.mlBlueprint);
    }
    // Re-canonicalize the relabelled phrases so their labels line up with the
    // canonical blueprint letters ('A' == first cluster in both).
    std::string canon = CanonicalizeForm(raw);
    for (size_t i = 0; i < fa.phrases.size() && i < canon.size(); ++i)
        fa.phrases[i].label = canon[i];
    return fa;
}

std::vector<Note> BuildMasterSection(const FormAnalysis& analysis, char label, float grid) {
    // Collect candidate phrases carrying this label.
    std::vector<const Phrase*> candidates;
    for (const auto& p : analysis.phrases)
        if (p.label == label) candidates.push_back(&p);
    if (candidates.empty()) return {};

    // "Best" instance = the one most similar to its siblings (most
    // representative), tie-broken by richer content (more notes).
    const Phrase* best = candidates.front();
    float bestScore = -1.0f;
    for (const Phrase* c : candidates) {
        float sim = 0.0f;
        for (const Phrase* o : candidates)
            if (o != c) sim += PhraseSimilarity(*c, *o, grid);
        if (candidates.size() > 1) sim /= static_cast<float>(candidates.size() - 1);
        else sim = 1.0f;
        // Blend representativeness with note count so a lone rich phrase wins
        // over a lone sparse one.
        float score = sim + 0.001f * static_cast<float>(c->notes.size());
        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }

    // Rebase to beat 0 and snap to the grid.
    std::vector<Note> master;
    master.reserve(best->notes.size());
    for (const auto& n : best->notes) {
        Note q = n;
        q.startBeat = std::max(0.0f, SnapToGrid(n.startBeat - best->startBeat, grid));
        q.lengthBeats = std::max(grid, SnapToGrid(n.lengthBeats, grid));
        master.push_back(q);
    }
    std::sort(master.begin(), master.end(),
              [](const Note& a, const Note& b) { return a.startBeat < b.startBeat; });
    return master;
}

std::vector<Note> Rebuild(const FormAnalysis& analysis, float grid) {
    const std::string& blueprint = analysis.targetBlueprint;
    if (blueprint.empty()) return {};

    // Pre-build a quantized master for every label the blueprint references.
    std::unordered_map<char, std::vector<Note>> masters;
    for (char label : blueprint) {
        if (masters.count(label)) continue;
        masters[label] = BuildMasterSection(analysis, label, grid);
    }

    // A blueprint may call for a section the extractor never captured (the
    // "missing section" case). Synthesize it as section 'A' transposed up a
    // perfect fourth (+5 semitones) so the reconstruction still has contrasting
    // material where the form demands it.
    const std::vector<Note>* fallbackSource = nullptr;
    if (auto it = masters.find('A'); it != masters.end() && !it->second.empty())
        fallbackSource = &it->second;
    else {
        for (auto& kv : masters)
            if (!kv.second.empty()) { fallbackSource = &kv.second; break; }
    }
    for (auto& kv : masters) {
        if (!kv.second.empty() || !fallbackSource) continue;
        std::vector<Note> synth = *fallbackSource;
        for (auto& n : synth)
            n.pitch = static_cast<uint8_t>(std::clamp(static_cast<int>(n.pitch) + 5, 0, 127));
        kv.second = std::move(synth);
    }

    // Clone masters onto sequential windows following the blueprint.
    std::vector<Note> out;
    for (size_t i = 0; i < blueprint.size(); ++i) {
        float offset = static_cast<float>(i) * analysis.beatsPerPhrase;
        const auto& master = masters[blueprint[i]];
        for (const auto& n : master) {
            Note placed = n;
            placed.startBeat = n.startBeat + offset;
            out.push_back(placed);
        }
    }
    return out;
}

std::vector<Note> CleanNotes(const std::vector<Note>& notes, float beatsPerPhrase,
                             float grid, FormAnalysis* outAnalysis) {
    FormAnalysis fa = Analyze(notes, beatsPerPhrase);
    if (outAnalysis) *outAnalysis = fa;
    return Rebuild(fa, grid);
}

} // namespace TrackCleaner

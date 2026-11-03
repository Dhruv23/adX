// adx-thread: main
#include "engine/analysis/TrackCleaner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <utility>

namespace adx::analysis {

namespace {

std::int64_t snap(std::int64_t ticks, std::int64_t grid) {
    if (grid <= 0) {
        return ticks;
    }
    const std::int64_t half = grid / 2;
    return (ticks >= 0 ? (ticks + half) / grid : -((-ticks + half) / grid)) * grid;
}

std::int64_t totalTicks(const std::vector<CleanNote>& notes) {
    std::int64_t end = 0;
    for (const CleanNote& n : notes) {
        end = std::max(end, n.start + n.length);
    }
    return end;
}

/// One onset per occupied grid slot; the highest pitch wins a collision, so the melody
/// outvotes polyphonic residue.
struct Onset {
    std::int64_t slot;
    int pitch;
};

std::vector<Onset> onsetsOf(const Phrase& p, std::int64_t grid) {
    std::map<std::int64_t, int> bySlot;
    for (const CleanNote& n : p.notes) {
        const std::int64_t slot = snap(n.start - p.start, grid) / std::max<std::int64_t>(1, grid);
        auto [it, inserted] = bySlot.try_emplace(slot, n.pitch);
        if (!inserted && n.pitch > it->second) {
            it->second = n.pitch;
        }
    }
    std::vector<Onset> out;
    out.reserve(bySlot.size());
    for (const auto& [slot, pitch] : bySlot) {
        out.push_back({.slot = slot, .pitch = pitch});
    }
    return out;
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> previous(b.size() + 1);
    std::vector<int> current(b.size() + 1);
    std::iota(previous.begin(), previous.end(), 0);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
        }
        std::swap(previous, current);
    }
    return previous[b.size()];
}

/// `super` can be made from `sub` by insertions only: the extractor dropped sections.
bool isSubsequence(const std::string& sub, const std::string& super) {
    std::size_t j = 0;
    for (const char c : super) {
        if (j < sub.size() && sub[j] == c) {
            ++j;
        }
    }
    return j == sub.size();
}

const std::vector<std::string>& candidateForms() {
    static const std::vector<std::string> forms = {
        "AAA",   "AAAA",               // strophic
        "AB",    "AABB",               // binary
        "ABA",   "ABABA",              // ternary
        "ABACA", "ABACABA",            // rondo
        "ABAB",  "ABABCB",  "ABABCAB", // verse-chorus
        "AABA",                        // 32-bar song form
    };
    return forms;
}

/// Weighted by how common each form is in the pop material this targets, so the
/// verse-chorus-bridge grammar is learnt as normal.
const std::vector<std::pair<std::string, int>>& trainingForms() {
    static const std::vector<std::pair<std::string, int>> forms = {
        {"ABABCB", 5},  {"ABABCAB", 3}, {"ABAB", 3}, {"AABA", 2}, {"ABA", 2},  {"ABACA", 2},
        {"ABACABA", 1}, {"ABABA", 1},   {"AAAA", 1}, {"AB", 1},   {"AABB", 1},
    };
    return forms;
}

/// First-order Markov over A..F with add-one smoothing.
class Markov {
public:
    static constexpr int kAlphabet = 6;

    Markov() {
        std::array<double, kAlphabet> startCount{};
        std::array<std::array<double, kAlphabet>, kAlphabet> count{};
        startCount.fill(1.0);
        for (auto& row : count) {
            row.fill(1.0);
        }
        for (const auto& [form, weight] : trainingForms()) {
            startCount[static_cast<std::size_t>(form[0] - 'A')] += weight;
            for (std::size_t i = 1; i < form.size(); ++i) {
                count[static_cast<std::size_t>(form[i - 1] - 'A')]
                     [static_cast<std::size_t>(form[i] - 'A')] += weight;
            }
        }
        const double total = std::accumulate(startCount.begin(), startCount.end(), 0.0);
        for (std::size_t i = 0; i < kAlphabet; ++i) {
            m_start[i] = startCount[i] / total;
            const double row = std::accumulate(count[i].begin(), count[i].end(), 0.0);
            for (std::size_t j = 0; j < kAlphabet; ++j) {
                m_transition[i][j] = count[i][j] / row;
            }
        }
    }

    /// The geometric mean of the step probabilities: length-normalised.
    [[nodiscard]] double score(const std::string& form) const {
        if (form.empty()) {
            return 0.0;
        }
        const auto letter = [](char c) { return c - 'A'; };
        if (letter(form[0]) < 0 || letter(form[0]) >= kAlphabet) {
            return 0.0;
        }
        double logLikelihood = std::log(m_start[static_cast<std::size_t>(letter(form[0]))]);
        for (std::size_t i = 1; i < form.size(); ++i) {
            const int a = letter(form[i - 1]);
            const int b = letter(form[i]);
            if (a < 0 || a >= kAlphabet || b < 0 || b >= kAlphabet) {
                return 0.0;
            }
            logLikelihood +=
                std::log(m_transition[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)]);
        }
        return std::exp(logLikelihood / static_cast<double>(form.size()));
    }

private:
    std::array<double, kAlphabet> m_start{};
    std::array<std::array<double, kAlphabet>, kAlphabet> m_transition{};
};

const Markov& markov() {
    static const Markov model;
    return model;
}

/// ±1 slot forgives a humanised or unquantized extraction.
constexpr std::int64_t kOnsetTolerance = 1;
/// Rhythm is the reliable fingerprint: a repeated chorus keeps its rhythm when its
/// melody is varied.
constexpr float kRhythmWeight = 0.65F;
constexpr float kPitchWeight = 0.35F;
/// More sections than this and the clusterer has "exploded": loosen and retry.
constexpr int kMaxLabels = 6;
constexpr float kThresholdFloor = 0.30F;
constexpr float kThresholdStep = 0.05F;

int clusterAt(std::vector<Phrase>& phrases, float threshold, std::int64_t grid) {
    std::vector<std::size_t> representatives;
    for (Phrase& phrase : phrases) {
        std::size_t assigned = representatives.size();
        for (std::size_t c = 0; c < representatives.size(); ++c) {
            if (phraseSimilarity(phrase, phrases[representatives[c]], grid) >= threshold) {
                assigned = c;
                break;
            }
        }
        if (assigned == representatives.size()) {
            representatives.push_back(static_cast<std::size_t>(&phrase - phrases.data()));
        }
        phrase.label = static_cast<char>('A' + std::min<std::size_t>(assigned, 25));
    }
    return static_cast<int>(representatives.size());
}

} // namespace

std::vector<Phrase> segmentPhrases(const std::vector<CleanNote>& notes, std::int64_t phraseTicks) {
    std::vector<Phrase> phrases;
    if (phraseTicks <= 0 || notes.empty()) {
        return phrases;
    }
    const std::int64_t total = totalTicks(notes);
    const std::int64_t count = std::max<std::int64_t>(1, (total + phraseTicks - 1) / phraseTicks);
    phrases.resize(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < phrases.size(); ++i) {
        phrases[i].index = static_cast<std::uint32_t>(i);
        phrases[i].start = static_cast<std::int64_t>(i) * phraseTicks;
    }
    for (const CleanNote& n : notes) {
        const std::int64_t index = std::clamp<std::int64_t>(n.start / phraseTicks, 0, count - 1);
        phrases[static_cast<std::size_t>(index)].notes.push_back(n);
    }
    return phrases;
}

float phraseSimilarity(const Phrase& a, const Phrase& b, std::int64_t grid) {
    const std::vector<Onset> oa = onsetsOf(a, grid);
    const std::vector<Onset> ob = onsetsOf(b, grid);
    if (oa.empty() && ob.empty()) {
        return 1.0F;
    }
    if (oa.empty() || ob.empty()) {
        return 0.0F;
    }
    // Each onset of a pairs with the nearest unused onset of b within the tolerance.
    // Both are slot-sorted, so a forward scan with a moving lower bound is enough.
    std::vector<bool> used(ob.size(), false);
    std::vector<std::pair<int, int>> pairs;
    std::size_t from = 0;
    for (const Onset& na : oa) {
        std::size_t best = ob.size();
        std::int64_t bestDiff = kOnsetTolerance + 1;
        for (std::size_t j = from; j < ob.size(); ++j) {
            const std::int64_t diff = ob[j].slot - na.slot;
            if (diff < -kOnsetTolerance) {
                if (j == from) {
                    ++from;
                }
                continue;
            }
            if (diff > kOnsetTolerance) {
                break;
            }
            if (!used[j] && std::abs(diff) < bestDiff) {
                bestDiff = std::abs(diff);
                best = j;
            }
        }
        if (best < ob.size()) {
            used[best] = true;
            pairs.emplace_back(na.pitch, ob[best].pitch);
        }
    }
    const auto matches = static_cast<int>(pairs.size());
    const int unionCount = static_cast<int>(oa.size() + ob.size()) - matches;
    const float rhythm =
        unionCount > 0 ? static_cast<float>(matches) / static_cast<float>(unionCount) : 1.0F;

    // The one transpose that reconciles the most aligned pairs: a literal repeat and a
    // transposed restatement both score 1, an unrelated melody scatters.
    float pitch = 0.0F;
    if (matches > 0) {
        std::map<int, int> offsets;
        int most = 0;
        for (const auto& [pa, pb] : pairs) {
            most = std::max(most, ++offsets[pa - pb]);
        }
        pitch = static_cast<float>(most) / static_cast<float>(matches);
    }
    return (kRhythmWeight * rhythm) + (kPitchWeight * pitch);
}

float clusterPhrases(std::vector<Phrase>& phrases, std::int64_t grid, float threshold) {
    // Greedy first match is monotone in the threshold, so lowering it only merges.
    float t = threshold;
    int count = clusterAt(phrases, t, grid);
    while (count > kMaxLabels && t - kThresholdStep >= kThresholdFloor - 1e-6F) {
        t -= kThresholdStep;
        count = clusterAt(phrases, t, grid);
    }
    return t;
}

std::string formString(const std::vector<Phrase>& phrases) {
    std::string out;
    out.reserve(phrases.size());
    for (const Phrase& p : phrases) {
        out.push_back(p.label);
    }
    return out;
}

std::string canonicalForm(const std::string& form) {
    std::map<char, char> remap;
    char next = 'A';
    std::string out;
    out.reserve(form.size());
    for (const char c : form) {
        auto [it, inserted] = remap.try_emplace(c, next);
        if (inserted && next < 'Z') {
            ++next;
        }
        out.push_back(it->second);
    }
    return out;
}

std::string matchKnownForm(const std::string& form) {
    const std::string c = canonicalForm(form);
    if (c.empty()) {
        return "Freeform";
    }
    // One section, or several the same, is strophic. This also keeps a lone phrase from
    // being "recovered" into a binary form with no evidence for a second section.
    if (std::ranges::all_of(c, [&](char ch) { return ch == c[0]; })) {
        return "Strophic";
    }
    static const std::map<std::string, std::string> known = {
        {"AB", "Binary"},
        {"AABB", "Binary"},
        {"ABA", "Ternary"},
        {"ABABA", "Ternary"},
        {"ABACA", "Rondo"},
        {"ABACABA", "Rondo"},
        {"ABAB", "Verse-Chorus"},
        {"ABABCB", "Verse-Chorus"},
        {"ABABCAB", "Verse-Chorus"},
        {"AABA", "Song Form (AABA)"},
    };
    const auto it = known.find(c);
    return it != known.end() ? it->second : "Freeform";
}

std::string blueprintFor(const std::string& observedForm) {
    const std::string observed = canonicalForm(observedForm);
    if (observed.empty()) {
        return observed;
    }
    std::string best = observed;
    double bestScore = -1.0;
    for (const std::string& candidate : candidateForms()) {
        const double m = markov().score(candidate);
        double score = 0.0;
        if (isSubsequence(observed, candidate)) {
            // Recovery: the candidate only adds sections. Always preferred over a noisy
            // match; fewest invented sections wins, the Markov score breaks ties.
            const auto insertions = static_cast<double>(candidate.size() - observed.size());
            score = 2.0 - (0.1 * insertions) + (0.01 * m);
        } else {
            const auto longest = static_cast<double>(std::max(observed.size(), candidate.size()));
            const double editSimilarity = 1.0 - (editDistance(observed, candidate) / longest);
            // Shorter than observed only matches by discarding real sections.
            const double shorter = std::max(0.0, static_cast<double>(observed.size()) -
                                                     static_cast<double>(candidate.size()));
            score = (0.6 * editSimilarity) + (0.4 * m) - (0.15 * shorter);
        }
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

FormAnalysis analyzeForm(const std::vector<CleanNote>& notes, std::int64_t phraseTicks,
                         std::int64_t grid) {
    FormAnalysis out;
    out.phraseTicks = phraseTicks;
    out.phrases = segmentPhrases(notes, phraseTicks);
    clusterPhrases(out.phrases, grid);
    const std::string raw = formString(out.phrases);
    out.observedForm = canonicalForm(raw);
    out.formName = matchKnownForm(out.observedForm);
    out.blueprint = blueprintFor(out.observedForm);
    // A known form is trusted as observed; otherwise a section was likely dropped or
    // added, and the blueprint is followed.
    if (out.formName != "Freeform") {
        out.target = out.observedForm;
    } else {
        out.target = out.blueprint;
        out.usedBlueprint = out.blueprint != out.observedForm;
        out.formName = matchKnownForm(out.blueprint);
    }
    for (std::size_t i = 0; i < out.phrases.size(); ++i) {
        out.phrases[i].label = out.observedForm[i];
    }
    return out;
}

std::vector<CleanNote> masterSection(const FormAnalysis& analysis, char label, std::int64_t grid) {
    std::vector<const Phrase*> candidates;
    for (const Phrase& p : analysis.phrases) {
        if (p.label == label) {
            candidates.push_back(&p);
        }
    }
    if (candidates.empty()) {
        return {};
    }
    // The instance most like its siblings; a richer one breaks a tie.
    const Phrase* best = candidates.front();
    float bestScore = -1.0F;
    for (const Phrase* c : candidates) {
        float similarity = 1.0F;
        if (candidates.size() > 1) {
            similarity = 0.0F;
            for (const Phrase* other : candidates) {
                if (other != c) {
                    similarity += phraseSimilarity(*c, *other, grid);
                }
            }
            similarity /= static_cast<float>(candidates.size() - 1);
        }
        const float score = similarity + (0.001F * static_cast<float>(c->notes.size()));
        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }
    std::vector<CleanNote> master;
    master.reserve(best->notes.size());
    for (const CleanNote& n : best->notes) {
        CleanNote q = n;
        q.start = std::max<std::int64_t>(0, snap(n.start - best->start, grid));
        q.length = std::max(grid, snap(n.length, grid));
        master.push_back(q);
    }
    std::ranges::stable_sort(master, {}, &CleanNote::start);
    return master;
}

std::vector<CleanNote> rebuild(const FormAnalysis& analysis, std::int64_t grid) {
    const std::string& target = analysis.target;
    if (target.empty()) {
        return {};
    }
    std::map<char, std::vector<CleanNote>> masters;
    for (const char label : target) {
        if (!masters.contains(label)) {
            masters[label] = masterSection(analysis, label, grid);
        }
    }
    // A section the form calls for but nobody played: A up a perfect fourth, so the
    // rebuild still has contrast where the form wants it.
    const std::vector<CleanNote>* source = nullptr;
    if (const auto a = masters.find('A'); a != masters.end() && !a->second.empty()) {
        source = &a->second;
    } else {
        for (const auto& [label, notes] : masters) {
            if (!notes.empty()) {
                source = &notes;
                break;
            }
        }
    }
    if (source != nullptr) {
        const std::vector<CleanNote> fourth = [&] {
            std::vector<CleanNote> out = *source;
            for (CleanNote& n : out) {
                n.pitch = static_cast<std::uint8_t>(std::clamp(n.pitch + 5, 0, 127));
            }
            return out;
        }();
        for (auto& [label, notes] : masters) {
            if (notes.empty()) {
                notes = fourth;
            }
        }
    }
    std::vector<CleanNote> out;
    for (std::size_t i = 0; i < target.size(); ++i) {
        const std::int64_t offset = static_cast<std::int64_t>(i) * analysis.phraseTicks;
        for (CleanNote n : masters[target[i]]) {
            n.start += offset;
            out.push_back(n);
        }
    }
    return out;
}

std::vector<CleanNote> cleanNotes(const std::vector<CleanNote>& notes, std::int64_t phraseTicks,
                                  std::int64_t grid, FormAnalysis* analysis) {
    FormAnalysis form = analyzeForm(notes, phraseTicks, grid);
    std::vector<CleanNote> out = rebuild(form, grid);
    if (analysis != nullptr) {
        *analysis = std::move(form);
    }
    return out;
}

std::vector<FormMarker> formMarkers(const FormAnalysis& analysis) {
    std::vector<FormMarker> out;
    char previous = '\0';
    for (const Phrase& p : analysis.phrases) {
        if (p.label != previous) {
            out.push_back({.tick = p.start, .label = p.label});
            previous = p.label;
        }
    }
    return out;
}

} // namespace adx::analysis

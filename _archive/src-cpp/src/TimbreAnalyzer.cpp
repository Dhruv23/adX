#include "TimbreAnalyzer.h"
#include "AudioFileLoader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>

namespace {

constexpr int kNumHarmonics = 16;      // TimbreKeyframe capacity
constexpr float kMinNoteSeconds = 0.15f; // shorter notes have no stable sustain to probe
constexpr size_t kMaxAnalyzedNotes = 48; // longest notes win — pads/chords, the sound we're after

// Goertzel single-bin DFT magnitude of mono[start, start+len) at freqHz,
// under a Hann window (leakage from neighbouring chord tones in the mix is
// the dominant error source, so the window matters more than usual here).
float GoertzelMagnitude(const std::vector<float>& mono, size_t start, size_t len,
                        float freqHz, float sampleRate) {
    if (len < 32 || start + len > mono.size()) return 0.0f;
    const float w = 2.0f * 3.14159265358979323846f * freqHz / sampleRate;
    const float coeff = 2.0f * std::cos(w);
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
    for (size_t i = 0; i < len; ++i) {
        float hann = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979323846f *
                                            static_cast<float>(i) / static_cast<float>(len - 1));
        s0 = mono[start + i] * hann + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return std::sqrt(std::max(0.0f, power)) / (static_cast<float>(len) * 0.25f); // Hann coherent gain = 0.5, /2 for DFT half-amplitude
}

float MedianOf(std::vector<float>& v, float fallback) {
    if (v.empty()) return fallback;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Same envelope-table semantics as PatchLibrary's builder (attack 0->1,
// decay 1->sustain, release sustain->0); duplicated because that helper is
// file-local there and this is the only other user.
std::vector<float> BuildCurve(float fromLevel, float toLevel, float timeMs, float shape) {
    size_t samples = static_cast<size_t>((timeMs / 1000.0f) * kEngineSampleRate);
    if (samples == 0) samples = 1;
    std::vector<float> table(samples);
    for (size_t i = 0; i < samples; ++i) {
        float t = samples > 1 ? static_cast<float>(i) / static_cast<float>(samples - 1) : 1.0f;
        table[i] = fromLevel + (toLevel - fromLevel) * std::pow(t, shape);
    }
    return table;
}

struct FlatNote {
    float startSec;
    float lenSec;
    uint8_t pitch;
};

} // namespace

namespace TimbreAnalyzer {

std::optional<Patch> AnalyzePatch(const std::string& audioPath,
                                  const std::vector<MidiImporter::ImportedTrack>& tracks,
                                  float bpm,
                                  float tuningA4) {
    auto monoOpt = AudioFileLoader::DecodeMono(audioPath, kEngineSampleRate);
    if (!monoOpt || monoOpt->empty()) {
        std::cerr << "TimbreAnalyzer: could not decode " << audioPath << "\n";
        return std::nullopt;
    }
    const std::vector<float>& mono = *monoOpt;
    const float sr = static_cast<float>(kEngineSampleRate);
    const float secPerBeat = 60.0f / std::max(1.0f, bpm);

    // Flatten pitched (non-drum) notes to absolute seconds and keep the ones
    // long enough to have a sustain region inside the audio file.
    std::vector<FlatNote> candidates;
    for (const auto& t : tracks) {
        if (t.isPercussion) continue; // channel-10 hits would poison harmonic ratios
        for (const Note& n : t.notes) {
            FlatNote fn{n.startBeat * secPerBeat, n.lengthBeats * secPerBeat, n.pitch};
            if (fn.lenSec < kMinNoteSeconds) continue;
            if ((fn.startSec + fn.lenSec) * sr >= static_cast<float>(mono.size())) continue;
            candidates.push_back(fn);
        }
    }
    if (candidates.empty()) {
        std::cerr << "TimbreAnalyzer: no analyzable notes (all too short or outside the audio)\n";
        return std::nullopt;
    }

    // Longest notes first: sustained pad/chord tones give the cleanest
    // harmonic snapshots and are exactly the voices worth cloning.
    std::sort(candidates.begin(), candidates.end(),
              [](const FlatNote& a, const FlatNote& b) { return a.lenSec > b.lenSec; });
    if (candidates.size() > kMaxAnalyzedNotes) candidates.resize(kMaxAnalyzedNotes);

    // --- Harmonic timbre: per note, Goertzel at k*f0 across a mid-sustain
    // window (past the attack transient, before the release), normalized
    // per-note so loud and quiet notes vote equally.
    struct Measured {
        uint8_t pitch;
        float weight; // note length — longer notes are more trustworthy
        std::array<float, kNumHarmonics> h;
    };
    std::vector<Measured> measured;

    for (const FlatNote& fn : candidates) {
        float f0 = tuningA4 * std::pow(2.0f, (static_cast<float>(fn.pitch) - 69.0f) / 12.0f);
        if (f0 * 2.0f >= sr * 0.5f) continue; // need at least 2 harmonics below Nyquist

        float winStartSec = fn.startSec + std::min(0.05f, fn.lenSec * 0.25f);
        float winLenSec = std::min(0.4f, fn.lenSec * 0.6f);
        size_t start = static_cast<size_t>(winStartSec * sr);
        size_t len = static_cast<size_t>(winLenSec * sr);
        if (len < 32) continue;

        Measured m{};
        m.pitch = fn.pitch;
        m.weight = fn.lenSec;
        float peak = 0.0f;
        for (int k = 0; k < kNumHarmonics; ++k) {
            float fk = f0 * static_cast<float>(k + 1);
            m.h[k] = (fk < sr * 0.45f) ? GoertzelMagnitude(mono, start, len, fk, sr) : 0.0f;
            peak = std::max(peak, m.h[k]);
        }
        if (peak <= 0.0f) continue;
        for (float& v : m.h) v /= peak;
        measured.push_back(m);
    }
    if (measured.empty()) {
        std::cerr << "TimbreAnalyzer: no note produced measurable harmonic energy\n";
        return std::nullopt;
    }

    // --- Collapse measurements into timbre keyframes. Real instruments get
    // darker up the keyboard, so if the notes span enough range, emit one
    // keyframe per pitch band and let the engine's keyframe interpolation do
    // the rest; a narrow range gets a single keyframe.
    uint8_t minPitch = 127, maxPitch = 0;
    for (const auto& m : measured) {
        minPitch = std::min(minPitch, m.pitch);
        maxPitch = std::max(maxPitch, m.pitch);
    }
    int bandCount = (maxPitch - minPitch >= 24) ? 3 : (maxPitch - minPitch >= 12) ? 2 : 1;

    Patch patch;
    patch.timbreKeyframes.clear();
    for (int band = 0; band < bandCount; ++band) {
        float lo = minPitch + (maxPitch - minPitch) * static_cast<float>(band) / bandCount;
        float hi = minPitch + (maxPitch - minPitch) * static_cast<float>(band + 1) / bandCount;
        std::array<float, kNumHarmonics> acc{};
        float wSum = 0.0f, pitchSum = 0.0f;
        for (const auto& m : measured) {
            float p = static_cast<float>(m.pitch);
            bool inBand = (p >= lo && (p < hi || band == bandCount - 1));
            if (!inBand) continue;
            for (int k = 0; k < kNumHarmonics; ++k) acc[k] += m.h[k] * m.weight;
            wSum += m.weight;
            pitchSum += p * m.weight;
        }
        if (wSum <= 0.0f) continue;
        TimbreKeyframe kf;
        kf.midiNote = static_cast<uint8_t>(std::clamp(pitchSum / wSum, 0.0f, 127.0f));
        kf.harmonics.assign(kNumHarmonics, 0.0f);
        float peak = 0.0f;
        for (int k = 0; k < kNumHarmonics; ++k) peak = std::max(peak, acc[k]);
        for (int k = 0; k < kNumHarmonics; ++k)
            kf.harmonics[k] = peak > 0.0f ? acc[k] / peak : (k == 0 ? 1.0f : 0.0f);
        patch.timbreKeyframes.push_back(kf);
    }
    if (patch.timbreKeyframes.empty()) {
        std::cerr << "TimbreAnalyzer: keyframe aggregation produced nothing\n";
        return std::nullopt;
    }

    // --- ADSR from the mix's RMS envelope around note boundaries. Medians
    // across notes keep one drum hit or delay repeat from skewing anything.
    const size_t hop = kEngineSampleRate / 200; // 5 ms
    std::vector<float> rms(mono.size() / hop + 1, 0.0f);
    {
        double acc = 0.0;
        size_t count = 0, out = 0;
        for (size_t i = 0; i < mono.size(); ++i) {
            acc += static_cast<double>(mono[i]) * mono[i];
            if (++count == hop) {
                rms[out++] = static_cast<float>(std::sqrt(acc / hop));
                acc = 0.0;
                count = 0;
            }
        }
    }
    auto rmsAt = [&](float sec) -> float {
        size_t idx = static_cast<size_t>(sec * sr / hop);
        return idx < rms.size() ? rms[idx] : 0.0f;
    };

    std::vector<float> attacks, sustains, releases;
    for (const FlatNote& fn : candidates) {
        // Attack: onset -> local RMS peak within the first 500 ms of the note.
        float searchEnd = std::min(fn.lenSec, 0.5f);
        float peakLevel = 0.0f, peakT = 0.0f;
        for (float t = 0.0f; t <= searchEnd; t += 0.005f) {
            float v = rmsAt(fn.startSec + t);
            if (v > peakLevel) { peakLevel = v; peakT = t; }
        }
        if (peakLevel <= 0.0f) continue;
        attacks.push_back(peakT * 1000.0f);

        // Sustain: level just before note-off relative to the attack peak.
        float endLevel = rmsAt(fn.startSec + fn.lenSec * 0.9f);
        sustains.push_back(std::clamp(endLevel / peakLevel, 0.0f, 1.0f));

        // Release: time after note-off for the mix to fall to 1/e of the
        // end level (capped at 2 s — beyond that it's the reverb, not the amp
        // envelope, and we're deliberately not cloning the effects).
        float target = endLevel * 0.368f;
        for (float t = 0.005f; t <= 2.0f; t += 0.005f) {
            if (rmsAt(fn.startSec + fn.lenSec + t) <= target) {
                releases.push_back(t * 1000.0f);
                break;
            }
        }
    }

    patch.attackMs = std::clamp(MedianOf(attacks, 30.0f), 1.0f, 1500.0f);
    patch.sustainLevel = std::clamp(MedianOf(sustains, 0.7f), 0.0f, 1.0f);
    patch.releaseMs = std::clamp(MedianOf(releases, 250.0f), 20.0f, 2500.0f);
    // Decay isn't separately observable in a dense mix (it blurs into the
    // sustain estimate), so use a musically neutral constant.
    patch.decayMs = 150.0f;

    patch.attackTable = BuildCurve(0.0f, 1.0f, patch.attackMs, patch.attackMs > 300.0f ? 1.6f : 0.8f); // long attack = pad swell shape
    patch.decayTable = BuildCurve(1.0f, patch.sustainLevel, patch.decayMs, 2.0f);
    patch.releaseTable = BuildCurve(patch.sustainLevel, 0.0f, patch.releaseMs, 1.8f);

    // Name from the audio file: "Analyzed: HOME - Resonance"
    size_t slash = audioPath.find_last_of("\\/");
    std::string stem = slash == std::string::npos ? audioPath : audioPath.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    patch.name = "Analyzed: " + stem;

    return patch;
}

} // namespace TimbreAnalyzer

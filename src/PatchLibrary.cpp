#include "AudioData.h"

#include <cmath>

// Factory patches for the C418 (ambient/modern-classical) and STAKILLAZ
// (trashwave/phonk/hardstyle) suites. Everything here runs on the Main
// Thread; returned patches carry ready-to-play envelope tables built at
// kEngineSampleRate so they never depend on the ImGui Bezier editor state.

namespace {

// Simple exponential-ish curve tables matching the engine's semantics:
// attack 0->1, decay 1->sustain, release sustain->0.
std::vector<float> buildCurve(float fromLevel, float toLevel, float timeMs, float shape) {
    size_t samples = static_cast<size_t>((timeMs / 1000.0f) * kEngineSampleRate);
    if (samples == 0) samples = 1;
    std::vector<float> table(samples);
    for (size_t i = 0; i < samples; ++i) {
        float t = samples > 1 ? static_cast<float>(i) / static_cast<float>(samples - 1) : 1.0f;
        // shape < 1 = fast start (percussive), shape > 1 = slow start (swell)
        float curved = std::pow(t, shape);
        table[i] = fromLevel + (toLevel - fromLevel) * curved;
    }
    return table;
}

void buildEnvelopeTables(Patch& p, float attackShape, float decayShape, float releaseShape) {
    p.attackTable = buildCurve(0.0f, 1.0f, p.attackMs, attackShape);
    p.decayTable = buildCurve(1.0f, p.sustainLevel, p.decayMs, decayShape);
    p.releaseTable = buildCurve(p.sustainLevel, 0.0f, p.releaseMs, releaseShape);
}

void setHarmonics(Patch& p, std::initializer_list<float> amps) {
    TimbreKeyframe kf;
    kf.midiNote = 60;
    kf.harmonics.assign(16, 0.0f);
    size_t i = 0;
    for (float a : amps) {
        if (i >= 16) break;
        kf.harmonics[i++] = a;
    }
    p.timbreKeyframes.clear();
    p.timbreKeyframes.push_back(kf);
}

void setEnvelope(Patch& p, float attackMs, float decayMs, float sustain, float releaseMs) {
    p.attackMs = attackMs;
    p.decayMs = decayMs;
    p.sustainLevel = sustain;
    p.releaseMs = releaseMs;
}

} // namespace

namespace PatchLibrary {

const std::vector<std::string>& Names() {
    static const std::vector<std::string> names = {
        // C418 suite
        "Kalimba", "Marimba", "Giant Piano", "Warm Pad", "Pizzicato",
        // STAKILLAZ suite
        "Hardstyle Kick", "808 Sub", "Screech Lead", "Phonk Bass",
    };
    return names;
}

Patch Create(const std::string& name) {
    Patch p;
    p.name = name;

    if (name == "Kalimba") {
        // "Aria Math" core: strong fundamental, scooped 2nd, metallic 4th/8th.
        setHarmonics(p, {1.0f, 0.2f, 0.0f, 0.4f, 0.0f, 0.0f, 0.0f, 0.4f});
        setEnvelope(p, 5.0f, 300.0f, 0.0f, 150.0f);
        buildEnvelopeTables(p, 0.8f, 2.0f, 2.0f);
        p.filterCutoffHz = 7000.0f; // soften the metallic top
    } else if (name == "Marimba") {
        // Wooden bars: fundamental + the classic "double octave" 4th partial.
        setHarmonics(p, {1.0f, 0.05f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.1f});
        setEnvelope(p, 3.0f, 250.0f, 0.0f, 120.0f);
        buildEnvelopeTables(p, 0.8f, 2.2f, 2.0f);
    } else if (name == "Giant Piano") {
        // NI "The Giant" character: 1/n rolloff, slight odd emphasis, long decay.
        setHarmonics(p, {1.0f, 0.5f, 0.38f, 0.25f, 0.24f, 0.17f, 0.17f, 0.12f});
        setEnvelope(p, 8.0f, 900.0f, 0.25f, 400.0f);
        buildEnvelopeTables(p, 0.7f, 2.5f, 2.2f);
        p.filterCutoffHz = 9000.0f;
    } else if (name == "Warm Pad") {
        // "Dreiton" backdrop: full 1/n stack, slow swell, evolving LPF.
        setHarmonics(p, {1.0f, 0.5f, 0.33f, 0.25f, 0.2f, 0.17f, 0.14f, 0.13f,
                         0.11f, 0.1f, 0.09f, 0.08f, 0.08f, 0.07f, 0.07f, 0.06f});
        setEnvelope(p, 1200.0f, 1.0f, 1.0f, 2500.0f);
        buildEnvelopeTables(p, 1.6f, 1.0f, 1.8f);
        p.filterCutoffHz = 2500.0f;
        p.filterLfoRateHz = 0.1f;
        p.filterLfoDepth = 0.4f;
    } else if (name == "Pizzicato") {
        // Kontakt pizzicato double bass: fundamental + 2nd, instant pluck.
        setHarmonics(p, {1.0f, 0.6f, 0.15f, 0.05f});
        setEnvelope(p, 2.0f, 100.0f, 0.0f, 80.0f);
        buildEnvelopeTables(p, 0.7f, 2.5f, 2.0f);
        p.filterCutoffHz = 4000.0f;
    } else if (name == "Hardstyle Kick") {
        // Distorted pitched kick: dense low harmonics, hard pitch drop, sub weld.
        setHarmonics(p, {1.0f, 0.7f, 0.5f, 0.4f, 0.3f, 0.25f, 0.2f, 0.15f});
        setEnvelope(p, 1.0f, 350.0f, 0.0f, 60.0f);
        buildEnvelopeTables(p, 0.6f, 1.8f, 2.0f);
        p.drive = 12.0f;
        p.subOscLevel = 0.9f;
        p.subOscWave = 0;
        p.pitchDropSemitones = 36.0f;
        p.pitchDropMs = 45.0f;
    } else if (name == "808 Sub") {
        // Boomy sine sub with a softer, slower drop and a long tail.
        setHarmonics(p, {1.0f});
        setEnvelope(p, 2.0f, 600.0f, 0.4f, 400.0f);
        buildEnvelopeTables(p, 0.7f, 2.0f, 2.0f);
        p.subOscLevel = 1.0f;
        p.subOscWave = 0;
        p.pitchDropSemitones = 12.0f;
        p.pitchDropMs = 60.0f;
        p.drive = 4.0f;
    } else if (name == "Screech Lead") {
        // Abrasive odd-harmonic stack driven hard — the trashwave lead.
        setHarmonics(p, {1.0f, 0.0f, 0.8f, 0.0f, 0.6f, 0.0f, 0.5f, 0.0f,
                         0.4f, 0.0f, 0.3f, 0.0f, 0.25f, 0.0f, 0.2f, 0.0f});
        setEnvelope(p, 10.0f, 80.0f, 0.8f, 120.0f);
        buildEnvelopeTables(p, 0.8f, 1.5f, 2.0f);
        p.drive = 18.0f;
        p.filterCutoffHz = 12000.0f;
        p.filterLfoRateHz = 4.0f;
        p.filterLfoDepth = 0.25f;
    } else if (name == "Phonk Bass") {
        // Dark low-mid bass, moderate saturation, rolled-off top.
        setHarmonics(p, {1.0f, 0.8f, 0.4f, 0.3f, 0.15f, 0.1f});
        setEnvelope(p, 4.0f, 200.0f, 0.6f, 150.0f);
        buildEnvelopeTables(p, 0.7f, 1.8f, 2.0f);
        p.drive = 6.0f;
        p.filterCutoffHz = 1800.0f;
        p.subOscLevel = 0.5f;
        p.subOscWave = 1;
    } else {
        // Unknown name: plain sine so callers always get something playable.
        setHarmonics(p, {1.0f});
        setEnvelope(p, 10.0f, 100.0f, 0.7f, 200.0f);
        buildEnvelopeTables(p, 1.0f, 1.5f, 1.8f);
    }

    return p;
}

} // namespace PatchLibrary

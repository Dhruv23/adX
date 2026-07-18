#include "AudioEffect.h"
#include "AudioData.h"

#include <cmath>

bool Track::effectsEquivalent(const Track& other) const {
    if (effects.size() != other.effects.size()) return false;
    for (size_t i = 0; i < effects.size(); ++i) {
        if (!effects[i] || !other.effects[i]) {
            if (static_cast<bool>(effects[i]) != static_cast<bool>(other.effects[i])) return false;
            continue;
        }
        if (!effects[i]->isEquivalent(*other.effects[i])) return false;
    }
    return true;
}

// Classic Freeverb tunings (samples @ 44.1kHz, matching kEngineSampleRate).
namespace {
constexpr size_t kCombTunings[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr size_t kAllpassTunings[4] = {556, 441, 341, 225};
constexpr size_t kStereoSpread = 23;
constexpr float kFixedGain = 0.015f;      // input scaling into the comb bank
constexpr float kAllpassFeedback = 0.5f;
constexpr float kWetScale = 3.0f;         // Freeverb's wet output scaling
}

ReverbEffect::ReverbEffect(float mixIn, float roomSizeIn, float dampingIn)
    : mix(mixIn), roomSize(roomSizeIn), damping(dampingIn) {
    for (size_t i = 0; i < kNumCombs; ++i) {
        m_combL[i].buffer.assign(kCombTunings[i], 0.0f);
        m_combR[i].buffer.assign(kCombTunings[i] + kStereoSpread, 0.0f);
    }
    for (size_t i = 0; i < kNumAllpasses; ++i) {
        m_allpassL[i].buffer.assign(kAllpassTunings[i], 0.0f);
        m_allpassR[i].buffer.assign(kAllpassTunings[i] + kStereoSpread, 0.0f);
    }
}

void ReverbEffect::processSample(float& left, float& right) {
    float mixNow = mix.load(std::memory_order_relaxed);
    float feedback = roomSize.load(std::memory_order_relaxed) * 0.28f + 0.7f;
    float damp = damping.load(std::memory_order_relaxed) * 0.4f;

    float input = (left + right) * kFixedGain;

    auto runComb = [&](Comb& c) {
        float output = c.buffer[c.index];
        c.filterStore = output * (1.0f - damp) + c.filterStore * damp;
        c.buffer[c.index] = input + c.filterStore * feedback;
        if (++c.index >= c.buffer.size()) c.index = 0;
        return output;
    };
    auto runAllpass = [&](Allpass& a, float in) {
        float bufOut = a.buffer[a.index];
        float output = bufOut - in;
        a.buffer[a.index] = in + bufOut * kAllpassFeedback;
        if (++a.index >= a.buffer.size()) a.index = 0;
        return output;
    };

    float outL = 0.0f;
    float outR = 0.0f;
    for (size_t i = 0; i < kNumCombs; ++i) {
        outL += runComb(m_combL[i]);
        outR += runComb(m_combR[i]);
    }
    for (size_t i = 0; i < kNumAllpasses; ++i) {
        outL = runAllpass(m_allpassL[i], outL);
        outR = runAllpass(m_allpassR[i], outR);
    }

    left = left * (1.0f - mixNow) + outL * kWetScale * mixNow;
    right = right * (1.0f - mixNow) + outR * kWetScale * mixNow;
}

bool ReverbEffect::isEquivalent(const AudioEffect& other) const {
    const auto* o = dynamic_cast<const ReverbEffect*>(&other);
    return o && o->mix.load() == mix.load() && o->roomSize.load() == roomSize.load() &&
           o->damping.load() == damping.load();
}

DistortionEffect::DistortionEffect(float driveIn, float mixIn)
    : drive(driveIn), mix(mixIn) {}

void DistortionEffect::processSample(float& left, float& right) {
    float driveNow = drive.load(std::memory_order_relaxed);
    float mixNow = mix.load(std::memory_order_relaxed);
    if (driveNow < 1.0f) driveNow = 1.0f;

    // Normalize by tanh(drive) so cranking drive changes the shape (harder
    // clipping, more overtones) without also exploding the output level.
    float norm = std::tanh(driveNow);
    float wetL = std::tanh(left * driveNow) / norm;
    float wetR = std::tanh(right * driveNow) / norm;

    left = left * (1.0f - mixNow) + wetL * mixNow;
    right = right * (1.0f - mixNow) + wetR * mixNow;
}

bool DistortionEffect::isEquivalent(const AudioEffect& other) const {
    const auto* o = dynamic_cast<const DistortionEffect*>(&other);
    return o && o->drive.load() == drive.load() && o->mix.load() == mix.load();
}

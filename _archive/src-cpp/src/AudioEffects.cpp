#include "AudioEffect.h"
#include "AudioData.h"

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

void ReverbEffect::runCombsAndAllpasses(float inputLeft, float inputRight, float& outLeft, float& outRight) {
    float feedback = roomSize.load(std::memory_order_relaxed) * 0.28f + 0.7f;
    float damp = damping.load(std::memory_order_relaxed) * 0.4f;

    float input = (inputLeft + inputRight) * kFixedGain;

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

    outLeft = outL * kWetScale;
    outRight = outR * kWetScale;
}

void ReverbEffect::processSample(float& left, float& right) {
    float mixNow = mix.load(std::memory_order_relaxed);
    float wetL, wetR;
    runCombsAndAllpasses(left, right, wetL, wetR);
    left = left * (1.0f - mixNow) + wetL * mixNow;
    right = right * (1.0f - mixNow) + wetR * mixNow;
}

void ReverbEffect::processWetOnly(float inputLeft, float inputRight, float& outLeft, float& outRight) {
    runCombsAndAllpasses(inputLeft, inputRight, outLeft, outRight);
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

BitcrushEffect::BitcrushEffect(float bitDepthIn, float rateHzIn, float mixIn)
    : bitDepth(bitDepthIn), rateHz(rateHzIn), mix(mixIn) {}

void BitcrushEffect::processSample(float& left, float& right) {
    float mixNow = mix.load(std::memory_order_relaxed);
    float rate = std::clamp(rateHz.load(std::memory_order_relaxed), 100.0f, static_cast<float>(kEngineSampleRate));
    float bits = std::clamp(bitDepth.load(std::memory_order_relaxed), 1.0f, 16.0f);

    // Sample-and-hold decimation: only refresh the held sample once per
    // (kEngineSampleRate / rate) input samples. Stepping a fractional phase
    // (rather than an integer counter) keeps non-integer ratios like
    // 44100/12000 from drifting over a long render.
    m_phase += rate / static_cast<float>(kEngineSampleRate);
    if (m_phase >= 1.0f) {
        m_phase -= 1.0f;
        m_heldL = left;
        m_heldR = right;
    }

    float levels = std::pow(2.0f, bits);
    float quantL = std::round(m_heldL * levels) / levels;
    float quantR = std::round(m_heldR * levels) / levels;

    left = left * (1.0f - mixNow) + quantL * mixNow;
    right = right * (1.0f - mixNow) + quantR * mixNow;
}

bool BitcrushEffect::isEquivalent(const AudioEffect& other) const {
    const auto* o = dynamic_cast<const BitcrushEffect*>(&other);
    return o && o->bitDepth.load() == bitDepth.load() && o->rateHz.load() == rateHz.load() &&
           o->mix.load() == mix.load();
}

ChorusEffect::ChorusEffect(float rateHzIn, float depthIn, float mixIn)
    : rateHz(rateHzIn), depth(depthIn), mix(mixIn) {
    // 50ms is comfortably more than the ~13ms max modulated delay below,
    // leaving headroom for the linear-interpolation read to never wrap past
    // the write point.
    size_t bufSize = static_cast<size_t>(0.05f * kEngineSampleRate) + 4;
    m_bufferL.assign(bufSize, 0.0f);
    m_bufferR.assign(bufSize, 0.0f);
}

void ChorusEffect::processSample(float& left, float& right) {
    float mixNow = mix.load(std::memory_order_relaxed);
    float rate = std::max(0.01f, rateHz.load(std::memory_order_relaxed));
    float depthNow = std::clamp(depth.load(std::memory_order_relaxed), 0.0f, 1.0f);

    size_t bufSize = m_bufferL.size();
    m_bufferL[m_writeIndex] = left;
    m_bufferR[m_writeIndex] = right;

    // Base delay ~7ms, modulated +/-6ms by depth — the classic chorus range.
    // Right channel's LFO is offset a quarter-cycle (quadrature) from the
    // left so the two channels diverge instead of just tremolo-ing together.
    constexpr float kBaseDelayMs = 7.0f;
    constexpr float kModDepthMs = 6.0f;
    float lfoL = std::sin(m_lfoPhase * 2.0f * static_cast<float>(M_PI));
    float lfoR = std::sin((m_lfoPhase + 0.25f) * 2.0f * static_cast<float>(M_PI));

    auto readDelayed = [&](const std::vector<float>& buf, float lfo) {
        float delayMs = kBaseDelayMs + lfo * kModDepthMs * depthNow;
        float delaySamples = std::clamp((delayMs / 1000.0f) * kEngineSampleRate, 1.0f, static_cast<float>(bufSize - 2));
        float readPos = static_cast<float>(m_writeIndex) - delaySamples;
        while (readPos < 0.0f) readPos += static_cast<float>(bufSize);
        size_t i0 = static_cast<size_t>(readPos);
        size_t i1 = (i0 + 1) % bufSize;
        float frac = readPos - static_cast<float>(i0);
        return buf[i0] * (1.0f - frac) + buf[i1] * frac;
    };

    float wetL = readDelayed(m_bufferL, lfoL);
    float wetR = readDelayed(m_bufferR, lfoR);

    if (++m_writeIndex >= bufSize) m_writeIndex = 0;
    m_lfoPhase += rate / static_cast<float>(kEngineSampleRate);
    if (m_lfoPhase >= 1.0f) m_lfoPhase -= 1.0f;

    left = left * (1.0f - mixNow) + wetL * mixNow;
    right = right * (1.0f - mixNow) + wetR * mixNow;
}

bool ChorusEffect::isEquivalent(const AudioEffect& other) const {
    const auto* o = dynamic_cast<const ChorusEffect*>(&other);
    return o && o->rateHz.load() == rateHz.load() && o->depth.load() == depth.load() &&
           o->mix.load() == mix.load();
}

// RBJ Audio EQ Cookbook biquad coefficients (shelf slope S = 1 throughout).
namespace {
struct BiquadCoeffs { float b0, b1, b2, a1, a2; };

BiquadCoeffs MakeLowShelf(float gainDb, float freqHz) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * static_cast<float>(M_PI) * freqHz / static_cast<float>(kEngineSampleRate);
    float cosw0 = std::cos(w0);
    float alpha = std::sin(w0) * 0.5f * std::sqrt(2.0f);
    float sqrtA = std::sqrt(A);

    float b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha);
    float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float a2 = (A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

BiquadCoeffs MakeHighShelf(float gainDb, float freqHz) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * static_cast<float>(M_PI) * freqHz / static_cast<float>(kEngineSampleRate);
    float cosw0 = std::cos(w0);
    float alpha = std::sin(w0) * 0.5f * std::sqrt(2.0f);
    float sqrtA = std::sqrt(A);

    float b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha);
    float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float a2 = (A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

BiquadCoeffs MakePeaking(float gainDb, float freqHz, float q) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * static_cast<float>(M_PI) * freqHz / static_cast<float>(kEngineSampleRate);
    float cosw0 = std::cos(w0);
    float alpha = std::sin(w0) / (2.0f * q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cosw0;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cosw0;
    float a2 = 1.0f - alpha / A;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// Direct Form I. state = {x1, x2, y1, y2}.
float RunBiquad(const BiquadCoeffs& c, float x, std::array<float, 4>& state) {
    float y = c.b0 * x + c.b1 * state[0] + c.b2 * state[1] - c.a1 * state[2] - c.a2 * state[3];
    state[1] = state[0]; state[0] = x;
    state[3] = state[2]; state[2] = y;
    return y;
}

// Fixed band-split points — the .adx EQ key only stores the three gains,
// not corner frequencies (see the format reference table in plan.md).
constexpr float kEqLowFreqHz = 250.0f;
constexpr float kEqMidFreqHz = 1200.0f;
constexpr float kEqMidQ = 0.7f;
constexpr float kEqHighFreqHz = 4000.0f;
} // namespace

EQEffect::EQEffect(float lowGainDbIn, float midGainDbIn, float highGainDbIn)
    : lowGainDb(lowGainDbIn), midGainDb(midGainDbIn), highGainDb(highGainDbIn) {}

void EQEffect::processSample(float& left, float& right) {
    // Coefficients are cheap to recompute every sample (a handful of trig
    // calls) and this keeps automated gain changes glitch-free without a
    // separate change-detection cache — the same tradeoff the per-voice
    // resonant filter (AudioEngine.cpp) already makes.
    BiquadCoeffs low = MakeLowShelf(lowGainDb.load(std::memory_order_relaxed), kEqLowFreqHz);
    BiquadCoeffs mid = MakePeaking(midGainDb.load(std::memory_order_relaxed), kEqMidFreqHz, kEqMidQ);
    BiquadCoeffs high = MakeHighShelf(highGainDb.load(std::memory_order_relaxed), kEqHighFreqHz);

    left = RunBiquad(low, left, m_stateL[0]);
    left = RunBiquad(mid, left, m_stateL[1]);
    left = RunBiquad(high, left, m_stateL[2]);

    right = RunBiquad(low, right, m_stateR[0]);
    right = RunBiquad(mid, right, m_stateR[1]);
    right = RunBiquad(high, right, m_stateR[2]);
}

bool EQEffect::isEquivalent(const AudioEffect& other) const {
    const auto* o = dynamic_cast<const EQEffect*>(&other);
    return o && o->lowGainDb.load() == lowGainDb.load() && o->midGainDb.load() == midGainDb.load() &&
           o->highGainDb.load() == highGainDb.load();
}

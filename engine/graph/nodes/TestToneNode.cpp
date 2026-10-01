#include "engine/graph/nodes/TestToneNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace adx::graph {
namespace {

constexpr float kTwoPi = 6.28318530717958647692F;

// 2^(k/12), written out. The corpus renders at pitch offsets of zero, and with this
// table a note's frequency is 440 times a literal times an exact power of two - no
// library function in the path at all, so no build configuration can disagree.
constexpr std::array<double, 12> kSemitoneRatios{
    1.0,
    1.0594630943592953,
    1.122462048309373,
    1.189207115002721,
    1.2599210498948732,
    1.3348398541700344,
    std::numbers::sqrt2,
    1.4983070768766815,
    1.5874010519681994,
    1.681792830507429,
    1.7817974362806785,
    1.8877486253633868,
};

// Voice state slots. Named so the arithmetic below reads as an envelope and an
// oscillator rather than as array indices.
constexpr std::size_t kPhase = 0;
constexpr std::size_t kEnvelope = 1;
constexpr std::size_t kAttackStep = 2;
constexpr std::size_t kReleaseStep = 3;

} // namespace

float sineOfTurns(float turns) noexcept {
    // Reduce to a quarter wave by symmetry, then an odd Taylor polynomial to z^11:
    // on [0, pi/2] the next term is below 6e-8, i.e. below float resolution.
    float x = turns - std::floor(turns);
    float sign = 1.0F;
    if (x >= 0.5F) {
        x -= 0.5F;
        sign = -1.0F;
    }
    if (x > 0.25F) {
        x = 0.5F - x;
    }
    const float z = x * kTwoPi;
    const float z2 = z * z;
    const float poly =
        1.0F + (z2 * (-1.0F / 6.0F +
                      (z2 * (1.0F / 120.0F +
                             (z2 * (-1.0F / 5040.0F +
                                    (z2 * (1.0F / 362880.0F + (z2 * (-1.0F / 39916800.0F))))))))));
    return sign * z * poly;
}

double testToneFrequency(std::uint8_t pitch, double cents) noexcept {
    const int semitones = static_cast<int>(pitch) - 69;
    // Floor division, so -1 is octave -1 step 11 rather than octave 0 step -1.
    const int octave = semitones >= 0 ? semitones / 12 : -((11 - semitones) / 12);
    const int step = semitones - (octave * 12);
    double frequency = 440.0 * kSemitoneRatios[static_cast<std::size_t>(step)];
    frequency = std::ldexp(frequency, octave);
    if (cents != 0.0) {
        frequency *= std::exp2(cents / 1200.0);
    }
    return frequency;
}

void TestToneNode::startVoice(Voice& voice, const BlockEvent& /*event*/,
                              std::uint32_t sampleRate) noexcept {
    const double attackSamples = kTestToneAttackSeconds * static_cast<double>(sampleRate);
    // Cosine phase: the note's first sample is attackStep * amplitude, never zero, so
    // an onset test can find the exact sample a note started on. The attack ramp is
    // what keeps that from being a click.
    voice.state[kPhase] = 0.25F;
    voice.state[kEnvelope] = 0.0F;
    voice.state[kAttackStep] =
        static_cast<float>(1.0 / (attackSamples < 1.0 ? 1.0 : attackSamples));
    voice.state[kReleaseStep] = 0.0F;
    voice.level = 0.0F;
}

bool TestToneNode::renderVoice(Voice& voice, std::span<float> left, std::span<float> right,
                               std::uint32_t sampleRate, float pitchCents) noexcept {
    const auto increment = static_cast<float>(testToneFrequency(voice.pitch, pitchCents) /
                                              static_cast<double>(sampleRate));
    const float amplitude = kTestToneAmplitude * (static_cast<float>(voice.velocity) / 127.0F);

    float phase = voice.state[kPhase];
    float envelope = voice.state[kEnvelope];
    const float attackStep = voice.state[kAttackStep];

    const bool releasing = voice.phase == VoicePhase::Released;
    if (releasing && voice.state[kReleaseStep] == 0.0F) {
        // The release starts from wherever the envelope is, and always takes the same
        // time to reach zero - a note released mid-attack does not click.
        const double releaseSamples = kTestToneReleaseSeconds * static_cast<double>(sampleRate);
        voice.state[kReleaseStep] =
            static_cast<float>(static_cast<double>(envelope) / releaseSamples);
        if (voice.state[kReleaseStep] <= 0.0F) {
            voice.state[kReleaseStep] = 1.0F; // already silent: finish on the next sample
        }
    }
    const float releaseStep = voice.state[kReleaseStep];

    bool alive = true;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        if (releasing) {
            envelope -= releaseStep;
            if (envelope <= 0.0F) {
                envelope = 0.0F;
                alive = false;
            }
        } else if (envelope < 1.0F) {
            envelope = std::min(envelope + attackStep, 1.0F);
        }
        const float sample = sineOfTurns(phase) * envelope * amplitude;
        left[i] = sample;
        right[i] = sample;
        phase += increment;
        if (phase >= 1.0F) {
            phase -= 1.0F;
        }
    }

    voice.state[kPhase] = phase;
    voice.state[kEnvelope] = envelope;
    voice.level = envelope * (static_cast<float>(voice.velocity) / 127.0F);
    return alive;
}

} // namespace adx::graph

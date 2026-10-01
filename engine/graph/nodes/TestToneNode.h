// The only instrument Phase 3 ships: a sine with a linear attack-release envelope.
//
// It exists so the graph can be validated end to end before Phase 4 writes a real
// synth, and it stays permanently as the instrument the scheduler's sample-accuracy
// tests use, because its output is analytically predictable and no real synth's is
// (phase_3.md §4.9). A note's onset is the first non-zero sample, exactly; its pitch
// is exactly 440 * 2^((p-69)/12).
//
// The sine is a polynomial, not std::sin. The golden-hash corpus has to produce the
// same bits in Debug, RelWithDebInfo and Release, and a library sin is free to be a
// different function in each - an intrinsic in one, a CRT call in another, a
// vectorised approximation in a third. Multiplies and adds under /fp:precise are not.
#pragma once

#include <cstdint>
#include <span>

#include "engine/graph/nodes/ChannelNode.h"

namespace adx::graph {

/// Envelope times. Short on purpose: the attack is long enough not to click and short
/// enough that an onset test can find the first sample; the release is long enough
/// that "released, not cut" is measurable.
inline constexpr double kTestToneAttackSeconds = 0.005;
inline constexpr double kTestToneReleaseSeconds = 0.050;

/// Peak amplitude of one voice at velocity 127. Well under 1 so a few hundred
/// channels summed into one master do not clip the golden renders into saturation.
inline constexpr float kTestToneAmplitude = 0.2F;

/// sin(2 pi turns), for turns in [0, 1). A 9th-order odd polynomial after reducing to
/// a quarter wave; error below 1e-6, which is well below the 24-bit noise floor.
[[nodiscard]] float sineOfTurns(float turns) noexcept;

/// The equal-tempered frequency of MIDI note `pitch`, detuned by `cents`.
[[nodiscard]] double testToneFrequency(std::uint8_t pitch, double cents) noexcept;

class TestToneNode final : public ChannelNode {
public:
    using ChannelNode::ChannelNode;

protected:
    void startVoice(Voice& voice, const BlockEvent& event,
                    std::uint32_t sampleRate) noexcept override;
    bool renderVoice(Voice& voice, std::span<float> left, std::span<float> right,
                     std::uint32_t sampleRate, float pitchCents) noexcept override;
};

} // namespace adx::graph

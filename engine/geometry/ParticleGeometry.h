// adx-thread: main
//
// The transient particle fountain (phase_10.md §4.4), ported from
// _archive/src-cpp/src/ParticleVisualizer.cpp: bursts fired on transients, drawn as
// small squares that arc up and fade from young to old.
//
//   onsets    the RMS of the newest 1024 frames against an envelope follower that
//             decays by 0.9 a frame: a jump of 1.5x above 0.02 is a burst whose
//             strength sets how many particles (8..40) and how fast
//   pool      512 particles, preallocated, recycled round robin: when it is full the
//             oldest particle is taken, so the pool never grows (particles_pool_bounded)
//   world     x, y in [0, 1], the fountain's mouth at (0.5, 1)
//   layer     0: the particles (triangles)
//
// What changed on port: the pool and the follower are members rather than file
// statics, so two panels can each have one; the random source is seeded, so a test can
// replay a burst; and the draw is geometry for the scene graph instead of ImGui calls.
#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <span>

#include "engine/geometry/GeometryBuffer.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::geometry {

inline constexpr std::size_t kMaxParticles = 512;

class ParticleGeometry {
public:
    explicit ParticleGeometry(std::uint32_t seed = 1) : m_random(seed) {}

    /// Advances `dtSeconds` (clamped to 0.1) with `history` the newest frames.
    void build(std::span<const rt::StereoFrame> history, double dtSeconds);

    [[nodiscard]] GeometryBuffer& particles() noexcept {
        return m_vertices;
    }
    [[nodiscard]] std::size_t alive() const noexcept;
    [[nodiscard]] std::uint64_t bursts() const noexcept {
        return m_bursts;
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return kMaxParticles;
    }
    /// Fires a burst of `strength` 0..1, as a transient would.
    void burst(float strength);

private:
    struct Particle {
        bool alive{false};
        float x{0.5F};
        float y{1.0F};
        float vx{0.0F};
        float vy{0.0F};
        float age{0.0F};
        float lifetime{1.0F};
    };

    float uniform(float lo, float hi);

    std::array<Particle, kMaxParticles> m_pool{};
    std::size_t m_next{0};
    float m_envelope{0.0F};
    std::uint64_t m_bursts{0};
    std::mt19937 m_random;
    GeometryBuffer m_vertices;
};

} // namespace adx::geometry

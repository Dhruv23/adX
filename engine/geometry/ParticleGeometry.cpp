// adx-thread: main
#include "engine/geometry/ParticleGeometry.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/Vertices.h"

namespace adx::geometry {

namespace {

constexpr std::size_t kRmsWindow = 1024;
constexpr float kThresholdRatio = 1.5F;
constexpr float kMinRms = 0.02F;
constexpr float kEnvelopeDecay = 0.90F;
constexpr float kGravity = 0.6F;

} // namespace

float ParticleGeometry::uniform(float lo, float hi) {
    std::uniform_real_distribution<float> distribution(lo, hi);
    return distribution(m_random);
}

std::size_t ParticleGeometry::alive() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(m_pool, [](const Particle& p) { return p.alive; }));
}

void ParticleGeometry::burst(float strength) {
    strength = std::clamp(strength, 0.0F, 1.0F);
    const auto count = static_cast<int>(std::lerp(8.0F, 40.0F, strength));
    for (int i = 0; i < count; ++i) {
        // Round robin: a full pool gives up its oldest slot, never grows.
        Particle& p = m_pool.at(m_next);
        m_next = (m_next + 1) % kMaxParticles;
        p.alive = true;
        p.x = 0.5F + uniform(-0.03F, 0.03F);
        p.y = 1.0F;
        const float speed = std::lerp(0.3F, 1.2F, strength) * uniform(0.7F, 1.3F);
        // Up (negative y) +-50 degrees: a fountain, not a jet.
        const float angle =
            (-90.0F + uniform(-50.0F, 50.0F)) * (std::numbers::pi_v<float> / 180.0F);
        p.vx = std::cos(angle) * speed;
        p.vy = std::sin(angle) * speed;
        p.age = 0.0F;
        p.lifetime = uniform(0.6F, 1.3F);
    }
    ++m_bursts;
}

void ParticleGeometry::build(std::span<const rt::StereoFrame> history, double dtSeconds) {
    const auto dt = static_cast<float>(std::clamp(dtSeconds, 0.0, 0.1));
    const std::size_t window = std::min(kRmsWindow, history.size());
    if (window > 0) {
        double sum = 0.0;
        for (const rt::StereoFrame& f : history.subspan(history.size() - window)) {
            const double mono = (static_cast<double>(f.left) + f.right) * 0.5;
            sum += mono * mono;
        }
        const auto rms = static_cast<float>(std::sqrt(sum / static_cast<double>(window)));
        if (rms > kMinRms && rms > m_envelope * kThresholdRatio) {
            burst((rms - m_envelope) / std::max(rms, 1e-4F));
        }
        m_envelope = std::max(rms, m_envelope * kEnvelopeDecay);
    }
    for (Particle& p : m_pool) {
        if (!p.alive) {
            continue;
        }
        p.age += dt;
        if (p.age >= p.lifetime) {
            p.alive = false;
            continue;
        }
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vy += kGravity * dt;
    }
    writeVertices(m_vertices, kMaxParticles * 6, [&](VertexWriter& w) {
        for (const Particle& p : m_pool) {
            if (!p.alive) {
                continue;
            }
            const float t = p.age / std::max(p.lifetime, 1e-4F);
            const float half = std::lerp(0.008F, 0.003F, t);
            w.quad(p.x - half, p.y - half, p.x + half, p.y + half,
                   t < 0.5F ? ColorRole::kParticleYoung : ColorRole::kParticleOld);
        }
    });
}

} // namespace adx::geometry

#include "ParticleVisualizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace ParticleVisualizer {

namespace {

constexpr size_t kMaxParticles = 512;
constexpr size_t kRmsWindow = 1024; // ~23ms at 44100Hz -- comfortably more than one UI frame's worth of new audio

struct Particle {
    bool alive = false;
    float x = 0.5f, y = 1.0f; // normalized position; origin is bottom-center (a "fountain")
    float vx = 0.0f, vy = 0.0f; // normalized units/second
    float age = 0.0f;
    float lifetime = 1.0f;
};

std::array<Particle, kMaxParticles> s_particles{};
size_t s_nextSlot = 0; // round-robin recycle cursor -- avoids an O(n) free-slot scan on every spawn

// Onset-detection envelope follower state (see Update()).
float s_envelope = 0.0f;

std::mt19937 s_rng{std::random_device{}()};

float RandRange(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(s_rng);
}

// Recycles the next slot (round-robin) for a fresh particle -- always
// succeeds, silently stealing the oldest slot if the pool is momentarily
// full rather than growing or dropping the spawn.
Particle& AllocateParticle() {
    Particle& p = s_particles[s_nextSlot];
    s_nextSlot = (s_nextSlot + 1) % kMaxParticles;
    return p;
}

void SpawnBurst(float strength) {
    strength = std::clamp(strength, 0.0f, 1.0f);
    int count = static_cast<int>(std::lerp(8.0f, 40.0f, strength));
    for (int i = 0; i < count; ++i) {
        Particle& p = AllocateParticle();
        p.alive = true;
        p.x = 0.5f + RandRange(-0.03f, 0.03f);
        p.y = 1.0f;
        float speed = std::lerp(0.3f, 1.2f, strength) * RandRange(0.7f, 1.3f);
        // Spread around straight "up" (screen-space: negative y), +/-50
        // degrees, so the burst reads as a fountain, not a single jet.
        float angleDeg = -90.0f + RandRange(-50.0f, 50.0f);
        float angleRad = angleDeg * (3.14159265f / 180.0f);
        p.vx = std::cos(angleRad) * speed;
        p.vy = std::sin(angleRad) * speed;
        p.age = 0.0f;
        p.lifetime = RandRange(0.6f, 1.3f);
    }
}

} // namespace

void Update(const AudioTap& tap, float dt) {
    dt = std::clamp(dt, 0.0f, 0.1f); // guard against a huge dt after a stall/breakpoint

    // --- Onset detection: short-window RMS vs. a decaying envelope ---
    static std::vector<AudioTapFrame> s_scratch(kRmsWindow);
    size_t got = tap.ReadLatest(s_scratch.data(), kRmsWindow);
    if (got > 0) {
        double sumSquares = 0.0;
        for (size_t i = 0; i < got; ++i) {
            float mono = (s_scratch[i].left + s_scratch[i].right) * 0.5f;
            sumSquares += static_cast<double>(mono) * static_cast<double>(mono);
        }
        float rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(got)));

        constexpr float kThresholdRatio = 1.5f; // jump vs. the envelope to count as a transient
        constexpr float kMinRms = 0.02f;        // ignore near-silence noise floor entirely
        constexpr float kEnvelopeDecay = 0.90f; // per-Update decay -- fast enough to re-arm between beats

        if (rms > kMinRms && rms > s_envelope * kThresholdRatio) {
            float strength = std::clamp((rms - s_envelope) / std::max(rms, 0.0001f), 0.0f, 1.0f);
            SpawnBurst(strength);
        }
        s_envelope = std::max(rms, s_envelope * kEnvelopeDecay);
    }

    // --- Integrate / fade / recycle every live particle ---
    constexpr float kGravity = 0.6f; // normalized units/sec^2, a gentle downward arc
    for (auto& p : s_particles) {
        if (!p.alive) continue;
        p.age += dt;
        if (p.age >= p.lifetime) {
            p.alive = false;
            continue;
        }
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vy += kGravity * dt;
    }
}

void Draw(ImDrawList* drawList, ImVec2 origin, ImVec2 size) {
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 255));

    // Reference palette: purple (young) -> cyan (about to fade out).
    constexpr ImU32 kPurple = IM_COL32(160, 60, 220, 255);
    constexpr ImU32 kCyan = IM_COL32(60, 220, 230, 255);

    for (const auto& p : s_particles) {
        if (!p.alive) continue;
        float t = std::clamp(p.age / std::max(p.lifetime, 0.0001f), 0.0f, 1.0f);
        float alpha = 1.0f - t;

        ImVec4 c0 = ImGui::ColorConvertU32ToFloat4(kPurple);
        ImVec4 c1 = ImGui::ColorConvertU32ToFloat4(kCyan);
        ImVec4 blended(std::lerp(c0.x, c1.x, t), std::lerp(c0.y, c1.y, t), std::lerp(c0.z, c1.z, t), alpha);
        ImU32 color = ImGui::ColorConvertFloat4ToU32(blended);

        ImVec2 screenPos(origin.x + p.x * size.x, origin.y + p.y * size.y);
        float radius = std::lerp(3.0f, 1.0f, t);
        drawList->AddCircleFilled(screenPos, radius, color);
    }
}

} // namespace ParticleVisualizer

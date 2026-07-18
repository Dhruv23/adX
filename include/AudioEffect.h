#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

// Per-track insert effect (Phase 5). Lifecycle contract:
//
//  - Instances are created on the MAIN thread only (constructors allocate
//    their DSP buffers up front — never on the audio thread).
//  - Tracks hold effects via shared_ptr, so the Track copies that
//    DispatchSequenceUpdate ships to the audio thread ALIAS the same effect
//    instances. That is deliberate: reverb tails survive a sequence hot-swap,
//    and UI parameter tweaks are heard live without re-dispatching the whole
//    track list.
//  - processSample() is called by the AUDIO thread only (DSP state is not
//    protected); parameters are std::atomic<float> so the main thread can
//    write them concurrently without a lock.
class AudioEffect {
public:
    virtual ~AudioEffect() = default;

    // Audio thread only. Processes one stereo frame in place.
    virtual void processSample(float& left, float& right) = 0;

    // Stable identifier used by .adx serialization ("Reverb", "Distortion").
    virtual const char* typeName() const = 0;

    // Same concrete type and same parameter values (used by hot-reload
    // diffing, where a reparsed file always yields fresh instances).
    virtual bool isEquivalent(const AudioEffect& other) const = 0;
};

// Freeverb-style algorithmic reverb (8 parallel combs + 4 series allpasses
// per channel, right channel offset by the classic 23-sample stereo spread).
class ReverbEffect : public AudioEffect {
public:
    ReverbEffect(float mix = 0.35f, float roomSize = 0.8f, float damping = 0.5f);

    void processSample(float& left, float& right) override;
    const char* typeName() const override { return "Reverb"; }
    bool isEquivalent(const AudioEffect& other) const override;

    std::atomic<float> mix;      // 0 dry .. 1 wet
    std::atomic<float> roomSize; // 0 .. 1
    std::atomic<float> damping;  // 0 .. 1

private:
    struct Comb {
        std::vector<float> buffer;
        size_t index = 0;
        float filterStore = 0.0f;
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t index = 0;
    };

    static constexpr size_t kNumCombs = 8;
    static constexpr size_t kNumAllpasses = 4;

    std::array<Comb, kNumCombs> m_combL;
    std::array<Comb, kNumCombs> m_combR;
    std::array<Allpass, kNumAllpasses> m_allpassL;
    std::array<Allpass, kNumAllpasses> m_allpassR;
};

// tanh waveshaper drive — the hardstyle kick distortion stage.
class DistortionEffect : public AudioEffect {
public:
    DistortionEffect(float drive = 8.0f, float mix = 1.0f);

    void processSample(float& left, float& right) override;
    const char* typeName() const override { return "Distortion"; }
    bool isEquivalent(const AudioEffect& other) const override;

    std::atomic<float> drive; // 1 (clean-ish) .. 30 (blown out)
    std::atomic<float> mix;   // 0 dry .. 1 wet
};

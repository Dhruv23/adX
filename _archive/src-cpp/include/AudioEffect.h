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

    // Fresh instance with the same parameters but pristine DSP state. Used by
    // offline export rendering, which must never process the SAME instance the
    // live audio thread is processing (concurrent DSP-state mutation).
    virtual std::shared_ptr<AudioEffect> clone() const = 0;
};

// Freeverb-style algorithmic reverb (8 parallel combs + 4 series allpasses
// per channel, right channel offset by the classic 23-sample stereo spread).
class ReverbEffect : public AudioEffect {
public:
    ReverbEffect(float mix = 0.35f, float roomSize = 0.8f, float damping = 0.5f);

    void processSample(float& left, float& right) override;
    const char* typeName() const override { return "Reverb"; }
    bool isEquivalent(const AudioEffect& other) const override;
    std::shared_ptr<AudioEffect> clone() const override {
        return std::make_shared<ReverbEffect>(mix.load(), roomSize.load(), damping.load());
    }

    // Phase 5: wet-only variant used by the master aux-send bus (per-track
    // SEND=Reverb,Amount). Runs the same comb/allpass core as processSample
    // but returns just the wet tail with no dry crossfade — the master bus
    // manages its own dry/wet mix (m_masterReverb.mix), so a per-track send
    // must not also bleed its input back into the dry passthrough the way
    // processSample's normal per-track-insert crossfade would.
    void processWetOnly(float inputLeft, float inputRight, float& outLeft, float& outRight);

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

    // Shared comb+allpass core for processSample/processWetOnly.
    void runCombsAndAllpasses(float inputLeft, float inputRight, float& outLeft, float& outRight);
};

// tanh waveshaper drive — the hardstyle kick distortion stage.
class DistortionEffect : public AudioEffect {
public:
    DistortionEffect(float drive = 8.0f, float mix = 1.0f);

    void processSample(float& left, float& right) override;
    const char* typeName() const override { return "Distortion"; }
    bool isEquivalent(const AudioEffect& other) const override;
    std::shared_ptr<AudioEffect> clone() const override {
        return std::make_shared<DistortionEffect>(drive.load(), mix.load());
    }

    std::atomic<float> drive; // 1 (clean-ish) .. 30 (blown out)
    std::atomic<float> mix;   // 0 dry .. 1 wet
};

// Phase 5: bit-depth quantization + sample-rate decimation ("lo-fi"/8-bit
// grit — the Vocal/Hat character in suffocation.adx).
class BitcrushEffect : public AudioEffect {
public:
    BitcrushEffect(float bitDepth = 8.0f, float rateHz = 22050.0f, float mix = 1.0f);

    void processSample(float& left, float& right) override;
    const char* typeName() const override { return "Bitcrush"; }
    bool isEquivalent(const AudioEffect& other) const override;
    std::shared_ptr<AudioEffect> clone() const override {
        return std::make_shared<BitcrushEffect>(bitDepth.load(), rateHz.load(), mix.load());
    }

    std::atomic<float> bitDepth; // 1..16 bits
    std::atomic<float> rateHz;   // sample-and-hold decimation rate
    std::atomic<float> mix;      // 0 dry .. 1 wet

private:
    // Sample-and-hold state (audio thread only). m_phase starts at 1.0 so
    // the very first processSample() call captures immediately instead of
    // holding silence until the phase accumulator first wraps.
    float m_heldL = 0.0f;
    float m_heldR = 0.0f;
    float m_phase = 1.0f;
};

// Phase 5: modulated stereo delay-line chorus (quadrature LFO offset between
// channels for width, classic supersaw/vocal-doubling thickener).
class ChorusEffect : public AudioEffect {
public:
    ChorusEffect(float rateHz = 0.5f, float depth = 0.5f, float mix = 0.5f);

    void processSample(float& left, float& right) override;
    const char* typeName() const override { return "Chorus"; }
    bool isEquivalent(const AudioEffect& other) const override;
    std::shared_ptr<AudioEffect> clone() const override {
        return std::make_shared<ChorusEffect>(rateHz.load(), depth.load(), mix.load());
    }

    std::atomic<float> rateHz; // LFO speed
    std::atomic<float> depth;  // 0..1, fraction of the modulation range
    std::atomic<float> mix;    // 0 dry .. 1 wet

private:
    std::vector<float> m_bufferL;
    std::vector<float> m_bufferR;
    size_t m_writeIndex = 0;
    float m_lfoPhase = 0.0f; // 0..1; right channel reads at +0.25 (quadrature)
};

// Phase 5: 3-band EQ (low shelf / mid peak / high shelf), RBJ cookbook
// biquads at fixed corner frequencies (the .adx format only carries the
// three gains, not the corner frequencies — see AudioEffects.cpp).
class EQEffect : public AudioEffect {
public:
    EQEffect(float lowGainDb = 0.0f, float midGainDb = 0.0f, float highGainDb = 0.0f);

    void processSample(float& left, float& right) override;
    const char* typeName() const override { return "EQ"; }
    bool isEquivalent(const AudioEffect& other) const override;
    std::shared_ptr<AudioEffect> clone() const override {
        return std::make_shared<EQEffect>(lowGainDb.load(), midGainDb.load(), highGainDb.load());
    }

    std::atomic<float> lowGainDb;
    std::atomic<float> midGainDb;
    std::atomic<float> highGainDb;

private:
    // Direct Form I biquad state per band per channel: {x1, x2, y1, y2}.
    std::array<std::array<float, 4>, 3> m_stateL{};
    std::array<std::array<float, 4>, 3> m_stateR{};
};

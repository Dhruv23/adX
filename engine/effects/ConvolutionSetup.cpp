// adx-thread: main
#include "engine/effects/ConvolutionSetup.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "engine/dsp/Math.h"
#include "engine/dsp/Noise.h"
#include "engine/format/audio/AudioFileLoader.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/project/Resources.h"
#include "engine/project/TypeCatalog.h"

namespace adx::effects {

/// The synthetic room is built at this rate whatever the project's: an IR is data,
/// like a sample, and a 44.1 kHz session hears the same room slightly longer - the
/// same compromise every IR file makes.
constexpr std::uint32_t kSyntheticRate = 48000;

struct ConvolutionPins {
    std::vector<float> left;
    std::vector<float> right;
    /// What it was built from: a resolved file, or the room's two numbers.
    std::filesystem::path file;
    double size{0.0};
    double damping{0.0};
};

void destroyConvolutionPins(ConvolutionPins* pins) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - the node's opaque owner.
    delete pins;
}

namespace {

double slotValue(const project::Slot& slot, std::string_view name) {
    if (const project::SlotParam* param = slot.find(name)) {
        return param->value;
    }
    const project::TypeInfo* type = project::findEffectType("Convolution");
    const project::ParamDescriptor* descriptor =
        type != nullptr ? project::findParam(*type, name) : nullptr;
    return descriptor != nullptr ? descriptor->defaultValue : 0.0;
}

std::filesystem::path fileOf(const project::Slot& slot, const project::Resources* resources) {
    if (resources == nullptr || !slot.impulse.valid()) {
        return {};
    }
    const project::SampleRef* ref = resources->find(slot.impulse);
    return ref != nullptr ? format::SamplePool::global().resolve(*resources, ref->path)
                          : std::filesystem::path{};
}

} // namespace

std::vector<float> syntheticRoom(double seconds, double damping, std::uint32_t sampleRate,
                                 std::uint32_t seed) {
    seconds = std::clamp(seconds, 0.05, 8.0);
    damping = std::clamp(damping, 0.0, 1.0);
    const auto gap = static_cast<std::size_t>(0.010 * sampleRate);
    const auto length = gap + static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> ir(length, 0.0F);
    dsp::WhiteNoise noise{seed};
    // -60 dB over `seconds`.
    const double decay = dsp::exp(-6.907755278982137 / (seconds * sampleRate));
    double envelope = 1.0;
    double lowpass = 0.0;
    // A one-pole low-pass that closes as the tail ages: damping 0 never closes.
    for (std::size_t n = gap; n < length; ++n) {
        const double age = static_cast<double>(n - gap) / static_cast<double>(length - gap);
        const double coefficient = damping * 0.9 * age;
        lowpass =
            ((1.0 - coefficient) * static_cast<double>(noise.next())) + (coefficient * lowpass);
        ir[n] = static_cast<float>(lowpass * envelope);
        envelope *= decay;
    }
    // Unit energy: the room is as loud as what goes in, whatever its size.
    double energy = 0.0;
    for (const float s : ir) {
        energy += static_cast<double>(s) * s;
    }
    if (energy > 0.0) {
        const double scale = 1.0 / std::sqrt(energy);
        for (float& s : ir) {
            s = static_cast<float>(s * scale);
        }
    }
    return ir;
}

void configureConvolution(Convolution& node, const project::Slot& slot,
                          const project::Resources* resources) {
    auto pins = std::make_unique<ConvolutionPins>();
    pins->file = fileOf(slot, resources);
    bool loaded = false;
    if (!pins->file.empty()) {
        format::SampleBuffer buffer;
        std::string error;
        if (format::decodeFile(pins->file, buffer, error) && buffer.frames > 0) {
            const std::span<const float> l = buffer.leftAudio();
            const std::span<const float> r = buffer.rightAudio();
            pins->left.assign(l.begin(), l.end());
            pins->right.assign(r.begin(), r.end());
            loaded = true;
        }
    }
    if (!loaded) {
        pins->file.clear();
        pins->size = slotValue(slot, "size");
        pins->damping = slotValue(slot, "damping");
        pins->left = syntheticRoom(pins->size, pins->damping, kSyntheticRate, 0xC0117EU);
        pins->right = syntheticRoom(pins->size, pins->damping, kSyntheticRate, 0x5EC0DEU);
    }
    const std::span<const float> left{pins->left};
    const std::span<const float> right{pins->right};
    node.setImpulse(left, right, pins.release());
}

bool convolutionMatches(const Convolution& node, const project::Slot& slot,
                        const project::Resources* resources) {
    const ConvolutionPins* pins = node.pins();
    if (pins == nullptr) {
        return false;
    }
    const std::filesystem::path file = fileOf(slot, resources);
    if (!file.empty() || !pins->file.empty()) {
        return file == pins->file;
    }
    return pins->size == slotValue(slot, "size") && pins->damping == slotValue(slot, "damping");
}

void Convolution::copyConfigTo(Effect& fresh) const {
    auto* target = dynamic_cast<Convolution*>(&fresh);
    if (target == nullptr || m_pins == nullptr) {
        return;
    }
    auto pins = std::make_unique<ConvolutionPins>(*m_pins);
    const std::span<const float> left{pins->left};
    const std::span<const float> right{pins->right};
    target->setImpulse(left, right, pins.release());
}

bool Convolution::isEquivalent(const Effect& other) const noexcept {
    const auto* that = dynamic_cast<const Convolution*>(&other);
    if (that == nullptr || m_pins == nullptr || that->m_pins == nullptr) {
        return false;
    }
    return m_pins->file == that->m_pins->file && m_pins->size == that->m_pins->size &&
           m_pins->damping == that->m_pins->damping && m_pins->left == that->m_pins->left;
}

} // namespace adx::effects

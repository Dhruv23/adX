// adx-thread: main
#include "engine/effects/Factory.h"

#include <algorithm>

#include "engine/effects/ConvolutionSetup.h"
#include "engine/effects/Delay.h"
#include "engine/effects/Drive.h"
#include "engine/effects/Dynamics.h"
#include "engine/effects/Effect.h"
#include "engine/effects/GrossBeat.h"
#include "engine/effects/Modulation.h"
#include "engine/effects/Multiband.h"
#include "engine/effects/ParametricEq.h"
#include "engine/effects/Ported.h"
#include "engine/effects/Spectral.h"
#include "engine/effects/Vocal.h"
#include "engine/project/TypeCatalog.h"

namespace adx::effects {
namespace {

/// The lookahead a slot asks for, or the type's default when it does not say.
[[nodiscard]] float lookaheadOf(const project::Slot& slot) noexcept {
    if (const project::SlotParam* param = slot.find("lookahead")) {
        return std::max(0.0F, static_cast<float>(param->value));
    }
    if (const project::TypeInfo* type = project::findEffectType(slot.type)) {
        if (const project::ParamDescriptor* descriptor = project::findParam(*type, "lookahead")) {
            return descriptor->defaultValue;
        }
    }
    return 0.0F;
}

} // namespace

std::shared_ptr<graph::SlotNode> makeEffect(std::string_view type) {
    if (type == "Reverb") {
        return std::make_shared<Reverb>();
    }
    if (type == "Distortion") {
        return std::make_shared<Distortion>();
    }
    if (type == "Bitcrush") {
        return std::make_shared<Bitcrush>();
    }
    if (type == "Chorus") {
        return std::make_shared<Chorus>();
    }
    if (type == "EQ") {
        return std::make_shared<Eq3>();
    }
    if (type == "Delay") {
        return std::make_shared<Delay>();
    }
    if (type == "Compressor") {
        return std::make_shared<Compressor>();
    }
    if (type == "Ducker") {
        return std::make_shared<Ducker>();
    }
    if (type == "Limiter") {
        return std::make_shared<Limiter>();
    }
    if (type == "Gate") {
        return std::make_shared<Gate>();
    }
    if (type == "ParametricEq") {
        return std::make_shared<ParametricEq>();
    }
    if (type == "Flanger") {
        return std::make_shared<Flanger>();
    }
    if (type == "Phaser") {
        return std::make_shared<Phaser>();
    }
    if (type == "Tremolo") {
        return std::make_shared<Tremolo>();
    }
    if (type == "RingMod") {
        return std::make_shared<RingMod>();
    }
    if (type == "FrequencyShifter") {
        return std::make_shared<FrequencyShifter>();
    }
    if (type == "StereoImager") {
        return std::make_shared<StereoImager>();
    }
    if (type == "Saturation") {
        return std::make_shared<Saturation>();
    }
    if (type == "Overdrive") {
        return std::make_shared<Overdrive>();
    }
    if (type == "MultibandComp") {
        return std::make_shared<MultibandComp>();
    }
    if (type == "TransientShaper") {
        return std::make_shared<TransientShaper>();
    }
    if (type == "PitchShifter") {
        return std::make_shared<PitchShifter>();
    }
    if (type == "SpectralFreeze") {
        return std::make_shared<SpectralFreeze>();
    }
    if (type == "Vocoder") {
        return std::make_shared<Vocoder>();
    }
    if (type == "FormantFilter") {
        return std::make_shared<FormantFilter>();
    }
    if (type == "GrossBeat") {
        return std::make_shared<GrossBeat>();
    }
    if (type == "Convolution") {
        // A bare Convolution plays its default synthetic room.
        auto node = std::make_shared<Convolution>();
        project::Slot slot;
        slot.type = "Convolution";
        configureConvolution(*node, slot, nullptr);
        return node;
    }
    return std::make_shared<graph::SlotNode>();
}

std::shared_ptr<graph::SlotNode> makeEffect(const project::Slot& slot,
                                            const project::Resources* resources) {
    if (slot.type == "Convolution") {
        auto node = std::make_shared<Convolution>();
        configureConvolution(*node, slot, resources);
        return node;
    }
    std::shared_ptr<graph::SlotNode> node = makeEffect(slot.type);
    if (auto* lookahead = dynamic_cast<LookaheadEffect*>(node.get())) {
        lookahead->setLookaheadMs(lookaheadOf(slot));
    }
    return node;
}

bool configMatches(const graph::SlotNode& node, const project::Slot& slot,
                   const project::Resources* resources) {
    if (node.typeName() != canonicalType(slot.type)) {
        return false;
    }
    if (const auto* lookahead = dynamic_cast<const LookaheadEffect*>(&node)) {
        return lookahead->lookaheadMs() == lookaheadOf(slot);
    }
    if (const auto* convolution = dynamic_cast<const Convolution*>(&node)) {
        return convolutionMatches(*convolution, slot, resources);
    }
    return true;
}

std::shared_ptr<graph::SlotNode> clone(const graph::SlotNode& effect) {
    std::shared_ptr<graph::SlotNode> fresh = makeEffect(effect.typeName());
    if (const auto* source = dynamic_cast<const Effect*>(&effect)) {
        if (auto* target = dynamic_cast<Effect*>(fresh.get())) {
            source->copyConfigTo(*target);
        }
        if (const auto* lookahead = dynamic_cast<const LookaheadEffect*>(source)) {
            if (auto* targetLookahead = dynamic_cast<LookaheadEffect*>(fresh.get())) {
                targetLookahead->setLookaheadMs(lookahead->lookaheadMs());
            }
        }
    }
    return fresh;
}

std::string_view canonicalType(std::string_view type) noexcept {
    const project::TypeInfo* info = project::findEffectType(type);
    return info != nullptr ? info->name : std::string_view{};
}

} // namespace adx::effects

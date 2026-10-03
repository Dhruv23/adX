// adx-thread: main
#include "engine/effects/Factory.h"

#include <algorithm>

#include "engine/effects/Delay.h"
#include "engine/effects/Dynamics.h"
#include "engine/effects/Effect.h"
#include "engine/effects/ParametricEq.h"
#include "engine/effects/Ported.h"
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
    return std::make_shared<graph::SlotNode>();
}

std::shared_ptr<graph::SlotNode> makeEffect(const project::Slot& slot) {
    std::shared_ptr<graph::SlotNode> node = makeEffect(slot.type);
    if (auto* lookahead = dynamic_cast<LookaheadEffect*>(node.get())) {
        lookahead->setLookaheadMs(lookaheadOf(slot));
    }
    return node;
}

bool configMatches(const graph::SlotNode& node, const project::Slot& slot) noexcept {
    if (node.typeName() != canonicalType(slot.type)) {
        return false;
    }
    if (const auto* lookahead = dynamic_cast<const LookaheadEffect*>(&node)) {
        return lookahead->lookaheadMs() == lookaheadOf(slot);
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

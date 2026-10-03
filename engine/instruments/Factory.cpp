// adx-thread: main
#include "engine/instruments/Factory.h"

#include <algorithm>

#include "engine/graph/nodes/TestToneNode.h"
#include "engine/instruments/additive/AdditiveInstrument.h"
#include "engine/instruments/sampler/SamplerInstrument.h"
#include "engine/instruments/sampler/SamplerSetup.h"
#include "engine/instruments/va/VaInstrument.h"
#include "engine/project/TypeCatalog.h"

namespace adx::instruments {

std::shared_ptr<graph::ChannelNode> makeInstrument(std::string_view type, std::uint32_t channelId,
                                                   std::uint16_t maxPolyphony,
                                                   project::VoiceStealMode stealMode) {
    if (type == "testtone") {
        return std::make_shared<graph::TestToneNode>(channelId, maxPolyphony, stealMode);
    }
    if (type == "additive") {
        return std::make_shared<AdditiveInstrument>(channelId, maxPolyphony, stealMode);
    }
    if (type == "sampler") {
        return std::make_shared<SamplerInstrument>(channelId, maxPolyphony, stealMode);
    }
    if (type == "va") {
        return std::make_shared<VaInstrument>(channelId, maxPolyphony, stealMode);
    }
    return std::make_shared<graph::SilentChannelNode>(channelId, maxPolyphony, stealMode);
}

std::shared_ptr<graph::ChannelNode> makeInstrument(const project::Channel& channel,
                                                   const InstrumentContext& context) {
    std::shared_ptr<graph::ChannelNode> node = makeInstrument(
        channel.instrument.type, channel.id.value, channel.maxPolyphony, channel.stealMode);
    if (auto* sampler = dynamic_cast<SamplerInstrument*>(node.get())) {
        configureSampler(*sampler, channel, context.resources, context.pool);
    }
    return node;
}

bool configMatches(const graph::ChannelNode& node, const project::Channel& channel,
                   const InstrumentContext& context) {
    if (node.maxPolyphony() != std::max<std::uint16_t>(channel.maxPolyphony, 1) ||
        node.stealMode() != channel.stealMode ||
        node.typeName() != canonicalType(channel.instrument.type)) {
        return false;
    }
    if (const auto* sampler = dynamic_cast<const SamplerInstrument*>(&node)) {
        return samplerMatches(*sampler, channel, context.resources, context.pool);
    }
    return true;
}

std::string_view canonicalType(std::string_view type) noexcept {
    const project::TypeInfo* info = project::findInstrumentType(type);
    return info != nullptr ? info->name : std::string_view{};
}

} // namespace adx::instruments

// adx-thread: main
#include "engine/instruments/Factory.h"

#include <algorithm>

#include "engine/graph/nodes/TestToneNode.h"
#include "engine/instruments/additive/AdditiveInstrument.h"
#include "engine/instruments/drumsynth/DrumSynth.h"
#include "engine/instruments/fm/FmInstrument.h"
#include "engine/instruments/granular/GranularInstrument.h"
#include "engine/instruments/pool/SamplePoolChannel.h"
#include "engine/instruments/sampler/SamplerInstrument.h"
#include "engine/instruments/sampler/SamplerSetup.h"
#include "engine/instruments/slicer/Slicer.h"
#include "engine/instruments/va/VaInstrument.h"
#include "engine/instruments/voice/VoiceSetup.h"
#include "engine/instruments/wavetable/WavetableSetup.h"
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
    if (type == "slicer") {
        return std::make_shared<SlicerInstrument>(channelId, maxPolyphony, stealMode);
    }
    if (type == "pool") {
        return std::make_shared<SamplePoolChannel>(channelId, maxPolyphony, stealMode);
    }
    if (type == "drumsynth") {
        return std::make_shared<DrumSynth>(channelId, maxPolyphony, stealMode);
    }
    if (type == "granular") {
        return std::make_shared<GranularInstrument>(channelId, maxPolyphony, stealMode);
    }
    if (type == "fm") {
        return std::make_shared<FmInstrument>(channelId, maxPolyphony, stealMode);
    }
    if (type == "wavetable") {
        return std::make_shared<WavetableInstrument>(channelId, maxPolyphony, stealMode);
    }
    if (type == "voice") {
        return std::make_shared<VoiceInstrument>(channelId, maxPolyphony, stealMode);
    }
    return std::make_shared<graph::SilentChannelNode>(channelId, maxPolyphony, stealMode);
}

std::shared_ptr<graph::ChannelNode> makeInstrument(const project::Channel& channel,
                                                   const InstrumentContext& context) {
    std::shared_ptr<graph::ChannelNode> node = makeInstrument(
        channel.instrument.type, channel.id.value, channel.maxPolyphony, channel.stealMode);
    if (auto* sampler = dynamic_cast<ZoneSet*>(node.get())) {
        configureSampler(*sampler, channel, context.resources, context.pool);
    }
    if (auto* wavetable = dynamic_cast<WavetableInstrument*>(node.get())) {
        configureWavetable(*wavetable, channel, context.resources);
    }
    if (auto* voice = dynamic_cast<VoiceInstrument*>(node.get())) {
        configureVoice(*voice, channel, context.project, context.resources);
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
    if (const auto* sampler = dynamic_cast<const ZoneSet*>(&node)) {
        return samplerMatches(*sampler, channel, context.resources, context.pool);
    }
    if (const auto* wavetable = dynamic_cast<const WavetableInstrument*>(&node)) {
        return wavetableMatches(*wavetable, channel, context.resources);
    }
    if (const auto* voice = dynamic_cast<const VoiceInstrument*>(&node)) {
        return voiceMatches(*voice, channel, context.project, context.resources);
    }
    return true;
}

std::string_view canonicalType(std::string_view type) noexcept {
    const project::TypeInfo* info = project::findInstrumentType(type);
    return info != nullptr ? info->name : std::string_view{};
}

} // namespace adx::instruments

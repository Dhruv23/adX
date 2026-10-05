#include "tests/cpp/instruments/InstrumentHarness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <span>
#include <string>

#include "engine/format/audio/SamplePool.h"
#include "engine/instruments/Factory.h"
#include "engine/instruments/voice/VoiceInstrument.h"
#include "engine/project/Channel.h"
#include "engine/project/Resources.h"
#include "engine/project/TypeCatalog.h"
#include "engine/render/OfflineRender.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"
#include "engine/transport/TimeSource.h"

namespace adx::tests {

std::vector<float> instrumentParams(std::string_view type) {
    std::vector<float> params{1.0F, 0.0F, 1.0F, 0.0F};
    if (const project::TypeInfo* info = project::findInstrumentType(type)) {
        for (const project::ParamDescriptor& descriptor : info->params) {
            params.push_back(descriptor.defaultValue);
        }
    }
    return params;
}

std::uint32_t instrumentParamIndex(std::string_view type, std::string_view name) {
    const project::TypeInfo* info = project::findInstrumentType(type);
    if (info == nullptr) {
        return project::kNoParam;
    }
    const std::uint32_t index = project::paramIndexOf(*info, name);
    return index == project::kNoParam ? index : index + graph::kChannelParamCount;
}

namespace {

graph::BlockEvent makeEvent(graph::BlockEventKind kind, std::uint32_t offset, std::uint32_t noteId,
                            std::uint8_t pitch, std::uint8_t velocity, float value,
                            std::uint32_t duration) {
    return graph::BlockEvent{.offset = offset,
                             .noteId = noteId,
                             .instance = 1,
                             .timeSource = 0,
                             .endTick = 1'000'000'000,
                             .value = value,
                             .duration = duration,
                             .kind = kind,
                             .pitch = pitch,
                             .velocity = velocity};
}

} // namespace

graph::BlockEvent noteOn(std::uint32_t offset, std::uint32_t noteId, std::uint8_t pitch,
                         std::uint8_t velocity) {
    return makeEvent(graph::BlockEventKind::NoteOn, offset, noteId, pitch, velocity, 0.0F, 0);
}

graph::BlockEvent noteOff(std::uint32_t offset, std::uint32_t noteId, std::uint8_t pitch) {
    return makeEvent(graph::BlockEventKind::NoteOff, offset, noteId, pitch, 0, 0.0F, 0);
}

graph::BlockEvent pitchGlide(std::uint32_t offset, std::uint32_t noteId, std::uint8_t pitch,
                             float cents, std::uint32_t duration) {
    return makeEvent(graph::BlockEventKind::PitchGlide, offset, noteId, pitch, 0, cents, duration);
}

InstrumentRun runInstrument(graph::ChannelNode& node, std::vector<graph::BlockEvent> events,
                            std::uint32_t frames, std::vector<float> params, std::uint32_t block) {
    std::ranges::stable_sort(events, {}, &graph::BlockEvent::offset);
    InstrumentRun run;
    run.left.assign(frames, 0.0F);
    run.right.assign(frames, 0.0F);
    run.sounding.reserve((frames / block) + 1);
    std::vector<std::byte> arenaStorage(1U << 20U);
    rt::BlockArena arena{arenaStorage.data(), arenaStorage.size()};
    const transport::TimeSource time;
    std::vector<graph::BlockEvent> here;
    here.reserve(events.size());

    rt::ViolationLog& log = rt::ViolationLog::instance();
    log.reset();
    std::size_t next = 0;
    for (std::uint32_t start = 0; start < frames; start += block) {
        const std::uint32_t count = std::min(block, frames - start);
        here.clear();
        while (next < events.size() && events[next].offset < start + count) {
            graph::BlockEvent event = events[next++];
            event.offset -= start;
            here.push_back(event);
        }
        const std::array<std::span<float>, 2> outputs{
            std::span<float>{run.left}.subspan(start, count),
            std::span<float>{run.right}.subspan(start, count)};
        arena.reset();
        graph::ProcessContext context{.time = time,
                                      .outputs = outputs,
                                      .inputs = {},
                                      .frames = count,
                                      .sampleRate = 48000,
                                      .events = here,
                                      .params = params,
                                      .automation = {},
                                      .arena = arena};
        {
            const rt::ScopedRtSection section;
            node.process(context);
        }
        run.sounding.push_back(node.pool().soundingCount());
    }
    run.violations =
        log.count(rt::ViolationKind::Allocation) + log.count(rt::ViolationKind::Deallocation);
    return run;
}

namespace {

/// The sample a table-driven test plays through a Sampler: one second of a 440 Hz sine
/// at 0.5, mapped across the whole keyboard with its root at A4. Without a zone a
/// Sampler is silent by design, which is not what a lifecycle test is asking about.
struct DefaultSampler {
    format::SamplePool pool{1};
    project::Resources resources;
    project::Channel channel;

    DefaultSampler() {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "adx_harness";
        std::filesystem::create_directories(dir);
        std::vector<float> sine(48000);
        for (std::size_t i = 0; i < sine.size(); ++i) {
            sine[i] = 0.5F * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 *
                                                         static_cast<double>(i) / 48000.0));
        }
        static_cast<void>(render::writeWavFloat32((dir / "sine.wav").string(), sine, 1, 48000));
        resources.baseDirectory = dir.string();
        resources.samples.push_back(
            project::SampleRef{.id = core::SampleId{1}, .path = "sine.wav"});
        channel.instrument.type = "sampler";
        project::SampleZone zone;
        zone.sample = core::SampleId{1};
        zone.rootKey = 69;
        channel.instrument.zones.push_back(zone);
    }
};

/// What a table-driven test plays through a Voice: the same rendered clip - one second
/// of a 440 Hz sine with a 100 ms lead, as if sung - for every note id up to 64. A
/// Voice's rendering is its own tests' business (test_voice.cpp); these exercise the
/// node that plays renders, which without any is silent by design.
struct DefaultVoice {
    std::vector<float> samples;
    instruments::VoiceClip clip;

    DefaultVoice() : samples(48000) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = 0.5F * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 *
                                                            static_cast<double>(i) / 48000.0));
        }
        clip.samples = samples.data();
        clip.frames = static_cast<std::uint32_t>(samples.size());
        clip.lead = 4800;
        clip.ready.store(true);
    }
};

void giveVoiceClips(graph::ChannelNode& node) {
    static const DefaultVoice kVoice;
    auto* target = dynamic_cast<instruments::VoiceInstrument*>(&node);
    if (target == nullptr) {
        return;
    }
    rt::OwnedArray<instruments::VoiceNoteRef> notes;
    notes.allocate(64);
    for (std::uint32_t n = 0; n < 64; ++n) {
        notes.view()[n] = instruments::VoiceNoteRef{.noteId = n, .clip = &kVoice.clip};
    }
    target->setNotes(std::move(notes), nullptr);
}

} // namespace

std::shared_ptr<graph::ChannelNode> preparedInstrument(std::string_view type,
                                                       std::uint16_t polyphony) {
    std::shared_ptr<graph::ChannelNode> node;
    // The instruments that play pool samples get the same sine across the keyboard.
    if (type == "sampler" || type == "slicer" || type == "pool") {
        static DefaultSampler sampler;
        project::Channel channel = sampler.channel;
        channel.instrument.type = std::string(type);
        if (type == "slicer") {
            channel.instrument.zones.front().end = 24000; // a half-second slice
        }
        channel.id = core::ChannelId{1};
        channel.maxPolyphony = polyphony;
        node = instruments::makeInstrument(
            channel,
            instruments::InstrumentContext{.resources = &sampler.resources, .pool = &sampler.pool});
        sampler.pool.waitAll();
    } else {
        node = instruments::makeInstrument(type, 1, polyphony,
                                           project::VoiceStealMode::OldestReleased);
    }
    giveVoiceClips(*node);
    node->prepare(graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    return node;
}

} // namespace adx::tests

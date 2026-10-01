#include "tests/cpp/graph/GraphFixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

#include "engine/graph/Pdc.h"
#include "engine/graph/TopoSort.h"

namespace adx::tests {

void ImpulseNode::process(graph::ProcessContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const float value = m_frame + i == m_at ? 1.0F : 0.0F;
        context.outputs[0][i] = value;
        context.outputs[1][i] = value;
    }
    m_frame += context.frames;
}

void LatencyNode::prepare(const graph::PrepareInfo& /*info*/) {
    m_left.assign(m_latency, 0.0F);
    m_right.assign(m_latency, 0.0F);
}

void LatencyNode::process(graph::ProcessContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (m_latency == 0) {
            context.outputs[0][i] = context.inputs[0][i];
            context.outputs[1][i] = context.inputs[1][i];
            continue;
        }
        context.outputs[0][i] = m_left[m_position];
        context.outputs[1][i] = m_right[m_position];
        m_left[m_position] = context.inputs[0][i];
        m_right[m_position] = context.inputs[1][i];
        m_position = (m_position + 1) % m_latency;
    }
}

void SidechainNode::process(graph::ProcessContext& context) noexcept {
    std::ranges::copy(context.inputs[0], context.outputs[0].begin());
    std::ranges::copy(context.inputs[1], context.outputs[1].begin());
}

void ManualRender::compile(const graph::Graph& graph) {
    const graph::TopoResult order = graph::topoSort(graph);
    REQUIRE(order.ok);
    const graph::PdcPlan pdc = graph::computePdc(graph, order.order);
    compiled = std::make_unique<graph::CompiledGraph>(graph, order, pdc);

    tracks.clear();
    for (const auto& list : events) {
        tracks.push_back(project::EventTrack{.events = list});
    }
    cursors.assign(tracks.size(), project::EventCursor{});
    if (params.empty()) {
        params.push_back(0.0F);
    }
    core::computeCumulativeSeconds(tempoEvents, cumSeconds);

    snapshot.sampleRate = 48000;
    snapshot.tempo = core::TempoView{.events = tempoEvents, .cumSeconds = cumSeconds};
    snapshot.eventTracks = tracks;
    snapshot.cursors = cursors;
    snapshot.graph = compiled->view();
    snapshot.params = params;
    transport.arrangement().bindTempo(snapshot.tempo, snapshot.sampleRate);
}

std::vector<float> ManualRender::render(std::uint64_t frames, std::uint32_t block) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2, 0.0F);
    std::uint64_t done = 0;
    while (done < frames) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(block, frames - done));
        arena.reset();
        scheduler.render(snapshot, transport, arena, out.data() + (done * 2), count, 2);
        done += count;
    }
    return out;
}

std::vector<float> runChannelNode(graph::ChannelNode& node, std::vector<graph::BlockEvent> events,
                                  std::uint32_t frames, std::vector<float> params) {
    std::vector<std::byte> arenaStorage(1U << 20U);
    rt::BlockArena arena{arenaStorage.data(), arenaStorage.size()};
    const transport::TimeSource time;
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);

    // One call per 64 frames, delivering each event in the call that contains it -
    // so dispatch across call boundaries is exercised too.
    constexpr std::uint32_t kCall = 64;
    std::size_t next = 0;
    for (std::uint32_t start = 0; start < frames; start += kCall) {
        const std::uint32_t count = std::min(kCall, frames - start);
        std::vector<graph::BlockEvent> here;
        while (next < events.size() && events[next].offset < start + count) {
            graph::BlockEvent event = events[next++];
            event.offset -= start;
            here.push_back(event);
        }
        const std::array<std::span<float>, 2> outputs{
            std::span<float>{left}.subspan(start, count),
            std::span<float>{right}.subspan(start, count)};
        arena.reset();
        graph::ProcessContext context{.time = time,
                                      .outputs = outputs,
                                      .inputs = {},
                                      .frames = count,
                                      .sampleRate = 48000,
                                      .events = here,
                                      .params = params,
                                      .arena = arena};
        node.process(context);
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        out[static_cast<std::size_t>(i) * 2] = left[i];
        out[(static_cast<std::size_t>(i) * 2) + 1] = right[i];
    }
    return out;
}

} // namespace adx::tests

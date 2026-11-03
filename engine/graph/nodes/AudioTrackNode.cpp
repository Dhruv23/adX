#include "engine/graph/nodes/AudioTrackNode.h"
#include "engine/dsp/Math.h"

#include <algorithm>
#include <cmath>

#include "engine/graph/nodes/ChannelNode.h"
#include "engine/graph/nodes/Gain.h"

namespace adx::graph {

namespace {

/// Evaluates a knot chain at increasing positions with one advancing index - the same
/// value knotValueAt gives, without a search per frame.
class KnotWalker {
public:
    KnotWalker(std::span<const project::Knot> knots, float fallback) noexcept
        : m_knots(knots), m_fallback(fallback) {}

    [[nodiscard]] float at(std::int64_t position) noexcept {
        if (m_knots.empty()) {
            return m_fallback;
        }
        if (position <= m_knots.front().tick) {
            return m_knots.front().value;
        }
        while (m_index + 1 < m_knots.size() && m_knots[m_index + 1].tick <= position) {
            ++m_index;
        }
        if (m_index + 1 >= m_knots.size()) {
            return m_knots.back().value;
        }
        const project::Knot& a = m_knots[m_index];
        const project::Knot& b = m_knots[m_index + 1];
        const auto t = static_cast<float>(static_cast<double>(position - a.tick) /
                                          static_cast<double>(b.tick - a.tick));
        return a.value + ((b.value - a.value) * t);
    }

private:
    std::span<const project::Knot> m_knots;
    float m_fallback;
    std::size_t m_index{0};
};

/// Source frame for arrangement sample `position` under a varispeed map.
class SourceWalker {
public:
    explicit SourceWalker(std::span<const SourceKnot> knots) noexcept : m_knots(knots) {}

    [[nodiscard]] double at(std::int64_t position) noexcept {
        if (position <= m_knots.front().at) {
            return m_knots.front().source;
        }
        while (m_index + 1 < m_knots.size() && m_knots[m_index + 1].at <= position) {
            ++m_index;
        }
        if (m_index + 1 >= m_knots.size()) {
            return m_knots.back().source;
        }
        const SourceKnot& a = m_knots[m_index];
        const SourceKnot& b = m_knots[m_index + 1];
        const double t = static_cast<double>(position - a.at) / static_cast<double>(b.at - a.at);
        return a.source + ((b.source - a.source) * t);
    }

private:
    std::span<const SourceKnot> m_knots;
    std::size_t m_index{0};
};

/// Reads one frame at a fractional source position; silence outside the audio. The
/// guard frames (SampleBuffer.h) cover the sinc's reach past either end.
inline void readFrame(const format::SampleView& view, const dsp::SincTable& sinc, double position,
                      float& left, float& right) noexcept {
    const double whole = dsp::floorExact(position);
    const auto index = static_cast<std::int64_t>(whole);
    if (index < -4 || index > static_cast<std::int64_t>(view.frames) + 3) {
        left = 0.0F;
        right = 0.0F;
        return;
    }
    const auto fraction = static_cast<float>(position - whole);
    // The view's pointers sit past the leading guard, so a small negative index is
    // still inside the buffer; offsetting the base keeps the arithmetic unsigned.
    constexpr std::int64_t kBias = 4;
    const auto biased = static_cast<std::size_t>(index + kBias);
    left = dsp::interpolateSinc(sinc, view.left - kBias, biased, fraction);
    right = dsp::interpolateSinc(sinc, view.right - kBias, biased, fraction);
}

} // namespace

void AudioTrackNode::prepare(const PrepareInfo& /*info*/) {
    m_sinc = &dsp::sincTable();
}

void AudioTrackNode::reset() noexcept {}

PortSpec AudioTrackNode::ports() const noexcept {
    return PortSpec{.inputs = 0, .outputs = 1, .acceptsEvents = false};
}

void AudioTrackNode::mix(const AudioRegion& region, const dsp::SincTable& sinc,
                         std::int64_t blockStart, std::int64_t from, std::int64_t to, float* left,
                         float* right) noexcept {
    const format::SampleView& view = region.handle->view();
    KnotWalker gain(region.gain, 1.0F);
    KnotWalker pan(region.pan, 0.0F);
    const bool panned = !region.pan.empty();
    const PanGains centre{};

    if (!region.sourceMap.empty()) {
        SourceWalker source(region.sourceMap);
        for (std::int64_t p = from; p < to; ++p) {
            float l = 0.0F;
            float r = 0.0F;
            readFrame(view, sinc, source.at(p), l, r);
            const float g = gain.at(p);
            const PanGains sides = panned ? balance(pan.at(p)) : centre;
            left[p - blockStart] += l * g * sides.left;
            right[p - blockStart] += r * g * sides.right;
        }
        return;
    }

    // A whole-frame start at unit rate is a copy: no interpolation, no rounding, so a
    // rendered buffer played back comes out bit for bit (freeze_is_transparent).
    if (region.rate == 1.0 && region.sourceAt == dsp::floorExact(region.sourceAt)) {
        const auto first = static_cast<std::int64_t>(region.sourceAt) + (from - region.start);
        const auto frames = static_cast<std::int64_t>(view.frames);
        for (std::int64_t p = from; p < to; ++p) {
            const std::int64_t index = first + (p - from);
            if (index < 0 || index >= frames) {
                continue;
            }
            const auto at = static_cast<std::size_t>(index);
            const float g = gain.at(p);
            const PanGains sides = panned ? balance(pan.at(p)) : centre;
            left[p - blockStart] += view.left[at] * g * sides.left;
            right[p - blockStart] += view.right[at] * g * sides.right;
        }
        return;
    }

    for (std::int64_t p = from; p < to; ++p) {
        float l = 0.0F;
        float r = 0.0F;
        // From the region's start every time, not accumulated: an accumulated position
        // drifts with the block size.
        readFrame(view, sinc,
                  region.sourceAt + (static_cast<double>(p - region.start) * region.rate), l, r);
        const float g = gain.at(p);
        const PanGains sides = panned ? balance(pan.at(p)) : centre;
        left[p - blockStart] += l * g * sides.left;
        right[p - blockStart] += r * g * sides.right;
    }
}

void AudioTrackNode::process(ProcessContext& context) noexcept {
    const std::span<float> left = context.outputs[0];
    const std::span<float> right = context.outputs[1];
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);
    // Software monitoring: the input through this track's insert, rolling or not.
    if (!context.input.empty() && context.inputChannels > 0) {
        const std::uint32_t channels = context.inputChannels;
        for (std::uint32_t f = 0; f < context.frames; ++f) {
            const float l = context.input[static_cast<std::size_t>(f) * channels];
            left[f] = l;
            right[f] =
                channels > 1 ? context.input[(static_cast<std::size_t>(f) * channels) + 1] : l;
        }
    }
    if (!context.time.rolling() || m_sinc == nullptr) {
        return;
    }
    const std::int64_t blockStart = context.time.positionSamples();
    const std::int64_t blockEnd = blockStart + context.frames;
    for (const AudioRegion& region : context.regions) {
        if (region.start >= blockEnd) {
            break;
        }
        if (region.end <= blockStart || region.handle == nullptr || !region.handle->ready()) {
            continue;
        }
        mix(region, *m_sinc, blockStart, std::max(region.start, blockStart),
            std::min(region.end, blockEnd), left.data(), right.data());
    }
    constexpr auto kAudible = static_cast<std::size_t>(ChannelParam::Audible);
    if (m_frozenChannel && kAudible < context.params.size() && context.params[kAudible] != 1.0F) {
        const float audible = context.params[kAudible];
        for (std::size_t i = 0; i < left.size(); ++i) {
            left[i] *= audible;
            right[i] *= audible;
        }
    }
}

} // namespace adx::graph

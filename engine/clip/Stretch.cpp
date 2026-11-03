// adx-thread: main
#include "engine/clip/Stretch.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include <rubberband/RubberBandStretcher.h>

namespace adx::clip {

bool isIdentity(const StretchRequest& request) noexcept {
    return request.timeRatio == 1.0 && request.pitchSemitones == 0.0 && request.keyFrames.empty();
}

Planar stretch(const Planar& in, const StretchRequest& request) {
    const std::size_t frames = in.frames();
    if (isIdentity(request) || frames == 0 || in.channels.empty()) {
        return in;
    }
    const auto channels = in.channels.size();
    const double ratio = std::max(request.timeRatio, 0.01);
    const auto wanted = static_cast<std::size_t>(std::llround(static_cast<double>(frames) * ratio));

    // The R3 engine, the archive's choice: it is the one RubberBand recommends for
    // offline work, and the only one that honours a key frame map with a pitch shift.
    // Threading off, so the result never depends on how the work was scheduled.
    using Stretcher = RubberBand::RubberBandStretcher;
    Stretcher stretcher(in.sampleRate, channels,
                        Stretcher::OptionProcessOffline | Stretcher::OptionEngineFiner |
                            Stretcher::OptionThreadingNever,
                        ratio, std::pow(2.0, request.pitchSemitones / 12.0));
    // In blocks, not in one call: R3 moves along a key frame map as it consumes input,
    // one process() call at a time, so a single call over the whole clip plays all of
    // it at the first segment's ratio. The archive passed everything at once, which was
    // right without a map and is wrong with one. How closely a marker lands follows the
    // block: a click two seconds into a 2x warp landed 66 ms early at 4096 frames, 12 ms
    // at 512 and 8.5 ms at 256, where it is left - offline, the calls cost nothing.
    constexpr std::size_t kBlock = 256;
    stretcher.setExpectedInputDuration(frames);
    stretcher.setMaxProcessSize(kBlock);

    std::vector<const float*> inputs(channels);
    for (std::size_t c = 0; c < channels; ++c) {
        inputs[c] = in.channels[c].data();
    }
    stretcher.study(inputs.data(), frames, true);
    if (!request.keyFrames.empty()) {
        std::map<std::size_t, std::size_t> map;
        for (const auto& [from, to] : request.keyFrames) {
            map.emplace(from, to);
        }
        stretcher.setKeyFrameMap(map);
    }

    Planar out;
    out.sampleRate = in.sampleRate;
    out.sourceChannels = in.sourceChannels;
    out.channels.assign(channels, {});
    std::vector<std::vector<float>> chunk(channels);
    std::vector<float*> pointers(channels);
    const auto drain = [&] {
        int available = 0;
        while ((available = stretcher.available()) > 0) {
            for (std::size_t c = 0; c < channels; ++c) {
                chunk[c].resize(static_cast<std::size_t>(available));
                pointers[c] = chunk[c].data();
            }
            const std::size_t got =
                stretcher.retrieve(pointers.data(), static_cast<std::size_t>(available));
            for (std::size_t c = 0; c < channels; ++c) {
                out.channels[c].insert(out.channels[c].end(), chunk[c].begin(),
                                       chunk[c].begin() + static_cast<std::ptrdiff_t>(got));
            }
        }
        return available;
    };
    std::vector<const float*> block(channels);
    for (std::size_t at = 0; at < frames; at += kBlock) {
        const std::size_t count = std::min(kBlock, frames - at);
        for (std::size_t c = 0; c < channels; ++c) {
            block[c] = inputs[c] + at;
        }
        stretcher.process(block.data(), count, at + count >= frames);
        static_cast<void>(drain());
    }
    // Single-threaded, the last process() has done the work; available() then goes to
    // -1. The bound is the archive's guard against a stretcher that never says so.
    for (std::size_t idle = 0; idle < 1000 && drain() == 0; ++idle) {
    }
    for (std::vector<float>& channel : out.channels) {
        channel.resize(wanted, 0.0F);
    }
    return out;
}

} // namespace adx::clip

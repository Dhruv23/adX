// adx-thread: main
#include "engine/clip/Destructive.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::clip {

Planar toPlanar(const format::SampleBuffer& buffer) {
    Planar out;
    out.sampleRate = buffer.sampleRate;
    out.sourceChannels = buffer.sourceChannels;
    const std::span<const float> left = buffer.leftAudio();
    out.channels.emplace_back(left.begin(), left.end());
    if (!buffer.right.empty()) {
        const std::span<const float> right = buffer.rightAudio();
        out.channels.emplace_back(right.begin(), right.end());
    }
    return out;
}

format::SampleBuffer fromPlanar(const Planar& planar) {
    format::SampleBuffer out;
    out.sampleRate = planar.sampleRate;
    out.sourceChannels = planar.sourceChannels;
    out.frames = planar.frames();
    const auto guarded = [&planar](const std::vector<float>& channel) {
        std::vector<float> padded(planar.frames() + (2 * format::kSampleGuardFrames), 0.0F);
        std::ranges::copy(channel, padded.begin() + format::kSampleGuardFrames);
        return padded;
    };
    if (!planar.channels.empty()) {
        out.left = guarded(planar.channels[0]);
    }
    if (planar.channels.size() > 1) {
        out.right = guarded(planar.channels[1]);
    }
    return out;
}

void reverse(Planar& audio) noexcept {
    for (std::vector<float>& channel : audio.channels) {
        std::ranges::reverse(channel);
    }
}

void normalize(Planar& audio, float peakDb) noexcept {
    float peak = 0.0F;
    for (const std::vector<float>& channel : audio.channels) {
        for (const float sample : channel) {
            peak = std::max(peak, std::abs(sample));
        }
    }
    if (peak <= 0.0F) {
        return;
    }
    const auto scale = static_cast<float>(dsp::dbToGain(peakDb) / static_cast<double>(peak));
    for (std::vector<float>& channel : audio.channels) {
        for (float& sample : channel) {
            sample *= scale;
        }
    }
}

void removeDc(Planar& audio) noexcept {
    for (std::vector<float>& channel : audio.channels) {
        if (channel.empty()) {
            continue;
        }
        // In double: the mean of a long file in float loses the small offset it is
        // looking for under the rounding of the large sum.
        double sum = 0.0;
        for (const float sample : channel) {
            sum += sample;
        }
        const auto mean = static_cast<float>(sum / static_cast<double>(channel.size()));
        for (float& sample : channel) {
            sample -= mean;
        }
    }
}

void applyGain(Planar& audio, float db) noexcept {
    const auto gain = static_cast<float>(dsp::dbToGain(db));
    for (std::vector<float>& channel : audio.channels) {
        for (float& sample : channel) {
            sample *= gain;
        }
    }
}

void trimSilence(Planar& audio, float thresholdDb) {
    const std::size_t frames = audio.frames();
    const auto threshold = static_cast<float>(dsp::dbToGain(thresholdDb));
    const auto loud = [&audio, threshold](std::size_t frame) {
        return std::ranges::any_of(audio.channels, [frame, threshold](const std::vector<float>& c) {
            return std::abs(c[frame]) >= threshold;
        });
    };
    std::size_t first = 0;
    while (first < frames && !loud(first)) {
        ++first;
    }
    std::size_t last = frames;
    while (last > first && !loud(last - 1)) {
        --last;
    }
    for (std::vector<float>& channel : audio.channels) {
        channel = std::vector<float>(channel.begin() + static_cast<std::ptrdiff_t>(first),
                                     channel.begin() + static_cast<std::ptrdiff_t>(last));
    }
}

format::SampleBuffer applyEdits(const format::SampleBuffer& source,
                                std::span<const project::ClipEdit> edits) {
    Planar audio = toPlanar(source);
    for (const project::ClipEdit& edit : edits) {
        switch (edit.kind) {
        case project::EditKind::Reverse:
            reverse(audio);
            break;
        case project::EditKind::Normalize:
            normalize(audio, edit.amount);
            break;
        case project::EditKind::RemoveDc:
            removeDc(audio);
            break;
        case project::EditKind::Gain:
            applyGain(audio, edit.amount);
            break;
        case project::EditKind::TrimSilence:
            trimSilence(audio, edit.amount);
            break;
        }
    }
    return fromPlanar(audio);
}

} // namespace adx::clip

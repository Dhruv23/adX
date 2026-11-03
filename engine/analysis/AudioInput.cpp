// adx-thread: main
#include "engine/analysis/AudioInput.h"

#include "engine/dsp/Resample.h"
#include "engine/format/audio/AudioFileLoader.h"

namespace adx::analysis {

std::vector<float> resampleMono(std::span<const float> mono, std::uint32_t from, std::uint32_t to) {
    if (from == to || from == 0 || to == 0) {
        return {mono.begin(), mono.end()};
    }
    dsp::Resampler resampler;
    resampler.prepare(from, to);
    std::vector<float> out(resampler.outputLength(mono.size()));
    resampler.process(mono, out);
    return out;
}

bool loadMono(const std::filesystem::path& path, std::uint32_t sampleRate, MonoAudio& out,
              std::string& error) {
    format::SampleBuffer buffer;
    if (!format::decodeFile(path, buffer, error)) {
        return false;
    }
    const std::span<const float> left = buffer.leftAudio();
    const std::span<const float> right = buffer.rightAudio();
    std::vector<float> mono(left.size());
    for (std::size_t i = 0; i < mono.size(); ++i) {
        mono[i] = 0.5F * (left[i] + right[i]);
    }
    const std::uint32_t rate = sampleRate == 0 ? buffer.sampleRate : sampleRate;
    out.samples =
        rate == buffer.sampleRate ? std::move(mono) : resampleMono(mono, buffer.sampleRate, rate);
    out.sampleRate = rate;
    return true;
}

} // namespace adx::analysis

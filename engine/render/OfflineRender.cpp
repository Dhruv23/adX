#include "engine/render/OfflineRender.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>

#include "engine/audio/OfflineBackend.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/instruments/voice/VoiceRenderCache.h"
#include "engine/project/Project.h"
#include "engine/render/RenderEngine.h"
#include "engine/rt/Violation.h"

namespace adx::render {

void measure(std::span<const float> samples, RenderStats& stats) noexcept {
    float peak = 0.0F;
    double squares = 0.0;
    for (const float sample : samples) {
        peak = std::max(peak, std::abs(sample));
        squares += static_cast<double>(sample) * sample;
    }
    stats.peak = peak;
    stats.rms = samples.empty() ? 0.0 : std::sqrt(squares / static_cast<double>(samples.size()));
    stats.hash = hashSamples(samples);
}

std::vector<float> renderOffline(const project::Project& project,
                                 const OfflineRenderOptions& options, RenderStats& stats) {
    const auto startedAt = std::chrono::steady_clock::now();
    stats = RenderStats{};
    stats.sampleRate = options.sampleRate;
    stats.blockFrames = options.blockFrames;
    stats.channels = options.channels;

    const core::Ticks end = options.end > options.start ? options.end : project.contentLength();
    const std::int64_t first = project.tempo.toSamples(options.start, options.sampleRate).value;
    const std::int64_t last = project.tempo.toSamples(end, options.sampleRate).value;
    const auto tail = static_cast<std::int64_t>(
        std::llround(options.tailSeconds * static_cast<double>(options.sampleRate)));
    const std::int64_t frames = options.frames > 0 ? static_cast<std::int64_t>(options.frames)
                                                   : std::max<std::int64_t>(last - first, 0) +
                                                         std::max<std::int64_t>(tail, 0);

    auto backend = std::make_unique<audio::OfflineBackend>();
    audio::OfflineBackend& offline = *backend;
    RenderEngine engine{std::move(backend), EngineOptions{.sampleRate = options.sampleRate,
                                                          .blockFrames = options.blockFrames,
                                                          .outputChannels = options.channels}};
    if (engine.open().code != audio::Error::Code::None) {
        stats.error = "could not open the offline backend";
        return {};
    }
    const CommitResult committed = engine.setProject(project, 0);
    if (!committed.rebuilt) {
        stats.error = committed.error;
        return {};
    }
    // Samples decode asynchronously and a voice plays silence until its sample is
    // ready. A realtime session tolerates that; a render must not, or the same
    // project would export differently depending on the decoder's speed. So it waits
    // (phase_4.md §4.11, and §4.13's "export waits for the cache").
    format::SamplePool::global().waitAll();
    instruments::VoiceRenderCache::global().waitAll();
    // An export plays straight through: the project's loop is a playback aid, not
    // part of the render. Stated explicitly so the render does not depend on whatever
    // the transport defaults to.
    engine.setLoop(transport::LoopRegion{});
    engine.seek(options.start);
    engine.play();
    static_cast<void>(engine.start());

    const std::uint64_t violationsBefore = rt::ViolationLog::instance().count();
    const std::span<const float> rendered =
        offline.renderFrames(static_cast<std::uint64_t>(frames));
    stats.violations = rt::ViolationLog::instance().count() - violationsBefore;

    std::vector<float> out(rendered.begin(), rendered.end());
    engine.stop();
    engine.pump();

    stats.frames = static_cast<std::uint64_t>(frames);
    stats.callbacks = engine.audio().info().callbackCount;
    stats.latencySamples = engine.latencySamples();
    stats.arenaHighWater = engine.audio().arenaHighWaterMark();
    stats.arenaCapacity = engine.audio().callbacks().arenaCapacity();
    measure(out, stats);
    stats.wallSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
    return out;
}

namespace {

void putLe(std::ofstream& file, std::uint32_t value, int bytes) {
    std::array<char, 4> buffer{};
    for (int i = 0; i < bytes; ++i) {
        buffer[static_cast<std::size_t>(i)] =
            static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU);
    }
    file.write(buffer.data(), bytes);
}

} // namespace

bool writeWavFloat32(const std::string& path, std::span<const float> samples,
                     std::uint32_t channels, std::uint32_t sampleRate) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    constexpr std::uint32_t kBytesPerSample = 4;
    constexpr std::uint32_t kFormatIeeeFloat = 3;
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * kBytesPerSample);

    file.write("RIFF", 4);
    putLe(file, 36 + dataBytes, 4);
    file.write("WAVEfmt ", 8);
    putLe(file, 16, 4);
    putLe(file, kFormatIeeeFloat, 2);
    putLe(file, channels, 2);
    putLe(file, sampleRate, 4);
    putLe(file, sampleRate * channels * kBytesPerSample, 4);
    putLe(file, channels * kBytesPerSample, 2);
    putLe(file, 32, 2);
    file.write("data", 4);
    putLe(file, dataBytes, 4);
    for (const float sample : samples) {
        std::array<char, sizeof(float)> bytes{};
        std::memcpy(bytes.data(), &sample, sizeof(float));
        file.write(bytes.data(), bytes.size());
    }
    return static_cast<bool>(file);
}

} // namespace adx::render

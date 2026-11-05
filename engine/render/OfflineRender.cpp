#include "engine/render/OfflineRender.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>

#include "engine/audio/OfflineBackend.h"
#include "engine/clip/ClipEdit.h"
#include "engine/clip/ClipRenderCache.h"
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

struct OfflineSession::Impl {
    format::SamplePool& pool = format::SamplePool::global();
    std::vector<std::shared_ptr<const void>> pins;
    audio::OfflineBackend* backend{nullptr};
    std::unique_ptr<RenderEngine> engine;
    OfflineRenderOptions options;
    std::uint64_t rangeFrames{0};
    std::uint64_t tailFrames{0};
    std::uint64_t rendered{0};
    std::uint64_t violationsBefore{0};
    std::chrono::steady_clock::time_point startedAt;
    std::string error;
};

OfflineSession::OfflineSession(const project::Project& project, const OfflineRenderOptions& options)
    : m_impl(std::make_unique<Impl>()) {
    Impl& s = *m_impl;
    s.startedAt = std::chrono::steady_clock::now();
    s.options = options;

    // Audio clips first: their files decode and their stretches render before the
    // engine's first snapshot is built, so that snapshot already plays them - and the
    // arrangement's end includes the last clip's, which only the files know.
    clip::ClipRenderCache& clips = clip::ClipRenderCache::global();
    s.pins = clip::prefetchAudio(project, options.sampleRate, s.pool, clips);
    s.pool.waitAll();
    clips.waitAll();

    const std::int64_t first = project.tempo.toSamples(options.start, options.sampleRate).value;
    std::int64_t last = 0;
    if (options.end > options.start) {
        last = project.tempo.toSamples(options.end, options.sampleRate).value;
    } else {
        last = std::max(project.tempo.toSamples(project.contentLength(), options.sampleRate).value,
                        clip::audioEndSamples(project, options.sampleRate, s.pool, clips));
    }
    const auto tail = static_cast<std::int64_t>(
        std::llround(options.tailSeconds * static_cast<double>(options.sampleRate)));
    if (options.frames > 0) {
        s.rangeFrames = options.frames;
    } else {
        s.rangeFrames = static_cast<std::uint64_t>(std::max<std::int64_t>(last - first, 0));
        s.tailFrames = static_cast<std::uint64_t>(std::max<std::int64_t>(tail, 0));
    }

    auto backend = std::make_unique<audio::OfflineBackend>();
    s.backend = backend.get();
    s.engine = std::make_unique<RenderEngine>(
        std::move(backend), EngineOptions{.sampleRate = options.sampleRate,
                                          .blockFrames = options.blockFrames,
                                          .outputChannels = options.channels,
                                          .offline = true,
                                          .renderWorkers = options.renderWorkers});
    if (s.engine->open().code != audio::Error::Code::None) {
        s.error = "could not open the offline backend";
        return;
    }
    s.engine->builder().setStem(options.stem);
    const CommitResult committed = s.engine->setProject(project, 0);
    if (!committed.rebuilt) {
        s.error = committed.error;
        return;
    }
    // Samples decode asynchronously and a voice plays silence until its sample is
    // ready. A realtime session tolerates that; a render must not, or the same
    // project would export differently depending on the decoder's speed. So it waits
    // (phase_4.md §4.11, and §4.13's "export waits for the cache").
    s.pool.waitAll();
    instruments::VoiceRenderCache::global().waitAll();
    // An export plays straight through: the project's loop is a playback aid, not
    // part of the render. Stated explicitly so the render does not depend on whatever
    // the transport defaults to.
    s.engine->setLoop(transport::LoopRegion{});
    s.engine->seek(options.start);
    s.engine->play();
    static_cast<void>(s.engine->start());
    s.violationsBefore = rt::ViolationLog::instance().count();
}

OfflineSession::~OfflineSession() {
    if (m_impl->engine) {
        m_impl->engine->stop();
        m_impl->engine->pump();
    }
}

const std::string& OfflineSession::error() const noexcept {
    return m_impl->error;
}

std::uint64_t OfflineSession::rangeFrames() const noexcept {
    return m_impl->rangeFrames;
}

std::uint64_t OfflineSession::tailFrames() const noexcept {
    return m_impl->tailFrames;
}

std::uint64_t OfflineSession::renderedFrames() const noexcept {
    return m_impl->rendered;
}

std::span<const float> OfflineSession::render(std::uint64_t frames) {
    Impl& s = *m_impl;
    if (!s.error.empty() || frames == 0) {
        return {};
    }
    s.rendered += frames;
    return s.backend->renderFrames(frames);
}

void OfflineSession::finish(RenderStats& stats) const {
    const Impl& s = *m_impl;
    stats.sampleRate = s.options.sampleRate;
    stats.blockFrames = s.options.blockFrames;
    stats.channels = s.options.channels;
    stats.error = s.error;
    stats.frames = s.rendered;
    if (!s.engine) {
        return;
    }
    stats.violations = rt::ViolationLog::instance().count() - s.violationsBefore;
    stats.callbacks = s.engine->audio().info().callbackCount;
    stats.latencySamples = s.engine->latencySamples();
    stats.arenaHighWater = s.engine->audio().arenaHighWaterMark();
    stats.arenaCapacity = s.engine->audio().callbacks().arenaCapacity();
    stats.parallelPieces = s.engine->core().scheduler().stats().parallelPieces;
    stats.wallSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - s.startedAt).count();
}

std::vector<float> renderOffline(const project::Project& project,
                                 const OfflineRenderOptions& options, RenderStats& stats) {
    stats = RenderStats{};
    OfflineSession session(project, options);
    if (!session.error().empty()) {
        session.finish(stats);
        return {};
    }
    const std::span<const float> rendered =
        session.render(session.rangeFrames() + session.tailFrames());
    std::vector<float> out(rendered.begin(), rendered.end());
    session.finish(stats);
    measure(out, stats);
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

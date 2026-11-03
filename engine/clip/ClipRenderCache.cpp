// adx-thread: main
#include "engine/clip/ClipRenderCache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>

#include "engine/clip/Destructive.h"
#include "engine/clip/Stretch.h"
#include "engine/dsp/Resample.h"
#include "engine/format/adx/Value.h"

namespace adx::clip {

std::string ClipRenderKey::text() const {
    std::string out = path.generic_string();
    out += "|r";
    out += std::to_string(sampleRate);
    for (const project::ClipEdit& edit : edits) {
        out += '|';
        out += project::toString(edit.kind);
        out += format::formatFloat(edit.amount);
    }
    if (reverse) {
        out += "|rev";
    }
    if (warp.size() >= 2) {
        for (const auto& [source, output] : warp) {
            out += "|w";
            out += format::formatDouble(source);
            out += ':';
            out += format::formatDouble(output);
        }
    } else if (stretch != 1.0) {
        out += "|s";
        out += format::formatDouble(stretch);
    }
    if (pitchSemitones != 0.0) {
        out += "|p";
        out += format::formatDouble(pitchSemitones);
    }
    return out;
}

std::shared_ptr<const format::SampleBuffer> ClipRenderEntry::buffer() const {
    return m_handle.ready() ? m_buffer : nullptr;
}

namespace {

void resampleTo(Planar& audio, std::uint32_t rate) {
    if (audio.sampleRate == rate || audio.sampleRate == 0 || rate == 0) {
        return;
    }
    dsp::Resampler resampler;
    resampler.prepare(audio.sampleRate, rate);
    for (std::vector<float>& channel : audio.channels) {
        std::vector<float> converted(resampler.outputLength(channel.size()));
        resampler.process(channel, converted);
        channel = std::move(converted);
    }
    audio.sampleRate = rate;
}

} // namespace

format::SampleBuffer renderClip(const format::SampleBuffer& source, const ClipRenderKey& key) {
    Planar audio = toPlanar(applyEdits(source, key.edits));
    if (key.reverse) {
        clip::reverse(audio);
    }
    resampleTo(audio, key.sampleRate);

    const double rate = audio.sampleRate;
    const auto frames = static_cast<double>(audio.frames());
    StretchRequest request{
        .timeRatio = key.stretch, .pitchSemitones = key.pitchSemitones, .keyFrames = {}};
    std::vector<std::pair<std::size_t, std::size_t>> keyFrames;
    if (key.warp.size() >= 2 && frames > 0) {
        // The overall ratio extends the last segment's to the end of the material; the
        // compile step already moved every output time so source 0 lands at output 0.
        const auto& [s0, o0] = key.warp[key.warp.size() - 2];
        const auto& [s1, o1] = key.warp.back();
        const double tailRatio = s1 > s0 ? (o1 - o0) / (s1 - s0) : 1.0;
        const double total = o1 + (std::max(0.0, (frames / rate) - s1) * tailRatio);
        request.timeRatio = total / (frames / rate);
        for (const auto& [s, o] : key.warp) {
            const double in = s * rate;
            if (in <= 0.0 || in >= frames) {
                continue;
            }
            keyFrames.emplace_back(static_cast<std::size_t>(std::llround(in)),
                                   static_cast<std::size_t>(std::llround(o * rate)));
        }
        request.keyFrames = keyFrames;
    }
    if (!isIdentity(request)) {
        audio = stretch(audio, request);
    }
    return fromPlanar(audio);
}

ClipRenderCache::ClipRenderCache(unsigned workers) {
    for (unsigned i = 0; i < std::max(1U, workers); ++i) {
        m_workers.emplace_back([this](const std::stop_token& stop) { work(stop); });
    }
}

ClipRenderCache::~ClipRenderCache() {
    for (std::jthread& worker : m_workers) {
        worker.request_stop();
    }
    m_wake.notify_all();
}

ClipRenderCache& ClipRenderCache::global() {
    static ClipRenderCache cache;
    return cache;
}

std::shared_ptr<const ClipRenderEntry> ClipRenderCache::request(const ClipRenderKey& key,
                                                                format::SamplePool& pool) {
    std::string text = key.text();
    const std::lock_guard lock(m_mutex);
    if (const auto found = m_entries.find(text); found != m_entries.end()) {
        if (std::shared_ptr<ClipRenderEntry> live = found->second.lock()) {
            return live;
        }
    }
    auto entry = std::make_shared<ClipRenderEntry>();
    entry->m_source = pool.request(key.path);
    entry->m_key = text;
    m_entries[std::move(text)] = entry;
    m_queue.push_back(Job{.entry = entry, .key = key});
    m_wake.notify_one();
    return entry;
}

void ClipRenderCache::waitAll() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return m_queue.empty() && m_running == 0; });
}

std::uint64_t ClipRenderCache::rendersDone() const {
    const std::lock_guard lock(m_mutex);
    return m_done;
}

void ClipRenderCache::render(ClipRenderEntry& entry, const ClipRenderKey& key) {
    const std::shared_ptr<const format::SampleBuffer> source = entry.m_source->buffer();
    if (!source) {
        entry.m_error = entry.m_source->error();
        entry.m_handle.fail();
        return;
    }
    const bool plain = key.edits.empty() && !key.reverse && key.stretch == 1.0 &&
                       key.pitchSemitones == 0.0 && key.warp.size() < 2 &&
                       source->sampleRate == key.sampleRate;
    // Nothing to do is no copy: the clip plays the decoded buffer itself.
    entry.m_buffer =
        plain ? source : std::make_shared<const format::SampleBuffer>(renderClip(*source, key));
    entry.m_handle.publish(entry.m_buffer->view());
}

void ClipRenderCache::work(const std::stop_token& stop) {
    std::unique_lock lock(m_mutex);
    while (!stop.stop_requested()) {
        if (!m_wake.wait(lock, stop, [this] { return !m_queue.empty(); })) {
            return;
        }
        Job job = std::move(m_queue.front());
        m_queue.pop_front();
        std::shared_ptr<ClipRenderEntry> const entry = job.entry.lock();
        if (!entry) {
            if (m_queue.empty() && m_running == 0) {
                m_idle.notify_all();
            }
            continue;
        }
        if (entry->m_source->handle().state() == format::SampleState::Loading) {
            // Its file is still decoding: back of the queue, and give the decoder a
            // moment rather than spinning on it.
            m_queue.push_back(std::move(job));
            m_wake.wait_for(lock, stop, std::chrono::milliseconds(2), [] { return false; });
            continue;
        }
        ++m_running;
        lock.unlock();
        render(*entry, job.key);
        lock.lock();
        --m_running;
        ++m_done;
        if (m_queue.empty() && m_running == 0) {
            m_idle.notify_all();
        }
    }
}

} // namespace adx::clip

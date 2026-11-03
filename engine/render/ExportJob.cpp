// adx-thread: main
#include "engine/render/ExportJob.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Loudness.h"
#include "engine/dsp/Math.h"
#include "engine/dsp/Resample.h"
#include "engine/format/midi/MidiExporter.h"
#include "engine/project/Project.h"

namespace adx::render {

namespace {

/// Rendered per pass: whole blocks, so the chunking never moves a block boundary.
constexpr std::uint64_t kBlocksPerPass = 64;

struct Source {
    std::string label;
    const graph::StemSpec* stem{nullptr};
};

std::filesystem::path besideOutput(const std::filesystem::path& output, const std::string& label) {
    std::string clean;
    for (const char c : label) {
        // Characters a Windows file name cannot hold.
        clean += std::string_view{"<>:\"/\\|?*"}.find(c) == std::string_view::npos ? c : '_';
    }
    return output.parent_path() /
           (output.stem().string() + " - " + clean + output.extension().string());
}

/// Renders one source over the range, with the request's tail. A fixed tail renders
/// exactly; "until silence" renders on until the output has been quiet for the hold
/// time and ends at the last sound - or, when `forcedTail` is set, renders exactly
/// that, so stems end where the master ended.
bool renderSource(const project::Project& project, const ExportRequest& request,
                  const Source& source, core::Ticks from, core::Ticks to,
                  std::optional<std::uint64_t> forcedTail, ExportControl* control,
                  float progressFrom, float progressSpan, std::vector<float>& out,
                  std::uint64_t& tailFrames, std::string& error) {
    OfflineRenderOptions options;
    options.sampleRate = request.renderRate;
    options.blockFrames = request.blockFrames;
    options.start = from;
    options.end = to;
    options.tailSeconds = request.tailUntilSilence ? 0.0 : request.tailSeconds;
    options.stem = source.stem;
    OfflineSession session(project, options);
    if (!session.error().empty()) {
        error = session.error();
        return false;
    }
    const std::uint64_t pass = static_cast<std::uint64_t>(request.blockFrames) * kBlocksPerPass;
    const std::uint64_t range = session.rangeFrames();
    std::uint64_t wanted = range + session.tailFrames();
    const bool open = request.tailUntilSilence && !forcedTail;
    if (forcedTail) {
        wanted = range + *forcedTail;
    } else if (open) {
        wanted = range + static_cast<std::uint64_t>(request.maxTailSeconds * request.renderRate);
    }
    const auto threshold = static_cast<float>(dsp::dbToGain(request.silenceDb));
    const auto hold = static_cast<std::uint64_t>(request.silenceHoldSeconds * request.renderRate);
    std::uint64_t lastLoud = range;
    out.clear();
    out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(wanted, range + pass)) * 2);
    while (session.renderedFrames() < wanted) {
        if (control != nullptr && control->cancel.load(std::memory_order_relaxed)) {
            return false;
        }
        const std::uint64_t at = session.renderedFrames();
        const std::span<const float> piece = session.render(std::min(pass, wanted - at));
        out.insert(out.end(), piece.begin(), piece.end());
        if (open) {
            for (std::size_t i = 0; i < piece.size(); ++i) {
                if (std::abs(piece[i]) > threshold) {
                    lastLoud = std::max(lastLoud, at + (i / 2) + 1);
                }
            }
            if (at + (piece.size() / 2) >= range && at + (piece.size() / 2) - lastLoud >= hold) {
                break;
            }
        }
        if (control != nullptr) {
            const float done = static_cast<float>(session.renderedFrames()) /
                               static_cast<float>(std::max<std::uint64_t>(wanted, 1));
            control->progress.store(progressFrom + (progressSpan * std::min(done, 1.0F)),
                                    std::memory_order_relaxed);
        }
    }
    if (open) {
        out.resize(static_cast<std::size_t>(lastLoud) * 2);
    }
    tailFrames = out.size() / 2 - std::min<std::uint64_t>(range, out.size() / 2);
    return true;
}

void resample(std::vector<float>& audio, std::uint32_t from, std::uint32_t to) {
    if (from == to || to == 0) {
        return;
    }
    dsp::Resampler resampler;
    resampler.prepare(from, to);
    const std::size_t frames = audio.size() / 2;
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = audio[i * 2];
        right[i] = audio[(i * 2) + 1];
    }
    const std::size_t length = resampler.outputLength(frames);
    std::vector<float> outLeft(length);
    std::vector<float> outRight(length);
    resampler.process(left, outLeft);
    resampler.process(right, outRight);
    audio.resize(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
        audio[i * 2] = outLeft[i];
        audio[(i * 2) + 1] = outRight[i];
    }
}

double integratedLoudness(const std::vector<float>& audio, std::uint32_t rate) {
    dsp::LoudnessMeter meter;
    meter.prepare(rate);
    const std::size_t frames = audio.size() / 2;
    constexpr std::size_t kChunk = 4096;
    std::vector<float> left(kChunk);
    std::vector<float> right(kChunk);
    for (std::size_t at = 0; at < frames; at += kChunk) {
        const std::size_t count = std::min(kChunk, frames - at);
        for (std::size_t i = 0; i < count; ++i) {
            left[i] = audio[(at + i) * 2];
            right[i] = audio[((at + i) * 2) + 1];
        }
        meter.process(std::span<const float>{left}.first(count),
                      std::span<const float>{right}.first(count));
    }
    return meter.integrated();
}

} // namespace

const char* toString(RangeKind kind) noexcept {
    switch (kind) {
    case RangeKind::Project:
        return "project";
    case RangeKind::Loop:
        return "loop";
    case RangeKind::Markers:
        return "markers";
    case RangeKind::Selection:
        return "selection";
    }
    return "";
}

const char* toString(NormalizeKind kind) noexcept {
    switch (kind) {
    case NormalizeKind::Off:
        return "off";
    case NormalizeKind::Peak:
        return "peak";
    case NormalizeKind::Lufs:
        return "lufs";
    }
    return "";
}

bool resolveRange(const project::Project& project, const ExportRequest& request, core::Ticks& from,
                  core::Ticks& to, std::string& error) {
    from = core::Ticks{0};
    to = core::Ticks{0};
    switch (request.range) {
    case RangeKind::Project:
        return true;
    case RangeKind::Loop:
        if (project.meta.loopEnd <= project.meta.loopStart) {
            error = "the project has no loop region";
            return false;
        }
        from = project.meta.loopStart;
        to = project.meta.loopEnd;
        return true;
    case RangeKind::Markers: {
        const auto named = [&project](const std::string& name) -> const project::Marker* {
            const auto found = std::ranges::find(project.markers, name, &project::Marker::name);
            return found == project.markers.end() ? nullptr : &*found;
        };
        const project::Marker* start = named(request.fromMarker);
        if (start == nullptr) {
            error = "no marker named '" + request.fromMarker + "'";
            return false;
        }
        from = start->at;
        if (!request.toMarker.empty()) {
            const project::Marker* end = named(request.toMarker);
            if (end == nullptr || end->at <= start->at) {
                error =
                    "no marker named '" + request.toMarker + "' after '" + request.fromMarker + "'";
                return false;
            }
            to = end->at;
        }
        return true;
    }
    case RangeKind::Selection:
        if (request.to <= request.from) {
            error = "the selection is empty";
            return false;
        }
        from = request.from;
        to = request.to;
        return true;
    }
    return true;
}

// NOLINTNEXTLINE(readability-function-size)
ExportResult runExport(const project::Project& project, const ExportRequest& request,
                       ExportControl* control) {
    ExportResult result;
    core::Ticks from;
    core::Ticks to;
    if (!resolveRange(project, request, from, to, result.error)) {
        return result;
    }
    EncodeSettings encode = request.encode;
    if (encode.sampleRate == 0) {
        encode.sampleRate = request.renderRate;
    }
    encode.channels = 2;

    // The stems, and the master - rendered first whenever anything depends on it: a
    // normalization gain, or a tail the stems must match.
    std::vector<Stem> stems;
    if (request.stems) {
        stems = stemsOf(project, *request.stems);
    }
    std::vector<Source> sources;
    const bool needMaster = request.master || request.normalize != NormalizeKind::Off ||
                            (request.tailUntilSilence && !stems.empty());
    if (needMaster) {
        sources.push_back(Source{.label = "master", .stem = nullptr});
    }
    for (const Stem& stem : stems) {
        sources.push_back(Source{.label = stem.name, .stem = &stem.spec});
    }
    if (sources.empty()) {
        result.error = "nothing to export: no master and no stems";
        return result;
    }

    // Where the range splits, as sample offsets from its start at the render rate.
    std::vector<std::pair<std::int64_t, std::string>> cuts;
    if (request.splitAtMarkers) {
        const std::int64_t base = project.tempo.toSamples(from, request.renderRate).value;
        for (const project::Marker& marker : project.markers) {
            if (marker.at > from && (to.value <= from.value || marker.at < to)) {
                cuts.emplace_back(project.tempo.toSamples(marker.at, request.renderRate).value -
                                      base,
                                  marker.name);
            }
        }
        std::ranges::sort(cuts);
    }

    const std::uint32_t bits = integerBits(encode.format, encode.flacBits);
    std::optional<std::uint64_t> masterTail;
    double gain = 1.0;
    const float span = 1.0F / static_cast<float>(sources.size());
    for (std::size_t s = 0; s < sources.size(); ++s) {
        const Source& source = sources[s];
        std::vector<float> audio;
        std::uint64_t tail = 0;
        if (!renderSource(project, request, source, from, to, masterTail, control,
                          span * static_cast<float>(s), span, audio, tail, result.error)) {
            result.cancelled = control != nullptr && control->cancel.load();
            return result;
        }
        if (source.stem == nullptr) {
            masterTail = tail;
        }
        resample(audio, request.renderRate, encode.sampleRate);

        if (source.stem == nullptr && request.normalize != NormalizeKind::Off) {
            result.loudness = integratedLoudness(audio, encode.sampleRate);
            if (request.normalize == NormalizeKind::Peak) {
                float peak = 0.0F;
                for (const float sample : audio) {
                    peak = std::max(peak, std::abs(sample));
                }
                result.gainDb = peak > 0.0F ? request.normalizeTarget - dsp::gainToDb(peak) : 0.0;
            } else if (result.loudness > dsp::kSilentLufs + 1.0F) {
                result.gainDb = request.normalizeTarget - result.loudness;
            }
            gain = dsp::dbToGain(result.gainDb);
        }
        if (gain != 1.0) {
            for (float& sample : audio) {
                sample = static_cast<float>(sample * gain);
            }
        }
        if (bits > 0 && request.dither != DitherKind::Off) {
            quantize(audio, 2, bits, request.dither);
        }
        if (source.stem == nullptr && !request.master) {
            continue; // rendered for its gain or its tail only
        }

        // One file, or one per stretch between cuts.
        const std::filesystem::path base =
            source.stem == nullptr ? request.output : besideOutput(request.output, source.label);
        const double ratio = static_cast<double>(encode.sampleRate) / request.renderRate;
        std::vector<std::pair<std::size_t, std::string>> pieces{{0, source.label}};
        for (const auto& [at, name] : cuts) {
            pieces.emplace_back(
                static_cast<std::size_t>(std::llround(static_cast<double>(at) * ratio)), name);
        }
        const std::size_t frames = audio.size() / 2;
        for (std::size_t p = 0; p < pieces.size(); ++p) {
            const std::size_t begin = std::min(pieces[p].first, frames);
            const std::size_t end =
                p + 1 < pieces.size() ? std::min(pieces[p + 1].first, frames) : frames;
            const std::filesystem::path path =
                cuts.empty()
                    ? base
                    : besideOutput(base, (p < 9 ? "0" : "") + std::to_string(p + 1) + " " +
                                             (p == 0 ? std::string{"start"} : pieces[p].second));
            const std::span<const float> slice =
                std::span<const float>{audio}.subspan(begin * 2, (end - begin) * 2);
            if (!encodeFile(path, slice, encode, result.error)) {
                return result;
            }
            ExportedFile file{.path = path,
                              .label = cuts.empty() ? source.label : pieces[p].second,
                              .frames = end - begin,
                              .sampleRate = encode.sampleRate,
                              .stats = {}};
            file.stats.sampleRate = encode.sampleRate;
            file.stats.channels = 2;
            file.stats.frames = end - begin;
            measure(slice, file.stats);
            result.files.push_back(std::move(file));
        }
    }
    if (request.writeMidi) {
        std::filesystem::path midi = request.output;
        midi.replace_extension(".mid");
        if (!format::midi::exportMidiFile(project, midi, result.error)) {
            return result;
        }
        ExportedFile side;
        side.path = midi;
        side.label = "midi";
        result.files.push_back(std::move(side));
    }
    if (control != nullptr) {
        control->progress.store(1.0F, std::memory_order_relaxed);
    }
    result.ok = true;
    return result;
}

ExportJob::ExportJob(const project::Project& project, ExportRequest request)
    : m_project(std::make_unique<project::Project>(project)), m_request(std::move(request)) {
    m_thread = std::thread([this] {
        m_result = runExport(*m_project, m_request, &m_control);
        m_done.store(true, std::memory_order_release);
    });
}

ExportJob::~ExportJob() {
    cancel();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

const ExportResult& ExportJob::wait() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    return m_result;
}

} // namespace adx::render

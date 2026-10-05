// adx-thread: main
#include "engine/instruments/voice/UtauResampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

#include <world/synthesis.h>

#include "engine/dsp/Math.h"
#include "engine/dsp/Resample.h"
#include "engine/instruments/voice/WorldAnalysis.h"

namespace adx::instruments {
namespace {

constexpr double kFadeInMs = 5.0;
/// Where P (peak compression) at 100 brings a note's peak.
constexpr double kPeakTarget = 0.6;

std::uint64_t mix(std::uint64_t h, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ bytes[i]) * 1099511628211ULL;
    }
    return h;
}

template<class T> std::uint64_t mixValue(std::uint64_t h, const T& value) noexcept {
    return mix(h, &value, sizeof(T));
}

/// Row `at` (fractional) of a frames x bins matrix, linear between frames, with the
/// frequency axis read at `warp` times each bin (gender).
void interpolateRow(const std::vector<std::vector<double>>& rows, double at, double warp,
                    std::vector<double>& out) {
    const std::size_t last = rows.size() - 1;
    const double clamped = std::clamp(at, 0.0, static_cast<double>(last));
    const auto a = static_cast<std::size_t>(clamped);
    const std::size_t b = std::min(a + 1, last);
    const double t = clamped - static_cast<double>(a);
    const std::size_t bins = out.size();
    for (std::size_t k = 0; k < bins; ++k) {
        const double source =
            std::min(static_cast<double>(k) * warp, static_cast<double>(bins - 1));
        const auto k0 = static_cast<std::size_t>(source);
        const std::size_t k1 = std::min(k0 + 1, bins - 1);
        const double u = source - static_cast<double>(k0);
        const double va = rows[a][k0] + ((rows[a][k1] - rows[a][k0]) * u);
        const double vb = rows[b][k0] + ((rows[b][k1] - rows[b][k0]) * u);
        out[k] = va + ((vb - va) * t);
    }
}

} // namespace

std::uint64_t renderKey(const NoteRenderRequest& request) {
    std::uint64_t h = 1469598103934665603ULL;
    h = mixValue(h, kResamplerVersion);
    h = mixValue(h, request.bank ? request.bank->contentHash() : 0);
    h = mix(h, request.oto.alias.data(), request.oto.alias.size());
    const std::u8string wav = request.oto.wav.u8string();
    h = mix(h, wav.data(), wav.size());
    for (const double v :
         {request.oto.offset, request.oto.consonant, request.oto.cutoff, request.oto.preutterance,
          request.oto.overlap, request.noteHz, request.lengthMs, request.cutMs, request.fadeOutMs,
          request.gender, request.breathiness, request.tuningCents, request.peakCompression}) {
        h = mixValue(h, v);
    }
    h = mix(h, request.cents.data(), request.cents.size() * sizeof(float));
    return h;
}

// NOLINTNEXTLINE(readability-function-size) - analysis, time map, flags, synthesis.
NoteRender renderNote(const NoteRenderRequest& request) {
    NoteRender result;
    const std::shared_ptr<const WorldAnalysis> analysis = analyseWav(request.oto.wav);
    if (!analysis || analysis->f0.empty()) {
        return result;
    }
    const OtoEntry& oto = request.oto;
    const double fileMs = analysis->durationMs();
    const double startMs = std::clamp(oto.offset, 0.0, fileMs);
    const double consonantEndMs = std::clamp(startMs + oto.consonant, startMs, fileMs);
    const double endMs =
        std::clamp(oto.endMs(fileMs), consonantEndMs + kWorldFramePeriodMs, fileMs);
    const double preMs = std::max(0.0, oto.preutterance);
    const double totalMs = preMs + std::max(request.cutMs, kWorldFramePeriodMs);
    const double consonantMs = std::min(consonantEndMs - startMs, totalMs * 0.8);

    const std::size_t frames =
        static_cast<std::size_t>(std::ceil(totalMs / kWorldFramePeriodMs)) + 1;
    const std::size_t bins = static_cast<std::size_t>(analysis->fftSize / 2) + 1;
    std::vector<double> f0(frames, 0.0);
    std::vector<std::vector<double>> sp(frames, std::vector<double>(bins));
    std::vector<std::vector<double>> ap(frames, std::vector<double>(bins));
    // Gender: read the envelope's frequency axis stretched or squeezed. Positive g moves
    // the formants down, as UTAU's g+ deepens the voice.
    const double warp = dsp::exp2(std::clamp(request.gender, -100.0, 100.0) / 200.0);
    const double breathExponent = 50.0 / std::clamp(request.breathiness, 1.0, 100.0);
    const auto sourceFrames = static_cast<double>(analysis->f0.size() - 1);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) * kWorldFramePeriodMs;
        // Consonant at natural speed, then the vowel stretched over what is left.
        double sourceMs = startMs + t;
        if (t >= consonantMs) {
            const double remaining = std::max(totalMs - consonantMs, kWorldFramePeriodMs);
            const double u = std::min((t - consonantMs) / remaining, 1.0);
            sourceMs = startMs + consonantMs + (u * (endMs - (startMs + consonantMs)));
        }
        const double at = std::min(sourceMs / kWorldFramePeriodMs, sourceFrames);
        interpolateRow(analysis->spectrogram, at, warp, sp[i]);
        interpolateRow(analysis->aperiodicity, at, 1.0, ap[i]);
        for (double& a : ap[i]) {
            a = std::clamp(dsp::pow(std::max(a, 1e-9), breathExponent), 0.0, 1.0);
        }
        const auto nearest = static_cast<std::size_t>(std::lround(at));
        if (analysis->f0[std::min(nearest, analysis->f0.size() - 1)] > 0.0) {
            // Frames before the note's start take the note's own pitch too.
            const double noteMs = t - preMs;
            const auto curveFrame =
                static_cast<std::size_t>(std::max(0.0, noteMs / kWorldFramePeriodMs));
            // Past the end of the curve, its last value holds.
            double curve = 0.0;
            if (curveFrame < request.cents.size()) {
                curve = request.cents[curveFrame];
            } else if (!request.cents.empty()) {
                curve = request.cents.back();
            }
            const double cents = request.tuningCents + curve;
            f0[i] = request.noteHz * dsp::exp2(cents / 1200.0);
        }
    }

    const int rate = analysis->sampleRate;
    const auto length = static_cast<int>(std::ceil(totalMs * rate / 1000.0));
    std::vector<double> y(static_cast<std::size_t>(length), 0.0);
    std::vector<const double*> spRows(frames);
    std::vector<const double*> apRows(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        spRows[i] = sp[i].data();
        apRows[i] = ap[i].data();
    }
    Synthesis(f0.data(), static_cast<int>(frames), spRows.data(), apRows.data(), analysis->fftSize,
              kWorldFramePeriodMs, rate, length, y.data());

    // Fades: in over 5 ms, out over the overlap the next note crossfades with.
    const auto fadeIn = static_cast<std::size_t>(kFadeInMs * rate / 1000.0);
    const auto fadeOut =
        static_cast<std::size_t>(std::max(request.fadeOutMs, kFadeInMs) * rate / 1000.0);
    for (std::size_t n = 0; n < y.size(); ++n) {
        double gain = 1.0;
        if (n < fadeIn) {
            gain = static_cast<double>(n) / static_cast<double>(fadeIn);
        }
        const std::size_t fromEnd = y.size() - 1 - n;
        if (fromEnd < fadeOut) {
            gain *= static_cast<double>(fromEnd) / static_cast<double>(fadeOut);
        }
        y[n] *= gain;
    }
    double peak = 0.0;
    for (const double s : y) {
        peak = std::max(peak, std::abs(s));
    }
    if (peak > 0.0) {
        const double amount = std::clamp(request.peakCompression, 0.0, 100.0) / 100.0;
        const double gain = (1.0 - amount) + (amount * kPeakTarget / peak);
        for (double& s : y) {
            s *= gain;
        }
    }

    // Once, at the cache boundary: the bank's rate to the render rate.
    std::vector<float> source(y.size());
    std::ranges::transform(y, source.begin(), [](double s) { return static_cast<float>(s); });
    if (std::cmp_equal(rate, kVoiceRenderRate)) {
        result.samples = std::move(source);
    } else {
        dsp::Resampler resampler;
        resampler.prepare(static_cast<double>(rate), static_cast<double>(kVoiceRenderRate));
        result.samples.resize(resampler.outputLength(source.size()));
        resampler.process(source, result.samples);
    }
    result.lead = static_cast<std::uint32_t>(std::lround(preMs * kVoiceRenderRate / 1000.0));
    result.ok = true;
    return result;
}

} // namespace adx::instruments

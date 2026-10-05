// adx-thread: main
#include "engine/instruments/voice/WorldAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <world/cheaptrick.h>
#include <world/d4c.h>
#include <world/harvest.h>

#include "engine/format/audio/AudioFileLoader.h"

namespace adx::instruments {
namespace {

constexpr std::size_t kFrqHeader = 40;
/// A .frq whose F0 is outside this is not trusted for a frame.
constexpr double kMinF0 = 40.0;
constexpr double kMaxF0 = 1600.0;

template<class T> T read(const std::vector<char>& bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

std::vector<double> framesOf(int sampleRate, std::size_t samples) {
    const int count =
        GetSamplesForHarvest(sampleRate, static_cast<int>(samples), kWorldFramePeriodMs);
    std::vector<double> positions(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < positions.size(); ++i) {
        positions[i] = static_cast<double>(i) * kWorldFramePeriodMs / 1000.0;
    }
    return positions;
}

/// The .frq's F0 on WORLD's frames: linear between .frq frames, 0 where the .frq says
/// the frame is unvoiced (no amplitude, or an F0 out of range).
std::vector<double> frqOnFrames(const FrqFile& frq, int sampleRate,
                                const std::vector<double>& positions) {
    std::vector<double> f0(positions.size(), 0.0);
    const double hopSeconds = static_cast<double>(frq.hop) / sampleRate;
    const auto voiced = [&frq](std::size_t k) {
        return k < frq.f0.size() && frq.f0[k] >= kMinF0 && frq.f0[k] <= kMaxF0 &&
               frq.amplitude[k] > 0.0;
    };
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const double at = positions[i] / hopSeconds;
        const auto k = static_cast<std::size_t>(at);
        if (!voiced(k)) {
            continue;
        }
        const double t = at - static_cast<double>(k);
        f0[i] = voiced(k + 1) ? frq.f0[k] + ((frq.f0[k + 1] - frq.f0[k]) * t) : frq.f0[k];
    }
    return f0;
}

std::mutex& cacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::pair<std::filesystem::path, bool>, std::shared_ptr<const WorldAnalysis>>& cache() {
    static std::map<std::pair<std::filesystem::path, bool>, std::shared_ptr<const WorldAnalysis>>
        entries;
    return entries;
}

} // namespace

std::optional<FrqFile> parseFrq(const std::vector<char>& bytes) {
    if (bytes.size() < kFrqHeader || std::memcmp(bytes.data(), "FREQ0003", 8) != 0) {
        return std::nullopt;
    }
    FrqFile frq;
    frq.hop = read<std::int32_t>(bytes, 8);
    frq.averageF0 = read<double>(bytes, 12);
    const auto count = read<std::int32_t>(bytes, 36);
    if (frq.hop <= 0 || count < 0 ||
        bytes.size() < kFrqHeader + (static_cast<std::size_t>(count) * 16)) {
        return std::nullopt;
    }
    frq.f0.resize(static_cast<std::size_t>(count));
    frq.amplitude.resize(static_cast<std::size_t>(count));
    for (std::size_t k = 0; k < frq.f0.size(); ++k) {
        frq.f0[k] = read<double>(bytes, kFrqHeader + (k * 16));
        frq.amplitude[k] = read<double>(bytes, kFrqHeader + (k * 16) + 8);
    }
    return frq;
}

std::filesystem::path frqPathFor(const std::filesystem::path& wav) {
    std::filesystem::path frq = wav;
    std::u8string name = wav.filename().u8string();
    const std::size_t dot = name.rfind(u8'.');
    if (dot != std::u8string::npos) {
        name[dot] = u8'_';
    }
    name += u8".frq";
    frq.replace_filename(name);
    return frq;
}

std::vector<double> harvestF0(const std::vector<double>& samples, int sampleRate) {
    HarvestOption option;
    InitializeHarvestOption(&option);
    option.frame_period = kWorldFramePeriodMs;
    option.f0_floor = kMinF0;
    std::vector<double> positions = framesOf(sampleRate, samples.size());
    std::vector<double> f0(positions.size());
    Harvest(samples.data(), static_cast<int>(samples.size()), sampleRate, &option, positions.data(),
            f0.data());
    return f0;
}

std::shared_ptr<const WorldAnalysis> analyseWav(const std::filesystem::path& wav, bool useFrq) {
    const auto key = std::make_pair(wav, useFrq);
    {
        const std::scoped_lock lock(cacheMutex());
        if (const auto found = cache().find(key); found != cache().end()) {
            return found->second;
        }
    }

    format::SampleBuffer buffer;
    std::string error;
    if (!format::decodeFile(wav, buffer, error) || buffer.frames == 0) {
        return nullptr;
    }
    auto analysis = std::make_shared<WorldAnalysis>();
    analysis->sampleRate = static_cast<int>(buffer.sampleRate);
    const std::span<const float> left = buffer.leftAudio();
    const std::span<const float> right = buffer.rightAudio();
    analysis->samples.resize(left.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        analysis->samples[i] = 0.5 * (static_cast<double>(left[i]) + right[i]);
    }
    const int rate = analysis->sampleRate;
    const auto length = static_cast<int>(analysis->samples.size());
    std::vector<double> positions = framesOf(rate, analysis->samples.size());

    // F0: the bank's own, when it has one that parses.
    if (useFrq) {
        std::ifstream in(frqPathFor(wav), std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>()};
        if (const std::optional<FrqFile> frq = parseFrq(bytes)) {
            analysis->f0 = frqOnFrames(*frq, rate, positions);
            analysis->fromFrq = std::ranges::any_of(analysis->f0, [](double f) { return f > 0.0; });
        }
    }
    if (!analysis->fromFrq) {
        analysis->f0 = harvestF0(analysis->samples, rate);
    }

    CheapTrickOption cheapTrick;
    InitializeCheapTrickOption(rate, &cheapTrick);
    cheapTrick.f0_floor = kMinF0;
    cheapTrick.fft_size = GetFFTSizeForCheapTrick(rate, &cheapTrick);
    analysis->fftSize = cheapTrick.fft_size;
    const std::size_t bins = static_cast<std::size_t>(analysis->fftSize / 2) + 1;
    const std::size_t frames = analysis->f0.size();
    analysis->spectrogram.assign(frames, std::vector<double>(bins));
    analysis->aperiodicity.assign(frames, std::vector<double>(bins));
    std::vector<double*> sp(frames);
    std::vector<double*> ap(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        sp[i] = analysis->spectrogram[i].data();
        ap[i] = analysis->aperiodicity[i].data();
    }
    CheapTrick(analysis->samples.data(), length, rate, positions.data(), analysis->f0.data(),
               static_cast<int>(frames), &cheapTrick, sp.data());
    D4COption d4c;
    InitializeD4COption(&d4c);
    D4C(analysis->samples.data(), length, rate, positions.data(), analysis->f0.data(),
        static_cast<int>(frames), analysis->fftSize, &d4c, ap.data());

    const std::scoped_lock lock(cacheMutex());
    return cache().emplace(key, std::move(analysis)).first->second;
}

} // namespace adx::instruments

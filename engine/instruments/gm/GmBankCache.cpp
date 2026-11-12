// adx-thread: main
//
// The gm instrument's main-thread half: banks loaded once and shared, and a channel's
// BANK= resolved to a file. Apart from GmInstrument.cpp, which the realtime ban list
// covers.
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "engine/format/soundfont/SoundFont.h"
#include "engine/instruments/gm/GmSetup.h"

namespace adx::instruments {

/// What a gm node's views point into: the shared bank, and the flat tables built from it.
struct GmPins {
    std::shared_ptr<const format::soundfont::Bank> bank;
    std::filesystem::path path;
    std::vector<GmPresetView> presets;
    std::vector<format::soundfont::BankZone> zones;
    std::vector<GmSampleView> samples;
};

void destroyGmPins(GmPins* pins) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - the node's opaque owner.
    delete pins;
}

namespace {

struct CacheEntry {
    std::weak_ptr<const format::soundfont::Bank> bank;
};

std::mutex& cacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, CacheEntry>& cache() {
    static std::map<std::string, CacheEntry> entries;
    return entries;
}

} // namespace

std::shared_ptr<const format::soundfont::Bank> GmBankCache::get(const std::filesystem::path& path,
                                                                std::string& error) {
    const std::string key = path.lexically_normal().generic_string();
    {
        const std::lock_guard lock(cacheMutex());
        if (auto held = cache()[key].bank.lock()) {
            return held;
        }
    }
    auto bank = std::make_shared<format::soundfont::Bank>();
    if (!format::soundfont::loadBank(path, *bank, error)) {
        return nullptr;
    }
    std::shared_ptr<const format::soundfont::Bank> shared = std::move(bank);
    const std::lock_guard lock(cacheMutex());
    if (auto raced = cache()[key].bank.lock()) {
        return raced;
    }
    cache()[key].bank = shared;
    return shared;
}

std::filesystem::path gmBankPath(const std::string& written, const std::filesystem::path& base) {
    if (written.empty() || written == "system:gm.dls") {
        return format::soundfont::systemGmBank();
    }
    std::filesystem::path path(written);
    if (path.is_relative() && !base.empty()) {
        path = base / path;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) ? path : std::filesystem::path{};
}

void setGmBank(GmInstrument& node, std::shared_ptr<const format::soundfont::Bank> bank,
               std::filesystem::path path) {
    auto pins = std::make_unique<GmPins>();
    pins->path = std::move(path);
    pins->bank = std::move(bank);
    GmBankView view{};
    if (pins->bank != nullptr) {
        for (const format::soundfont::BankPreset& preset : pins->bank->presets) {
            pins->presets.push_back(
                GmPresetView{.bank = preset.bank,
                             .program = preset.program,
                             .firstZone = static_cast<std::uint32_t>(pins->zones.size()),
                             .zoneCount = static_cast<std::uint32_t>(preset.zones.size())});
            pins->zones.insert(pins->zones.end(), preset.zones.begin(), preset.zones.end());
        }
        pins->samples.reserve(pins->bank->samples.size());
        for (const format::soundfont::BankSample& sample : pins->bank->samples) {
            pins->samples.push_back(
                GmSampleView{.data = sample.data.data(),
                             .frames = static_cast<std::uint32_t>(sample.data.size()),
                             .rate = sample.rate});
        }
        view = GmBankView{.presets = pins->presets, .zones = pins->zones, .samples = pins->samples};
    }
    node.setBank(view, pins.release());
}

const format::soundfont::Bank* gmBankOf(const GmInstrument& node) noexcept {
    return node.pins() != nullptr ? node.pins()->bank.get() : nullptr;
}

std::filesystem::path gmBankPathOf(const GmInstrument& node) {
    return node.pins() != nullptr ? node.pins()->path : std::filesystem::path{};
}

void configureGm(GmInstrument& node, const project::Channel& channel,
                 const std::filesystem::path& base) {
    const std::filesystem::path path = gmBankPath(channel.instrument.voicebank, base);
    std::string error;
    setGmBank(node, path.empty() ? nullptr : GmBankCache::get(path, error), path);
}

bool gmMatches(const GmInstrument& node, const project::Channel& channel,
               const std::filesystem::path& base) {
    return gmBankPathOf(node) == gmBankPath(channel.instrument.voicebank, base);
}

} // namespace adx::instruments

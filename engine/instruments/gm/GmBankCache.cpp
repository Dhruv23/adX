// adx-thread: main
//
// The gm instrument's main-thread half: banks loaded once and shared, and a channel's
// BANK= resolved to a file. Apart from GmInstrument.cpp, which the realtime ban list
// covers.
#include <map>
#include <mutex>

#include "engine/format/soundfont/SoundFont.h"
#include "engine/instruments/gm/GmInstrument.h"

namespace adx::instruments {
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

void configureGm(GmInstrument& node, const project::Channel& channel,
                 const std::filesystem::path& base) {
    const std::filesystem::path path = gmBankPath(channel.instrument.voicebank, base);
    std::string error;
    node.setBank(path.empty() ? nullptr : GmBankCache::get(path, error), path);
}

bool gmMatches(const GmInstrument& node, const project::Channel& channel,
               const std::filesystem::path& base) {
    return node.bankPath() == gmBankPath(channel.instrument.voicebank, base);
}

} // namespace adx::instruments

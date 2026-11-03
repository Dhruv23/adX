// adx-thread: main
#include "engine/midi/DeviceNames.h"

#include <array>
#include <mutex>

#include "engine/midi/MidiRouter.h"

namespace adx::midi {

namespace {

struct Table {
    std::mutex mutex;
    std::array<std::string, MidiRouter::kDevices> names;
};

Table& table() {
    static Table instance;
    return instance;
}

} // namespace

void setDeviceName(std::size_t slot, std::string name) {
    Table& t = table();
    const std::scoped_lock lock(t.mutex);
    if (slot < t.names.size()) {
        t.names.at(slot) = std::move(name);
    }
}

void clearDeviceName(std::size_t slot) {
    setDeviceName(slot, {});
}

std::uint8_t deviceSlot(std::string_view name) {
    if (name.empty()) {
        return kAnyDevice;
    }
    Table& t = table();
    const std::scoped_lock lock(t.mutex);
    for (std::size_t i = 0; i < t.names.size(); ++i) {
        if (t.names.at(i) == name) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return kNoDevice;
}

std::string deviceName(std::uint8_t slot) {
    Table& t = table();
    const std::scoped_lock lock(t.mutex);
    return slot < t.names.size() ? t.names.at(slot) : std::string{};
}

} // namespace adx::midi

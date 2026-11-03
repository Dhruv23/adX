// adx-thread: main
//
// Which open MIDI input sits in which engine slot, by name. A project names devices
// (a route's, a learned binding's) by what the system calls them; the audio thread
// knows them by slot. MidiDevices records each open here, and the snapshot builder
// resolves names through it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace adx::midi {

/// No open input has that name: matches nothing.
inline constexpr std::uint8_t kNoDevice = 0xFE;

void setDeviceName(std::size_t slot, std::string name);
void clearDeviceName(std::size_t slot);
/// kAnyDevice for an empty name, the slot of the open input called `name`, or kNoDevice.
[[nodiscard]] std::uint8_t deviceSlot(std::string_view name);
/// The name of the input in `slot`, or empty.
[[nodiscard]] std::string deviceName(std::uint8_t slot);

} // namespace adx::midi

// What to open: rates, block size, channel counts, device ids.
#pragma once

#include <cstdint>

namespace adx::audio {

/// "Whatever the OS considers the default device."
inline constexpr std::uint32_t kDefaultDeviceId = 0xFFFFFFFFU;

/// "No device on this side of the stream."
inline constexpr std::uint32_t kNoDevice = 0xFFFFFFFEU;

/// Defaults are a deliberate middle. 48 kHz because that is what Windows mixes at,
/// so anything else costs a resample nobody asked for; 256 frames (~5.3 ms) because
/// it is comfortably achievable on WASAPI shared mode without being so tight that a
/// Debug build with the allocator hook installed cannot keep up.
struct StreamConfig {
    std::uint32_t outputDeviceId{kDefaultDeviceId};
    std::uint32_t inputDeviceId{kNoDevice};
    std::uint32_t sampleRate{48000};
    std::uint32_t blockFrames{256};
    std::uint32_t outputChannels{2};
    std::uint32_t inputChannels{0};
};

} // namespace adx::audio

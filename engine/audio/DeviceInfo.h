// A plain description of one audio device.
//
// This crosses the pybind11 boundary (phase_1.md §3.12), so it is a value with
// fixed-capacity strings rather than std::string - see engine/rt/FixedString.h for
// why anything the engine holds on behalf of the audio side avoids allocation.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/rt/FixedString.h"
#include "engine/rt/FixedVector.h"

namespace adx::audio {

/// Most sample rates any one device advertises. Generous; real devices list 6-8.
inline constexpr std::size_t kMaxSampleRatesPerDevice = 16;

struct DeviceInfo {
    std::uint32_t id{};
    rt::FixedString<128> name;
    /// "WASAPI", "DirectSound", "ASIO" (Phase 9), "Null" or "Offline".
    rt::FixedString<32> apiName;
    std::uint32_t maxOutputChannels{};
    std::uint32_t maxInputChannels{};
    rt::FixedVector<std::uint32_t, kMaxSampleRatesPerDevice> sampleRates;
    std::uint32_t preferredSampleRate{};
    bool isDefaultOutput{false};
    bool isDefaultInput{false};
};

} // namespace adx::audio

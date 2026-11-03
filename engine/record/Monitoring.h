// adx-thread: main
//
// Monitoring and the latency a take is compensated by (phase_8.md §3.1).
//
//   direct      the interface plays the input to the headphones itself, at zero
//               latency; adX does not play it again
//   software    the input plays through the armed track's insert - its effects heard
//               while recording - at the cost of the round trip, which the readout shows
//               so the choice is an informed one
//
// A recorded take must land where the performer heard the music, so it is moved back
// by the time a sound took to come out of the engine and go back in: the device's
// output and input latency plus the graph's own (PDC). Drivers misreport theirs, so a
// loopback calibration (RecordSession::calibrate) can measure the round trip instead;
// a measured value overrides the reported one.
#pragma once

#include <cstdint>
#include <optional>

#include "engine/audio/AudioBackend.h"

namespace adx::record {

enum class MonitorMode : std::uint8_t {
    Direct,
    Software,
};

[[nodiscard]] const char* toString(MonitorMode mode) noexcept;

struct LatencyReport {
    std::int64_t inputSamples{0};
    std::int64_t outputSamples{0};
    std::int64_t graphSamples{0};
    /// The measured round trip, when a calibration has run for this device.
    std::optional<std::int64_t> measuredRoundTrip;

    /// What a take is moved back by.
    [[nodiscard]] std::int64_t compensation() const noexcept {
        return measuredRoundTrip.value_or(inputSamples + outputSamples) + graphSamples;
    }
    /// What software monitoring delays the input by, in milliseconds: the readout.
    [[nodiscard]] double monitoringMs(std::uint32_t sampleRate) const noexcept {
        return sampleRate == 0 ? 0.0 : static_cast<double>(compensation()) * 1000.0 / sampleRate;
    }
};

/// The report for a stream and a graph, with a calibration if there is one.
[[nodiscard]] LatencyReport latencyOf(const audio::StreamInfo& stream, std::uint32_t graphLatency,
                                      std::optional<std::int64_t> measured = std::nullopt);

} // namespace adx::record

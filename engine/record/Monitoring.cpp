// adx-thread: main
#include "engine/record/Monitoring.h"

#include <cmath>

namespace adx::record {

const char* toString(MonitorMode mode) noexcept {
    return mode == MonitorMode::Software ? "software" : "direct";
}

LatencyReport latencyOf(const audio::StreamInfo& stream, std::uint32_t graphLatency,
                        std::optional<std::int64_t> measured) {
    const auto samples = [&stream](double ms) {
        return static_cast<std::int64_t>(std::llround(ms * stream.sampleRate / 1000.0));
    };
    return LatencyReport{.inputSamples = samples(stream.inputLatencyMs),
                         .outputSamples = samples(stream.outputLatencyMs),
                         .graphSamples = graphLatency,
                         .measuredRoundTrip = measured};
}

} // namespace adx::record

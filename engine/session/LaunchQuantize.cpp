// adx-thread: realtime
#include "engine/session/LaunchQuantize.h"

#include <array>
#include <cstring>

#include "engine/core/Time.h"

namespace adx::session {

namespace {

constexpr std::array<const char*, 8> kNames{"global", "instant", "1/4",  "1/2",
                                            "bar",    "2bar",    "4bar", "8bar"};

} // namespace

std::int64_t quantizeTicks(LaunchQuantize quantize, std::int64_t barTicks) noexcept {
    switch (quantize) {
    case LaunchQuantize::Quarter:
        return core::kPpq;
    case LaunchQuantize::Half:
        return core::kPpq * 2;
    case LaunchQuantize::Bar:
        return barTicks;
    case LaunchQuantize::TwoBars:
        return barTicks * 2;
    case LaunchQuantize::FourBars:
        return barTicks * 4;
    case LaunchQuantize::EightBars:
        return barTicks * 8;
    case LaunchQuantize::Global:
    case LaunchQuantize::Instant:
        break;
    }
    return 0;
}

std::int64_t nextBoundary(std::int64_t tick, LaunchQuantize quantize, std::int64_t barTicks,
                          std::int64_t origin) noexcept {
    const std::int64_t step = quantizeTicks(quantize, barTicks);
    if (step <= 0) {
        return tick;
    }
    const std::int64_t along = tick - origin;
    // Floor division that is right for a tick before the origin too.
    std::int64_t steps = along / step;
    if (along % step != 0 && along < 0) {
        --steps;
    }
    std::int64_t boundary = origin + (steps * step);
    if (boundary < tick) {
        boundary += step;
    }
    return boundary;
}

const char* toString(LaunchQuantize quantize) noexcept {
    const auto index = static_cast<std::size_t>(quantize);
    return index < kNames.size() ? kNames.at(index) : "bar";
}

bool parseQuantize(const char* text, LaunchQuantize& out) noexcept {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (std::strcmp(text, kNames.at(i)) == 0) {
            out = static_cast<LaunchQuantize>(i);
            return true;
        }
    }
    return false;
}

} // namespace adx::session

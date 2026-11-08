// adx-thread: realtime
#include "engine/session/FollowAction.h"

#include <array>
#include <cstring>

namespace adx::session {

namespace {

constexpr std::array<const char*, 7> kNames{"none",  "stop",   "next",  "previous",
                                            "first", "random", "repeat"};

std::uint32_t mix(std::uint32_t x) noexcept {
    // A 32-bit avalanche (lowbias32): every input bit moves every output bit.
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

} // namespace

FollowResult follow(FollowAction action, std::uint32_t row, std::uint32_t rows,
                    std::uint32_t passes, std::uint32_t repeats, std::uint32_t seed) noexcept {
    using Kind = FollowResult::Kind;
    if (rows == 0) {
        return {.kind = Kind::Stop};
    }
    switch (action) {
    case FollowAction::None:
        return {.kind = Kind::Continue};
    case FollowAction::Stop:
        return {.kind = Kind::Stop};
    case FollowAction::Next:
        return {.kind = Kind::Launch, .row = (row + 1) % rows};
    case FollowAction::Previous:
        return {.kind = Kind::Launch, .row = row == 0 ? rows - 1 : row - 1};
    case FollowAction::First:
        return {.kind = Kind::Launch, .row = 0};
    case FollowAction::Random: {
        if (rows == 1) {
            return {.kind = Kind::Launch, .row = 0};
        }
        // One of the other rows: the clip's own is never drawn, so Random always moves.
        const std::uint32_t pick =
            mix(seed ^ (row * 0x9E3779B9U) ^ (passes * 0x85EBCA6BU)) % (rows - 1);
        return {.kind = Kind::Launch, .row = pick >= row ? pick + 1 : pick};
    }
    case FollowAction::RepeatN:
        if (passes < (repeats == 0 ? 1U : repeats)) {
            return {.kind = Kind::Launch, .row = row};
        }
        return {.kind = Kind::Stop};
    }
    return {.kind = Kind::Continue};
}

const char* toString(FollowAction action) noexcept {
    const auto index = static_cast<std::size_t>(action);
    return index < kNames.size() ? kNames.at(index) : "none";
}

bool parseFollow(const char* text, FollowAction& out) noexcept {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (std::strcmp(text, kNames.at(i)) == 0) {
            out = static_cast<FollowAction>(i);
            return true;
        }
    }
    return false;
}

} // namespace adx::session

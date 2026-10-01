// A stable hash of a rendered buffer: the mechanism behind the golden-hash corpus.
//
// 128-bit FNV-1a over the raw bytes of the floats, in order (phase_3.md §4.10). Raw
// bytes, not values: -0.0 and 0.0 hash differently, and so does every last-bit
// difference a reordered sum produces - which is exactly the sensitivity a test for
// "bit-identical" needs. 128 bits because the corpus will grow for years and a
// collision would be a test that cannot fail.
//
// The corpus starts in Phase 3 rather than Phase 8 on purpose: a golden corpus that
// starts late has no history to protect.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace adx::render {

struct RenderHash {
    std::uint64_t high{0};
    std::uint64_t low{0};

    /// 32 lowercase hex digits, high word first. What the corpus file stores.
    [[nodiscard]] std::string hex() const;

    [[nodiscard]] friend bool operator==(const RenderHash&, const RenderHash&) noexcept = default;
};

[[nodiscard]] RenderHash hashSamples(std::span<const float> samples) noexcept;

} // namespace adx::render

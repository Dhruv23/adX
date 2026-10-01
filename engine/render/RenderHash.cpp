#include "engine/render/RenderHash.h"

#include <array>
#include <cstring>

namespace adx::render {
namespace {

// FNV-1a 128: offset basis 0x6c62272e07bb014262b821756295c58d,
// prime 2^88 + 0x13b.
constexpr std::uint64_t kBasisHigh = 0x6c62272e07bb0142ULL;
constexpr std::uint64_t kBasisLow = 0x62b821756295c58dULL;
constexpr std::uint64_t kPrimeLowWord = 0x13bULL;
constexpr unsigned kPrimeShift = 88 - 64;

/// The high 64 bits of a 64 x 64 product, in portable arithmetic.
[[nodiscard]] constexpr std::uint64_t mulHigh(std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t aLow = a & 0xFFFFFFFFULL;
    const std::uint64_t aHigh = a >> 32U;
    const std::uint64_t bLow = b & 0xFFFFFFFFULL;
    const std::uint64_t bHigh = b >> 32U;
    const std::uint64_t lowLow = aLow * bLow;
    const std::uint64_t lowHigh = aLow * bHigh;
    const std::uint64_t highLow = aHigh * bLow;
    const std::uint64_t highHigh = aHigh * bHigh;
    const std::uint64_t middle =
        (lowLow >> 32U) + (lowHigh & 0xFFFFFFFFULL) + (highLow & 0xFFFFFFFFULL);
    return highHigh + (lowHigh >> 32U) + (highLow >> 32U) + (middle >> 32U);
}

} // namespace

RenderHash hashSamples(std::span<const float> samples) noexcept {
    std::uint64_t high = kBasisHigh;
    std::uint64_t low = kBasisLow;
    for (const float sample : samples) {
        std::array<unsigned char, sizeof(float)> bytes{};
        std::memcpy(bytes.data(), &sample, sizeof(float));
        for (const unsigned char byte : bytes) {
            low ^= byte;
            // (high:low) * (2^88 + 0x13b), modulo 2^128. The 2^88 term shifts `low`
            // entirely into the high word; `high`'s own contribution overflows away.
            const std::uint64_t productLow = low * kPrimeLowWord;
            const std::uint64_t carry = mulHigh(low, kPrimeLowWord);
            high = (high * kPrimeLowWord) + carry + (low << kPrimeShift);
            low = productLow;
        }
    }
    return RenderHash{.high = high, .low = low};
}

std::string RenderHash::hex() const {
    static constexpr std::array<char, 16> kDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out(32, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        const auto shift = static_cast<unsigned>((15 - i) * 4);
        out[i] = kDigits[(high >> shift) & 0xFU];
        out[16 + i] = kDigits[(low >> shift) & 0xFU];
    }
    return out;
}

} // namespace adx::render

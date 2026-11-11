// adx-thread: main
//
// A bounds-checked RIFF walker shared by the SF2 and DLS parsers (phase_12.md §4.7.4).
//
// Banks come off the internet, so every size is checked against its parent and the
// file: a chunk that claims more than it has is an error with a message, never a read
// past the end. Little-endian throughout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adx::format::soundfont {

struct Chunk {
    std::string id;       // four characters
    std::string listType; // for RIFF and LIST: the form type; empty otherwise
    std::span<const std::byte> body;
    /// Offset of the chunk's header in the file, for messages.
    std::size_t offset{0};
};

class RiffReader {
public:
    explicit RiffReader(std::span<const std::byte> file) : m_file(file) {}

    /// The file's top RIFF chunk; nullopt (with error()) when it is not one.
    [[nodiscard]] std::optional<Chunk> root();

    /// The chunks inside a RIFF or LIST chunk's body (after its form type). Stops, with
    /// error() set, at the first chunk that does not fit.
    [[nodiscard]] std::vector<Chunk> children(const Chunk& parent);

    [[nodiscard]] const std::string& error() const noexcept {
        return m_error;
    }

private:
    [[nodiscard]] std::vector<Chunk> walk(std::span<const std::byte> body, std::size_t base);

    std::span<const std::byte> m_file;
    std::string m_error;
};

/// The first child with `id` (and, for a LIST, `listType`), or nullptr.
[[nodiscard]] const Chunk* findChunk(const std::vector<Chunk>& chunks, std::string_view id,
                                     std::string_view listType = {});

/// Little-endian reads at `at`; false when they do not fit.
[[nodiscard]] inline bool readU16(std::span<const std::byte> b, std::size_t at,
                                  std::uint16_t& out) noexcept {
    if (at + 2 > b.size()) {
        return false;
    }
    out = static_cast<std::uint16_t>(static_cast<unsigned>(b[at]) |
                                     (static_cast<unsigned>(b[at + 1]) << 8U));
    return true;
}
[[nodiscard]] inline bool readU32(std::span<const std::byte> b, std::size_t at,
                                  std::uint32_t& out) noexcept {
    if (at + 4 > b.size()) {
        return false;
    }
    out = static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8U) |
          (static_cast<std::uint32_t>(b[at + 2]) << 16U) |
          (static_cast<std::uint32_t>(b[at + 3]) << 24U);
    return true;
}
[[nodiscard]] inline bool readI16(std::span<const std::byte> b, std::size_t at,
                                  std::int16_t& out) noexcept {
    std::uint16_t u = 0;
    if (!readU16(b, at, u)) {
        return false;
    }
    out = static_cast<std::int16_t>(u);
    return true;
}
[[nodiscard]] inline bool readI32(std::span<const std::byte> b, std::size_t at,
                                  std::int32_t& out) noexcept {
    std::uint32_t u = 0;
    if (!readU32(b, at, u)) {
        return false;
    }
    out = static_cast<std::int32_t>(u);
    return true;
}
/// A fixed-width, NUL-padded name.
[[nodiscard]] std::string readName(std::span<const std::byte> b, std::size_t at, std::size_t width);

} // namespace adx::format::soundfont

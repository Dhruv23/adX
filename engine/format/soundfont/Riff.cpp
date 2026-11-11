// adx-thread: main
#include "engine/format/soundfont/Riff.h"

#include <algorithm>

namespace adx::format::soundfont {
namespace {

std::string fourcc(std::span<const std::byte> b, std::size_t at) {
    std::string out(4, ' ');
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<char>(b[at + i]);
    }
    return out;
}

} // namespace

std::optional<Chunk> RiffReader::root() {
    std::uint32_t size = 0;
    if (m_file.size() < 12 || fourcc(m_file, 0) != "RIFF" || !readU32(m_file, 4, size)) {
        m_error = "not a RIFF file";
        return std::nullopt;
    }
    if (static_cast<std::size_t>(size) + 8 > m_file.size() || size < 4) {
        m_error = "the RIFF chunk claims " + std::to_string(size) + " bytes; the file has " +
                  std::to_string(m_file.size() - 8);
        return std::nullopt;
    }
    return Chunk{.id = "RIFF",
                 .listType = fourcc(m_file, 8),
                 .body = m_file.subspan(12, size - 4),
                 .offset = 0};
}

std::vector<Chunk> RiffReader::children(const Chunk& parent) {
    const std::size_t base = parent.offset + (parent.listType.empty() ? 8 : 12);
    return walk(parent.body, base);
}

std::vector<Chunk> RiffReader::walk(std::span<const std::byte> body, std::size_t base) {
    std::vector<Chunk> out;
    std::size_t at = 0;
    while (at + 8 <= body.size()) {
        std::uint32_t size = 0;
        static_cast<void>(readU32(body, at + 4, size));
        const std::string id = fourcc(body, at);
        if (static_cast<std::size_t>(size) > body.size() - at - 8) {
            m_error = "chunk '" + id + "' at byte " + std::to_string(base + at) + " claims " +
                      std::to_string(size) + " bytes; its parent has " +
                      std::to_string(body.size() - at - 8);
            break;
        }
        Chunk chunk{
            .id = id, .listType = {}, .body = body.subspan(at + 8, size), .offset = base + at};
        if ((id == "LIST" || id == "RIFF") && size >= 4) {
            chunk.listType = fourcc(body, at + 8);
            chunk.body = body.subspan(at + 12, size - 4);
        }
        out.push_back(std::move(chunk));
        at += 8 + static_cast<std::size_t>(size) + (size & 1U);
    }
    return out;
}

const Chunk* findChunk(const std::vector<Chunk>& chunks, std::string_view id,
                       std::string_view listType) {
    const auto match = std::ranges::find_if(chunks, [&](const Chunk& c) {
        return c.id == id && (listType.empty() || c.listType == listType);
    });
    return match == chunks.end() ? nullptr : &*match;
}

std::string readName(std::span<const std::byte> b, std::size_t at, std::size_t width) {
    std::string out;
    for (std::size_t i = 0; i < width && at + i < b.size(); ++i) {
        const auto c = static_cast<char>(b[at + i]);
        if (c == '\0') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace adx::format::soundfont

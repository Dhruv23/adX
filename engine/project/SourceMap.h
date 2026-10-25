// Where each entity was written in the file it was loaded from (P2-2).
//
// A Project need not have come from text, which is why validate() reported its
// findings - a routing cycle, a note clip for a channel that is gone - with no
// position. When it did come from text, the parser can say where each entity was, and
// validate() then reports against that line: the editor panel underlines the ROUTE that
// closes the cycle instead of printing "line 0".
//
// Filled only when asked (ParseOptions::sources): a 100k-note load does not pay for a
// map nobody reads.
#pragma once

#include <cstdint>
#include <unordered_map>

#include "engine/format/adx/Diagnostics.h"

namespace adx::project {

enum class SourceKind : std::uint8_t {
    Channel,
    Pattern,
    Note,
    Insert,
    Slot,
    Send,
    Route,
    Track,
    Item,
    Marker,
    AutomationClip,
    Controller,
};

class SourceMap {
public:
    void record(SourceKind kind, std::uint32_t id, format::Span span) {
        m_spans[key(kind, id)] = span;
    }

    /// The entity's span, or a span on line 0 - "no position" - when it was not
    /// recorded.
    [[nodiscard]] format::Span find(SourceKind kind, std::uint32_t id) const noexcept {
        const auto found = m_spans.find(key(kind, id));
        return found != m_spans.end()
                   ? found->second
                   : format::Span{.line = 0, .column = 0, .length = 0, .byteOffset = 0};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_spans.size();
    }

    void clear() noexcept {
        m_spans.clear();
    }

private:
    [[nodiscard]] static std::uint64_t key(SourceKind kind, std::uint32_t id) noexcept {
        return (static_cast<std::uint64_t>(kind) << 32U) | id;
    }

    std::unordered_map<std::uint64_t, format::Span> m_spans;
};

} // namespace adx::project

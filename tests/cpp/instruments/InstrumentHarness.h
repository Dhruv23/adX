// Drives one instrument node directly, for the instrument tests: every buffer is
// allocated before the first process() call and every call runs inside a
// ScopedRtSection, so any instrument test is also an allocation test (phase_4.md §6,
// instrument_no_alloc_in_process).
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "engine/graph/Node.h"
#include "engine/graph/nodes/ChannelNode.h"

namespace adx::tests {

/// A channel's full parameter slice for instrument `type`: volume 1, pan 0, audible,
/// no pitch offset, then every descriptor of the type at its default, in table order.
[[nodiscard]] std::vector<float> instrumentParams(std::string_view type);

/// Index into that slice of the instrument parameter `name`.
[[nodiscard]] std::uint32_t instrumentParamIndex(std::string_view type, std::string_view name);

[[nodiscard]] graph::BlockEvent noteOn(std::uint32_t offset, std::uint32_t noteId,
                                       std::uint8_t pitch, std::uint8_t velocity = 100);
[[nodiscard]] graph::BlockEvent noteOff(std::uint32_t offset, std::uint32_t noteId,
                                        std::uint8_t pitch);
[[nodiscard]] graph::BlockEvent pitchGlide(std::uint32_t offset, std::uint32_t noteId,
                                           std::uint8_t pitch, float cents, std::uint32_t duration);

struct InstrumentRun {
    std::vector<float> left;
    std::vector<float> right;
    std::uint64_t violations{0};
    /// The pool's sounding-voice count after each block.
    std::vector<std::uint32_t> sounding;
};

/// Runs `node` (prepared at 48 kHz) for `frames`, in blocks of `block`, delivering each
/// event in the block that contains its offset (offsets are absolute frames).
InstrumentRun runInstrument(graph::ChannelNode& node, std::vector<graph::BlockEvent> events,
                            std::uint32_t frames, std::vector<float> params,
                            std::uint32_t block = 64);

/// A fresh node of `type`, prepared at 48 kHz.
[[nodiscard]] std::shared_ptr<graph::ChannelNode> preparedInstrument(std::string_view type,
                                                                     std::uint16_t polyphony = 8);

} // namespace adx::tests

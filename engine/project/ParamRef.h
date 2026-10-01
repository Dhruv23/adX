// A resolved parameter address: eight bytes the audio thread can compare.
//
// Its own header because it crosses to the audio thread inside the render snapshot,
// and ParamRegistry.h - which turns paths into these - needs std::string to do it.
#pragma once

#include <cstdint>

namespace adx::project {

/// What a ParamRef's `owner` and `index` mean.
///
/// The kind decides which id space `owner` lives in - channel, insert, slot or
/// send. Slots and sends have their own project-wide ids for exactly this reason:
/// it keeps ParamRef at eight bytes instead of needing a second index to say which
/// slot of which insert.
enum class ParamKind : std::uint16_t {
    None = 0,
    ChannelVolume,
    ChannelPan,
    ChannelPitch,
    ChannelArpGate,
    /// `owner` is a ChannelId, `index` is the position in its instrument's params.
    ChannelInstrumentParam,
    InsertGain,
    InsertPan,
    InsertWidth,
    /// `owner` is a SlotId.
    SlotMix,
    SlotBypass,
    /// `owner` is a SlotId, `index` is the position in that slot's params.
    SlotParam,
    /// `owner` is a SendId.
    SendLevel,
};

/// A resolved parameter. POD, eight bytes, trivially copyable - which is what lets
/// Phase 3 put it in a snapshot and hand it to the audio thread.
struct ParamRef {
    std::uint32_t owner{0};
    std::uint16_t index{0};
    ParamKind kind{ParamKind::None};

    [[nodiscard]] bool valid() const noexcept {
        return kind != ParamKind::None;
    }

    [[nodiscard]] friend bool operator==(const ParamRef&, const ParamRef&) noexcept = default;
};

static_assert(sizeof(ParamRef) == 8, "ParamRef is a POD the audio thread copies");

} // namespace adx::project

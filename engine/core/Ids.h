// Strong entity ids.
//
// Every entity in the project model is addressed by one of these and never by a
// pointer or an index. Pointers dangle when a std::vector reallocates, and indices
// shift when something before them is deleted - both of which an undo stack does
// constantly. An id survives both (phase_2.md §4.6).
//
// Ids are monotonic and never reused. That is what makes (ChannelId, NoteId) a
// well-defined voice identity for Phase 3 (FINAL_PLAN §3.3.4): a note that is
// deleted and redrawn is a *different* note, and the voice allocator can tell.
#pragma once

#include <compare>
#include <cstdint>

namespace adx::core {

/// A 32-bit id tagged with the entity type it addresses.
///
/// `Tag` is only ever declared, never defined - it exists so ChannelId and
/// PatternId are distinct types that cannot be passed to each other's overloads.
/// Zero is the null id: `Project::find` returns nullptr for it, and a
/// default-constructed id is therefore safely invalid rather than pointing at
/// entity number one.
template<class Tag> struct Id {
    std::uint32_t value{0};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != 0;
    }

    friend constexpr auto operator<=>(const Id&, const Id&) noexcept = default;
};

// One tag per entity kind. Declared-only types, so this costs nothing at runtime.
struct ChannelTag;
struct PatternTag;
struct NoteTag;
struct InsertTag;
struct SlotTag;
struct SendTag;
struct RouteTag;
struct PlaylistTrackTag;
struct ItemTag;
struct MarkerTag;
struct SampleTag;
struct AutomationClipTag;

using ChannelId = Id<ChannelTag>;
using PatternId = Id<PatternTag>;
using NoteId = Id<NoteTag>;
using InsertId = Id<InsertTag>;
using SlotId = Id<SlotTag>;
using SendId = Id<SendTag>;
using RouteId = Id<RouteTag>;
using PlaylistTrackId = Id<PlaylistTrackTag>;
using ItemId = Id<ItemTag>;
using MarkerId = Id<MarkerTag>;
using SampleId = Id<SampleTag>;
using AutomationClipId = Id<AutomationClipTag>;

/// A monotonic id source.
///
/// `restore()` exists for one reason and it is the reason the undo gate passes: a
/// create command that allocates id N must, on revert, put the counter back to
/// N-1, so that redoing it allocates N again rather than N+1. Without that, undo
/// followed by redo produces a project that is structurally identical but writes
/// different `insert.N` references, and `undo_redo_symmetry` fails on text that
/// looks correct (phase_2.md §4.9).
template<class Tag> class IdCounter {
public:
    [[nodiscard]] Id<Tag> next() noexcept {
        ++m_last;
        return Id<Tag>{m_last};
    }

    [[nodiscard]] std::uint32_t mark() const noexcept {
        return m_last;
    }

    void restore(std::uint32_t mark) noexcept {
        m_last = mark;
    }

    /// Raises the counter so `id` can never be handed out again. Used by the
    /// parser, which is told which ids to use rather than choosing them.
    void observe(Id<Tag> id) noexcept {
        if (id.value > m_last) {
            m_last = id.value;
        }
    }

    friend constexpr bool operator==(const IdCounter&, const IdCounter&) noexcept = default;

private:
    std::uint32_t m_last{0};
};

} // namespace adx::core

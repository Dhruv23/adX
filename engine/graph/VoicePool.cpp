#include "engine/graph/VoicePool.h"

namespace adx::graph {
namespace {

[[nodiscard]] bool sounding(const Voice& voice) noexcept {
    return voice.phase == VoicePhase::Active || voice.phase == VoicePhase::Released;
}

/// The voice started longest ago among those `accept` admits. Ties go to the lower
/// slot, so the choice is a pure function of the pool's state.
template<class Accept>
[[nodiscard]] Voice* oldestWhere(std::span<Voice> voices, Accept accept) noexcept {
    Voice* best = nullptr;
    for (Voice& voice : voices) {
        if (accept(voice) && (best == nullptr || voice.startedAt < best->startedAt)) {
            best = &voice;
        }
    }
    return best;
}

[[nodiscard]] Voice* quietestActive(std::span<Voice> voices) noexcept {
    Voice* best = nullptr;
    for (Voice& voice : voices) {
        if (voice.phase != VoicePhase::Active) {
            continue;
        }
        // Quietest first; among equally quiet voices, the oldest. Level ties are the
        // common case - every voice at full sustain - and "oldest" is the rule that
        // then applies.
        if (best == nullptr || voice.level < best->level ||
            (voice.level == best->level && voice.startedAt < best->startedAt)) {
            best = &voice;
        }
    }
    return best;
}

} // namespace

VoicePool::VoicePool(std::uint16_t maxPolyphony, std::span<Voice> storage,
                     std::uint32_t fadeSamples) noexcept
    : m_voices(storage), m_maxPolyphony(maxPolyphony == 0 ? 1 : maxPolyphony),
      m_fadeSamples(fadeSamples == 0 ? 1 : fadeSamples) {
    clear();
}

void VoicePool::clear() noexcept {
    for (Voice& voice : m_voices) {
        voice = Voice{};
    }
}

std::uint32_t VoicePool::soundingCount() const noexcept {
    std::uint32_t count = 0;
    for (const Voice& voice : m_voices) {
        if (sounding(voice)) {
            ++count;
        }
    }
    return count;
}

Voice* VoicePool::freeSlot() noexcept {
    for (Voice& voice : m_voices) {
        if (voice.phase == VoicePhase::Free) {
            return &voice;
        }
    }
    return nullptr;
}

Voice* VoicePool::chooseVictim(project::VoiceStealMode mode) noexcept {
    switch (mode) {
    case project::VoiceStealMode::OldestReleased: {
        if (Voice* released = oldestWhere(
                m_voices, [](const Voice& v) { return v.phase == VoicePhase::Released; })) {
            return released;
        }
        if (Voice* quiet = quietestActive(m_voices)) {
            return quiet;
        }
        return oldestWhere(m_voices, sounding);
    }
    case project::VoiceStealMode::Quietest: {
        if (Voice* quiet = quietestActive(m_voices)) {
            return quiet;
        }
        return oldestWhere(m_voices, sounding);
    }
    case project::VoiceStealMode::Oldest:
        return oldestWhere(m_voices, sounding);
    case project::VoiceStealMode::None:
        return nullptr;
    }
    return nullptr;
}

Voice* VoicePool::allocate(VoiceKey key, project::VoiceStealMode mode) noexcept {
    if (soundingCount() >= m_maxPolyphony) {
        Voice* victim = chooseVictim(mode);
        if (victim == nullptr) {
            return nullptr;
        }
        victim->phase = VoicePhase::Stolen;
        victim->fadeRemaining = m_fadeSamples;
    }

    Voice* slot = freeSlot();
    if (slot == nullptr) {
        // Every spare slot is mid-fade: more steals landed inside one fade window than
        // there is polyphony. Cut the fade closest to finished - the least audible
        // choice left - rather than refuse the note.
        for (Voice& voice : m_voices) {
            if (voice.phase == VoicePhase::Stolen &&
                (slot == nullptr || voice.fadeRemaining < slot->fadeRemaining)) {
                slot = &voice;
            }
        }
        if (slot == nullptr) {
            return nullptr;
        }
    }

    *slot = Voice{};
    slot->key = key;
    slot->phase = VoicePhase::Active;
    slot->startedAt = m_clock;
    return slot;
}

Voice* VoicePool::findSounding(VoiceKey key) noexcept {
    for (Voice& voice : m_voices) {
        if (sounding(voice) && voice.key == key) {
            return &voice;
        }
    }
    return nullptr;
}

Voice* VoicePool::find(VoiceKey key) noexcept {
    for (Voice& voice : m_voices) {
        if (voice.phase == VoicePhase::Active && voice.key == key) {
            return &voice;
        }
    }
    return nullptr;
}

void VoicePool::release(Voice& voice) noexcept {
    if (voice.phase == VoicePhase::Active) {
        voice.phase = VoicePhase::Released;
    }
}

void VoicePool::releaseAll(std::uint32_t timeSource) noexcept {
    for (Voice& voice : m_voices) {
        if (voice.timeSource == timeSource) {
            release(voice);
        }
    }
}

void VoicePool::releaseEndingAtOrAfter(std::uint32_t timeSource, std::int64_t tick) noexcept {
    for (Voice& voice : m_voices) {
        if (voice.timeSource == timeSource && voice.endTick >= tick) {
            release(voice);
        }
    }
}

void VoicePool::free(Voice& voice) noexcept {
    voice.phase = VoicePhase::Free;
    voice.fadeRemaining = 0;
    voice.level = 0.0F;
}

} // namespace adx::graph

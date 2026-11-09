#include "engine/instruments/dx7/Dx7Engine.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace adx::dx7 {

namespace {

constexpr std::int32_t kSemitone = (1 << 24) / 12;

[[nodiscard]] int transposed(const Dx7Voice& voice, int midinote) noexcept {
    return std::clamp(midinote + static_cast<int>(voice.bytes[kTranspose]) - 24, 0, 127);
}

} // namespace

void Dx7Engine::prepare(double sampleRate) {
    static_cast<void>(msfa::constTables());
    msfa::buildRateTables(m_rate, sampleRate);
    reset();
}

void Dx7Engine::reset() noexcept {
    for (Slot& slot : m_slots) {
        slot.live = false;
        slot.keydown = false;
        slot.sustained = false;
    }
    m_lfo = msfa::Lfo{};
    m_lfo.reset(m_rate, &m_voice.bytes[kLfoSpeed]);
    m_sustain = false;
    m_current = 0;
    m_heldCount = 0;
    m_lastNote = -1;
}

void Dx7Engine::setVoice(const Dx7Voice& voice) noexcept {
    m_voice = voice;
    m_lfo.reset(m_rate, &m_voice.bytes[kLfoSpeed]);
    for (Slot& slot : m_slots) {
        if (slot.live) {
            slot.note.update(m_voice.bytes.data());
        }
    }
}

void Dx7Engine::setFunction(const Dx7Function& function) noexcept {
    if (function.mono != m_function.mono) {
        for (Slot& slot : m_slots) {
            if (slot.live && slot.keydown) {
                release(slot);
            }
        }
        m_heldCount = 0;
    }
    m_function = function;
    m_function.polyphony = std::clamp(function.polyphony, 1, kMaxVoices);
    m_ctrls.bendRange = function.bendRange;
    m_ctrls.bendStep = function.bendStep;
    m_ctrls.range = function.range;
    m_ctrls.assign = function.assign;
}

void Dx7Engine::setController(int which, int value) noexcept {
    if (which >= 0 && which < 4) {
        m_ctrls.value[static_cast<std::size_t>(which)] = std::clamp(value, 0, 127);
    }
}

int Dx7Engine::allocate() noexcept {
    const int limit = m_function.polyphony;
    // A free slot within the limit, round robin as upstream's AllocateNote.
    int note = m_current % limit;
    for (int i = 0; i < limit; i++) {
        if (!m_slots[static_cast<std::size_t>(note)].live) {
            m_current = (note + 1) % limit;
            return note;
        }
        note = (note + 1) % limit;
    }
    // Every slot sounds: steal the oldest released one, else the oldest of all.
    int oldest = -1;
    for (int pass = 0; pass < 2 && oldest < 0; ++pass) {
        std::uint32_t best = 0;
        for (int i = 0; i < limit; ++i) {
            const Slot& slot = m_slots[static_cast<std::size_t>(i)];
            if (pass == 0 && slot.keydown) {
                continue;
            }
            const std::uint32_t age = m_clock - slot.age;
            if (oldest < 0 || age > best) {
                oldest = i;
                best = age;
            }
        }
    }
    return oldest;
}

void Dx7Engine::startGlide(Slot& slot, int fromNote) noexcept {
    slot.glide = 0;
    slot.glideStep = 0;
    if (m_function.portaTime <= 0 || fromNote < 0) {
        return;
    }
    const bool held = std::ranges::any_of(
        m_slots, [&](const Slot& s) { return &s != &slot && s.live && s.keydown; });
    if (m_function.portaMode == 0 && !held && !m_function.mono) {
        return; // retain: only from a note still held
    }
    slot.glide = (fromNote - slot.midinote) * kSemitone;
    // About 4 s across an octave at 99, quickly shorter below.
    const double time = m_function.portaTime / 99.0;
    const double seconds =
        4.0 * time * time * std::max(1.0, std::abs(fromNote - slot.midinote) / 12.0);
    const double blocks = std::max(1.0, seconds * m_rate.sampleRate / msfa::kN);
    slot.glideStep = static_cast<std::int32_t>(std::ceil(std::abs(slot.glide) / blocks));
}

void Dx7Engine::noteOn(int midinote, int velocity, std::uint64_t tag, std::uint32_t source,
                       std::int64_t endTick) noexcept {
    if (m_function.mono) {
        monoOn(midinote, velocity, tag, source, endTick);
        return;
    }
    const int index = allocate();
    if (index < 0) {
        return;
    }
    Slot& slot = m_slots[static_cast<std::size_t>(index)];
    m_lfo.keydown(); // upstream: on every note-on
    slot.midinote = transposed(m_voice, midinote);
    slot.tag = tag;
    slot.source = source;
    slot.endTick = endTick;
    slot.age = m_clock;
    slot.keydown = true;
    slot.sustained = m_sustain;
    slot.live = true;
    slot.note.init(m_rate, m_voice.bytes.data(), slot.midinote, velocity);
    startGlide(slot, m_lastNote);
    m_lastNote = slot.midinote;
}

void Dx7Engine::monoOn(int midinote, int velocity, std::uint64_t tag, std::uint32_t source,
                       std::int64_t endTick) noexcept {
    if (std::cmp_less(m_heldCount, m_held.size())) {
        m_held[static_cast<std::size_t>(m_heldCount++)] =
            Held{.note = midinote, .velocity = velocity, .tag = tag};
    }
    Slot& slot = m_slots[0];
    const int note = transposed(m_voice, midinote);
    const bool legato = slot.live && slot.keydown;
    slot.tag = tag;
    slot.source = source;
    slot.endTick = endTick;
    slot.age = m_clock;
    if (legato) {
        const int from = slot.midinote;
        slot.midinote = note;
        slot.note.retune(m_voice.bytes.data(), note);
        startGlide(slot, from);
    } else {
        m_lfo.keydown();
        slot.midinote = note;
        slot.keydown = true;
        slot.sustained = m_sustain;
        slot.live = true;
        slot.note.init(m_rate, m_voice.bytes.data(), note, velocity);
        startGlide(slot, m_function.portaMode == 1 ? m_lastNote : -1);
    }
    m_lastNote = note;
}

void Dx7Engine::monoOff(std::uint64_t tag) noexcept {
    int at = -1;
    for (int i = 0; i < m_heldCount; ++i) {
        if (m_held[static_cast<std::size_t>(i)].tag == tag) {
            at = i;
        }
    }
    if (at < 0) {
        return;
    }
    const bool top = at == m_heldCount - 1;
    for (int i = at; i + 1 < m_heldCount; ++i) {
        m_held[static_cast<std::size_t>(i)] = m_held[static_cast<std::size_t>(i) + 1];
    }
    --m_heldCount;
    Slot& slot = m_slots[0];
    if (!top || !slot.live) {
        return;
    }
    if (m_heldCount > 0) {
        // Last-note priority: back to the key still held, legato.
        const Held& back = m_held[static_cast<std::size_t>(m_heldCount) - 1];
        const int from = slot.midinote;
        slot.midinote = transposed(m_voice, back.note);
        slot.tag = back.tag;
        slot.note.retune(m_voice.bytes.data(), slot.midinote);
        startGlide(slot, from);
        m_lastNote = slot.midinote;
        return;
    }
    release(slot);
}

// NOLINTNEXTLINE(readability-make-member-function-const) - it reads m_sustain, edits slot
void Dx7Engine::release(Slot& slot) noexcept {
    if (!slot.keydown) {
        return;
    }
    if (m_sustain) {
        slot.sustained = true;
    } else {
        slot.note.keyup();
    }
    slot.keydown = false;
}

void Dx7Engine::noteOff(std::uint64_t tag) noexcept {
    if (m_function.mono) {
        monoOff(tag);
        return;
    }
    for (Slot& slot : m_slots) {
        if (slot.live && slot.keydown && slot.tag == tag) {
            release(slot);
        }
    }
}

void Dx7Engine::releaseAll(std::uint32_t source) noexcept {
    for (Slot& slot : m_slots) {
        if (slot.live && slot.keydown && (source == kAllSources || slot.source == source)) {
            release(slot);
        }
    }
    if (m_function.mono) {
        m_heldCount = 0;
    }
}

void Dx7Engine::releaseFrom(std::uint32_t source, std::int64_t endTick) noexcept {
    for (Slot& slot : m_slots) {
        if (slot.live && slot.keydown && slot.source == source && slot.endTick >= endTick) {
            release(slot);
        }
    }
}

void Dx7Engine::setSustain(bool on) noexcept {
    m_sustain = on;
    if (!on) {
        for (Slot& slot : m_slots) {
            if (slot.live && slot.sustained && !slot.keydown) {
                slot.note.keyup();
                slot.sustained = false;
            }
        }
    }
}

void Dx7Engine::render(std::int32_t* out) noexcept {
    std::fill_n(out, msfa::kN, 0);
    const std::int32_t lfoValue = m_lfo.getsample();
    const std::int32_t lfoDelay = m_lfo.getdelay();
    for (Slot& slot : m_slots) {
        if (!slot.live) {
            continue;
        }
        if (slot.glideStep != 0) {
            const std::int32_t step = std::min(slot.glideStep, std::abs(slot.glide));
            slot.glide += slot.glide > 0 ? -step : step;
            if (slot.glide == 0) {
                slot.glideStep = 0;
            }
        }
        std::int32_t offset = slot.glide;
        if (m_function.portaGliss && offset != 0) {
            offset = (offset / kSemitone) * kSemitone;
        }
        slot.note.setPitchOffset(offset + m_tuning);
        slot.note.compute(out, lfoValue, lfoDelay, m_ctrls);
        if (!slot.keydown && !slot.sustained && slot.note.finished()) {
            slot.live = false;
        }
    }
    ++m_clock;
}

bool Dx7Engine::idle() const noexcept {
    return std::ranges::none_of(m_slots, [](const Slot& s) { return s.live; });
}

int Dx7Engine::liveVoices() const noexcept {
    return static_cast<int>(std::ranges::count_if(m_slots, [](const Slot& s) { return s.live; }));
}

} // namespace adx::dx7

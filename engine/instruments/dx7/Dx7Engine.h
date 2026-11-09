// The dx7 voice manager: msfa's SynthUnit, without its MIDI parser, ring buffer and
// filter (phase_11.5.md §4.2). It owns the notes, the one LFO and the controllers, and
// renders 64-sample fixed-point blocks; Dx7Instrument puts those on the engine's
// timeline. Kept apart from the graph node so the bit-exactness test and the bench
// drive exactly what the instrument plays.
//
// Upstream behaviour kept: one LFO for the instrument, keyed (and reset if it syncs) by
// every note-on; sustain holds released keys; a voice's output is summed in int32.
// adX's: mono with last-note priority and legato, portamento (retain/follow, with
// glissando), the controllers, operator on/off, a polyphony limit with stealing, and a
// voice freed once it can no longer be heard (msfa never frees one).
#pragma once

#include <array>
#include <cstdint>

#include "engine/instruments/dx7/Dx7Voice.h"
#include "engine/instruments/dx7/msfa/Dx7Note.h"
#include "engine/instruments/dx7/msfa/Tables.h"

namespace adx::dx7 {

/// The DX7 function parameters and adX's play settings.
struct Dx7Function {
    bool mono{false};
    int bendRange{2};
    int bendStep{0};
    /// 0 retain (poly) / fingered (mono): glide only when a key is still held;
    /// 1 follow (poly) / full time (mono): glide from the last note always.
    int portaMode{0};
    bool portaGliss{false};
    int portaTime{0};
    /// Wheel, foot, breath, aftertouch: range 0..99 and assign bits.
    std::array<int, 4> range{};
    std::array<int, 4> assign{};
    int polyphony{16};

    friend bool operator==(const Dx7Function&, const Dx7Function&) = default;
};

class Dx7Engine {
public:
    static constexpr int kMaxVoices = 64;

    /// Main thread.
    void prepare(double sampleRate);

    void setVoice(const Dx7Voice& voice) noexcept;
    void setFunction(const Dx7Function& function) noexcept;
    [[nodiscard]] const Dx7Voice& voice() const noexcept {
        return m_voice;
    }

    /// `tag` identifies the note for its note-off; `source` and `endTick` are what a
    /// release-all or a loop wrap match on.
    void noteOn(int midinote, int velocity, std::uint64_t tag, std::uint32_t source = 0,
                std::int64_t endTick = 0) noexcept;
    void noteOff(std::uint64_t tag) noexcept;
    /// Releases every note driven by `source` (all of them for kAllSources).
    void releaseAll(std::uint32_t source) noexcept;
    /// Releases the notes of `source` whose note-off is at or beyond `endTick`.
    void releaseFrom(std::uint32_t source, std::int64_t endTick) noexcept;
    void setSustain(bool on) noexcept;
    void setPitchBend(int value14) noexcept {
        m_ctrls.pitchBend = value14;
    }
    /// 0 wheel, 1 foot, 2 breath, 3 aftertouch; 0..127.
    void setController(int which, int value) noexcept;
    /// Q24 log2 added to every note: the channel's tuning.
    void setTuning(std::int32_t q24) noexcept {
        m_tuning = q24;
    }

    /// One 64-sample block, overwriting `out`.
    void render(std::int32_t* out) noexcept;

    /// No note sounding.
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] int liveVoices() const noexcept;
    /// Back to the start: no notes, the LFO at upstream's initial phase.
    void reset() noexcept;

    static constexpr std::uint32_t kAllSources = 0xFFFFFFFFU;

private:
    struct Slot {
        msfa::Dx7Note note;
        std::uint64_t tag{0};
        std::uint32_t source{0};
        std::int64_t endTick{0};
        std::uint32_t age{0};
        int midinote{0};
        bool keydown{false};
        bool sustained{false};
        bool live{false};
        /// Portamento: the offset now and how much it moves per block, Q24.
        std::int32_t glide{0};
        std::int32_t glideStep{0};
    };

    [[nodiscard]] int allocate() noexcept;
    void startGlide(Slot& slot, int fromNote) noexcept;
    void release(Slot& slot) noexcept;
    void monoOn(int midinote, int velocity, std::uint64_t tag, std::uint32_t source,
                std::int64_t endTick) noexcept;
    void monoOff(std::uint64_t tag) noexcept;

    msfa::RateTables m_rate;
    std::array<Slot, kMaxVoices> m_slots{};
    msfa::Lfo m_lfo;
    msfa::Controllers m_ctrls;
    Dx7Voice m_voice = Dx7Voice::init();
    Dx7Function m_function;
    std::int32_t m_tuning{0};
    bool m_sustain{false};
    int m_current{0};
    std::uint32_t m_clock{0};
    /// The last note started, for portamento; -1 before the first.
    int m_lastNote{-1};
    /// Mono: the keys held, oldest first, with their tags.
    struct Held {
        int note;
        int velocity;
        std::uint64_t tag;
    };
    std::array<Held, 16> m_held{};
    int m_heldCount{0};
};

} // namespace adx::dx7

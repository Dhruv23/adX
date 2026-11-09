// The oracle's one translation unit: upstream msfa's headers, as they are.
#include "tests/cpp/dx7/MsfaOracle.h"

#include <array>
#include <memory>

// Upstream's own include order: synth.h (N, min, max) before everything. Its memory
// barrier is defined first, as cmake/msfa_compat.h does for upstream's sources.
#define SynthMemoryBarrier() // NOLINT(readability-identifier-naming) - upstream's name
// clang-format off
#include "synth.h"
#include "controllers.h"
#include "dx7note.h"
#include "exp2.h"
#include "freqlut.h"
#include "lfo.h"
#include "pitchenv.h"
#include "sin.h"
// clang-format on

namespace adx::test {

std::vector<std::int32_t> msfaRender(double sampleRate, const std::uint8_t* patch,
                                     std::span<const OracleEvent> events, int blocks) {
    // SynthUnit::Init
    Freqlut::init(sampleRate);
    Exp2::init();
    Tanh::init();
    Sin::init();
    Lfo::init(sampleRate);
    PitchEnv::init(sampleRate);

    struct Active {
        int midiNote{0};
        bool keydown{false};
        bool sustained{false};
        bool live{false};
        std::unique_ptr<Dx7Note> note = std::make_unique<Dx7Note>();
    };
    constexpr int kNotes = 32;
    std::array<Active, kNotes> active{};
    std::array<char, 156> unpacked{};
    for (std::size_t i = 0; i < unpacked.size(); ++i) {
        unpacked[i] = static_cast<char>(patch[i]);
    }
    Lfo lfo{};
    lfo.reset(unpacked.data() + 137); // ProgramChange
    Controllers controllers{};
    controllers.values_[kControllerPitch] = 0x2000;
    bool sustain = false;
    int current = 0;

    std::vector<std::int32_t> out;
    out.reserve(static_cast<std::size_t>(blocks) * N);
    std::size_t next = 0;
    for (int block = 0; block < blocks; ++block) {
        while (next < events.size() && events[next].block <= block) {
            const OracleEvent& e = events[next++];
            switch (e.kind) {
            case OracleEvent::Kind::On: {
                int ix = -1;
                int n = current;
                for (int i = 0; i < kNotes; i++) {
                    if (!active[n].keydown) {
                        current = (n + 1) % kNotes;
                        ix = n;
                        break;
                    }
                    n = (n + 1) % kNotes;
                }
                if (ix >= 0) {
                    lfo.keydown();
                    active[ix].midiNote = e.note;
                    active[ix].keydown = true;
                    active[ix].sustained = sustain;
                    active[ix].live = true;
                    active[ix].note->init(unpacked.data(), e.note, e.velocity);
                }
                break;
            }
            case OracleEvent::Kind::Off:
                for (Active& a : active) {
                    if (a.midiNote == e.note && a.keydown) {
                        if (sustain) {
                            a.sustained = true;
                        } else {
                            a.note->keyup();
                        }
                        a.keydown = false;
                    }
                }
                break;
            case OracleEvent::Kind::Sustain:
                sustain = e.value != 0;
                if (!sustain) {
                    for (Active& a : active) {
                        if (a.sustained && !a.keydown) {
                            a.note->keyup();
                            a.sustained = false;
                        }
                    }
                }
                break;
            case OracleEvent::Kind::Bend:
                controllers.values_[kControllerPitch] = e.value;
                break;
            }
        }
        std::array<std::int32_t, N> audio{};
        const std::int32_t lfoValue = lfo.getsample();
        const std::int32_t lfoDelay = lfo.getdelay();
        for (const Active& a : active) {
            if (a.live) {
                a.note->compute(audio.data(), lfoValue, lfoDelay, &controllers);
            }
        }
        out.insert(out.end(), audio.begin(), audio.end());
    }
    return out;
}

} // namespace adx::test

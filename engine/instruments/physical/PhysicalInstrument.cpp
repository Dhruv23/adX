#include "engine/instruments/physical/PhysicalInstrument.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

#include "engine/dsp/Math.h"

namespace adx::instruments {
namespace {

using P = PhysicalParam;

constexpr double kTwoPi = 2.0 * std::numbers::pi;
/// The lowest note a line has room for.
constexpr double kLowestHz = 20.0;
/// A voice this quiet for this long has finished ringing.
constexpr float kQuiet = 1e-6F;
constexpr std::uint32_t kQuietFrames = 4800;

[[nodiscard]] constexpr std::uint32_t idx(P param) noexcept {
    return static_cast<std::uint32_t>(param);
}

[[nodiscard]] int choice(const graph::VoiceRender& render, P param, int highest) noexcept {
    return std::clamp(static_cast<int>(dsp::floorExact(render.param(idx(param)) + 0.5F)), 0,
                      highest);
}

[[nodiscard]] double cosRadians(double omega) noexcept {
    return dsp::cosTurns(omega / kTwoPi);
}

[[nodiscard]] double sinRadians(double omega) noexcept {
    return dsp::sinTurns(omega / kTwoPi);
}

/// The argument of a + b e^{-j omega}, radians.
[[nodiscard]] double argument(double a, double b, double omega) noexcept {
    return kTwoPi * dsp::atan2Turns(-b * sinRadians(omega), a + (b * cosRadians(omega)));
}

/// Reads `line` `delay` (fractional, >= 1) samples behind `write`, linearly.
[[nodiscard]] float readBehind(std::span<const float> line, std::size_t mask, std::uint32_t write,
                               float delay) noexcept {
    const float whole = dsp::floorExact(delay);
    const float fraction = delay - whole;
    const auto at = static_cast<std::size_t>(write - static_cast<std::uint32_t>(whole));
    const float a = line[at & mask];
    const float b = line[(at - 1) & mask];
    return a + ((b - a) * fraction);
}

/// A resonator's two feedback coefficients for `hz` decaying with radius `r`.
void resonator(double hz, double r, double rate, float& b1, float& b2) noexcept {
    b1 = static_cast<float>(2.0 * r * cosRadians(kTwoPi * hz / rate));
    b2 = static_cast<float>(r * r);
}

/// Three body resonances per type: Hz, Q, gain.
struct BodyMode {
    float hz;
    float q;
    float gain;
};
// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
constexpr std::array<std::array<BodyMode, 3>, kPhysicalBodies> kBodies{{
    {{{0, 1, 0}, {0, 1, 0}, {0, 1, 0}}},
    {{{102, 12, 1.0F}, {204, 14, 0.8F}, {410, 8, 0.5F}}},  // guitar: air, top, back
    {{{275, 15, 0.8F}, {460, 12, 1.0F}, {2800, 3, 0.6F}}}, // violin: A0, B1, bridge hill
    {{{98, 14, 1.0F}, {205, 12, 0.8F}, {560, 6, 0.5F}}},   // cello
    {{{620, 6, 0.7F}, {1850, 5, 0.5F}, {3400, 4, 0.3F}}},  // steel-pan skirt
}};
// NOLINTEND(modernize-use-designated-initializers)

[[nodiscard]] std::uint32_t voiceSeed(const graph::Voice& voice) noexcept {
    return modVoiceSeed(voice);
}

} // namespace

double allpassDelay(double eta, double omega) noexcept {
    const double phase = argument(eta, 1.0, omega) - argument(1.0, eta, omega);
    return -phase / omega;
}

double allpassFor(double delay, double omega) noexcept {
    // The phase delay falls as eta rises; bisect.
    double low = -0.95;
    double high = 0.95;
    for (int i = 0; i < 48; ++i) {
        const double mid = 0.5 * (low + high);
        if (allpassDelay(mid, omega) > delay) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return 0.5 * (low + high);
}

double onePoleDelay(double a, double omega) noexcept {
    // H = (1 - a) / (1 - a e^{-jw}): its phase is minus that of the denominator.
    return a == 0.0 ? 0.0 : argument(1.0, -a, omega) / omega;
}

double onePoleGain(double a, double omega) noexcept {
    return (1.0 - a) / std::sqrt(1.0 - (2.0 * a * cosRadians(omega)) + (a * a));
}

void PhysicalInstrument::prepareInstrument(const graph::PrepareInfo& info) {
    m_lineSize = std::bit_ceil(static_cast<std::size_t>(info.sampleRate / kLowestHz) + 16);
    m_lineMask = m_lineSize - 1;
    m_lines.allocate(voiceSlots() * 2 * m_lineSize);
}

std::span<float> PhysicalInstrument::lineA(const graph::Voice& voice) noexcept {
    return m_lines.view().subspan(voiceIndex(voice) * 2 * m_lineSize, m_lineSize);
}

std::span<float> PhysicalInstrument::lineB(const graph::Voice& voice) noexcept {
    return m_lines.view().subspan((voiceIndex(voice) * 2 * m_lineSize) + m_lineSize, m_lineSize);
}

float PhysicalInstrument::portamentoSeconds(std::span<const float> params) const noexcept {
    const std::uint32_t at = idx(P::Glide);
    return at < params.size() ? std::max(0.0F, params[at]) : 0.0F;
}

std::uint8_t PhysicalInstrument::voiceMode(std::span<const float> params) const noexcept {
    return static_cast<std::uint8_t>(modVoiceMode(params, kPhysicalMod));
}

bool PhysicalInstrument::glideAlways(std::span<const float> params) const noexcept {
    return modGlideAlways(params, kPhysicalMod);
}

void PhysicalInstrument::setBody(PhysicalVoice& state, std::uint8_t type) const noexcept {
    state.body = type;
    const auto rate = static_cast<double>(sampleRate());
    for (std::size_t b = 0; b < 3; ++b) {
        const BodyMode& mode = kBodies.at(type).at(b);
        if (mode.hz <= 0.0F) {
            state.bodyB1.at(b) = state.bodyB2.at(b) = state.bodyGain.at(b) = 0.0F;
            continue;
        }
        const double r = dsp::exp(-std::numbers::pi * mode.hz / (mode.q * rate));
        resonator(mode.hz, r, rate, state.bodyB1.at(b), state.bodyB2.at(b));
        // Unity at resonance: a two-pole peak is 1 / ((1 - r) 2 sin(theta)).
        state.bodyGain.at(b) =
            static_cast<float>((1.0 - r) * 2.0 * sinRadians(kTwoPi * mode.hz / rate)) * mode.gain;
    }
    state.bodyY1.fill(0.0F);
    state.bodyY2.fill(0.0F);
}

// NOLINTNEXTLINE(readability-function-size) - one tuning per model.
void PhysicalInstrument::tune(PhysicalVoice& state, const graph::VoiceRender& render,
                              std::uint32_t frame, float hz, const dsp::ModFrame& mod) noexcept {
    const auto rate = static_cast<double>(sampleRate());
    const double f0 = std::clamp(static_cast<double>(hz), kLowestHz, 0.45 * rate);
    state.tunedHz = hz;
    const double period = rate / f0;
    const double omega = kTwoPi * f0 / rate;
    switch (state.model) {
    case PhysicalModel::Pluck: {
        const float mute = std::clamp(render.paramAt(idx(P::Mute), frame), 0.0F, 1.0F);
        // Damping, moved by a Cutoff slot (octaves up: brighter), and by the mute.
        const double damping =
            std::clamp(static_cast<double>(render.paramAt(idx(P::StringDamping), frame)) -
                           (0.1 * static_cast<double>(mod.of(dsp::ModDest::Cutoff))) + (0.5 * mute),
                       0.0, 1.0);
        // The loss pole, kept below what would eat the whole period on a high note.
        double a = 0.85 * damping;
        while (a > 0.0 && onePoleDelay(a, omega) > 0.3 * period) {
            a *= 0.8;
        }
        const double dispersion =
            -0.6 * std::clamp(static_cast<double>(render.paramAt(idx(P::StringDispersion), frame)),
                              0.0, 1.0);
        const double lossDelay = onePoleDelay(a, omega);
        const double dispDelay = dispersion != 0.0 ? 2.0 * allpassDelay(dispersion, omega) : 0.0;
        const double rest = std::max(2.0, period - lossDelay - dispDelay);
        const double whole = std::max(1.0, dsp::floorExact(rest - 0.5));
        state.integerDelay = static_cast<std::uint32_t>(whole);
        state.eta = static_cast<float>(allpassFor(rest - whole, omega));
        state.lossA = static_cast<float>(a);
        state.dispersion = static_cast<float>(dispersion);
        // The fundamental falls 60 dB in string.decay at C4, scaled by the key.
        const double decay = static_cast<double>(render.paramAt(idx(P::StringDecay), frame)) *
                             std::sqrt(261.6255653 / f0) *
                             (1.0 - (0.9 * static_cast<double>(mute)));
        const double perPeriod = dsp::exp2(-3.0 * dsp::kLog2Of10 / (std::max(decay, 0.01) * f0));
        state.loopGain = static_cast<float>(std::min(0.99999, perPeriod / onePoleGain(a, omega)));
        break;
    }
    case PhysicalModel::Bow: {
        const double length = std::max(4.0, period - 4.0);
        const double beta =
            std::clamp(static_cast<double>(render.paramAt(idx(P::BowPosition), frame)), 0.02, 0.5);
        state.lengthA = static_cast<float>(std::max(2.0, length * (1.0 - beta)));
        state.lengthB = static_cast<float>(std::max(2.0, length * beta));
        state.lossA = 0.6F;
        const double decay = static_cast<double>(render.paramAt(idx(P::StringDecay), frame)) * 0.25;
        state.loopGain = static_cast<float>(
            std::min(0.995, dsp::exp2(-3.0 * dsp::kLog2Of10 / (std::max(decay, 0.01) * f0)) /
                                onePoleGain(0.6, omega)));
        break;
    }
    case PhysicalModel::Modal: {
        const auto& table =
            kModeTables.at(static_cast<std::size_t>(choice(render, P::ModesTable, 15)));
        const double decay = static_cast<double>(render.paramAt(idx(P::ModesDecay), frame)) *
                             dsp::exp2(static_cast<double>(mod.of(dsp::ModDest::ModalDecay)));
        const double tilt = render.paramAt(idx(P::ModesDecayTilt), frame);
        state.modeCount = 0;
        for (std::size_t m = 0; m < table.count; ++m) {
            const double modeHz = f0 * static_cast<double>(table.ratio.at(m));
            if (modeHz >= 0.45 * rate) {
                continue;
            }
            const std::size_t k = state.modeCount++;
            const double modeDecay =
                decay /
                (1.0 + (tilt * std::max(0.0, static_cast<double>(table.ratio.at(m)) - 1.0)));
            const double r = dsp::exp2(-3.0 * dsp::kLog2Of10 / (std::max(modeDecay, 0.005) * rate));
            resonator(modeHz, r, rate, state.modeB1.at(k), state.modeB2.at(k));
            state.modeGain.at(k) =
                table.gain.at(m) * static_cast<float>(sinRadians(kTwoPi * modeHz / rate));
            state.modeRatio.at(k) = table.ratio.at(m);
        }
        break;
    }
    case PhysicalModel::Tube: {
        const int kind = choice(render, P::TubeKind, kPhysicalTubes - 1);
        if (kind == 0) {
            state.lengthA = static_cast<float>(std::max(4.0, period - 2.0));
            state.lengthB =
                std::max(2.0F, state.lengthA * std::clamp(render.paramAt(idx(P::JetRatio), frame),
                                                          0.05F, 0.6F));
        } else if (kind == 1) {
            // A closed cylinder: a quarter wave, so the line is half the period.
            state.lengthA = static_cast<float>(std::max(4.0, (period * 0.5) - 1.5));
        } else {
            state.lengthA = static_cast<float>(std::max(4.0, period - 3.0));
        }
        state.lossA = static_cast<float>(0.7 - (0.1 * 22050.0 / rate));
        break;
    }
    }
}

void PhysicalInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                    const graph::VoiceRender& render) noexcept {
    PhysicalVoice& state = stateOf(voice);
    std::ranges::fill(lineA(voice), 0.0F);
    std::ranges::fill(lineB(voice), 0.0F);
    state.model = static_cast<PhysicalModel>(choice(render, P::Model, kPhysicalModels - 1));
    state.write = 0;
    state.fracX1 = state.fracY1 = state.lossState = 0.0F;
    state.dispX1.fill(0.0F);
    state.dispY1.fill(0.0F);
    state.filterState = state.dcX1 = state.dcY1 = 0.0F;
    state.modeY1.fill(0.0F);
    state.modeY2.fill(0.0F);
    state.noise.reseed(voiceSeed(voice));
    state.velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.quietFrames = 0;
    state.released = false;
    state.decayScale = 1.0F;
    modStart(state.mod, voice, render, kPhysicalMod, voiceSeed(voice),
             static_cast<double>(pool().clock()) / static_cast<double>(sampleRate()));
    const float hz = dsp::midiToHz(static_cast<float>(voice.pitch));
    tune(state, render, 0, hz, dsp::neutralModFrame());
    setBody(state, static_cast<std::uint8_t>(choice(render, P::BodyType, kPhysicalBodies - 1)));

    // The pluck's burst: one period of noise, low-passed by brightness and hardness.
    const auto rate = static_cast<float>(sampleRate());
    const float brightness = std::clamp(render.param(idx(P::StringBrightness)), 0.0F, 1.0F);
    const float hardness = std::clamp(render.param(idx(P::ExciterHardness)), 0.0F, 1.0F);
    const float cutoff =
        150.0F * dsp::exp2F((brightness * 6.0F) + (hardness * 1.5F) + (state.velocity * 1.0F));
    state.exciteCoefficient = dsp::exp2F(-cutoff / rate * 9.0647F); // e^{-2 pi fc / fs}
    state.exciteLp = 0.0F;
    const float period = rate / std::max(hz, 20.0F);
    state.exciteLeft = static_cast<std::uint32_t>(std::max(2.0F, period));
    state.pickDelay = static_cast<std::uint32_t>(
        std::max(1.0F, period * std::clamp(render.param(idx(P::ExciterPosition)), 0.02F, 0.5F)));
    state.thunk = std::clamp(render.param(idx(P::Mute)), 0.0F, 1.0F) * state.velocity * 0.5F;

    // The mallet: a half-sine whose contact time hardness sets, 5 ms to 0.3 ms.
    const float contact = 0.005F * dsp::exp2F(-hardness * 4.06F);
    state.strikeLength = static_cast<std::uint32_t>(std::max(2.0F, contact * rate));
    state.strikeLeft = state.strikeLength;

    state.shape.attackSeconds = render.param(idx(P::EnvAttack));
    state.shape.decaySeconds = render.param(idx(P::EnvDecay));
    state.shape.sustain = render.param(idx(P::EnvSustain));
    state.shape.releaseSeconds = render.param(idx(P::EnvRelease));
    state.env.reset();
    state.env.trigger(state.shape, sampleRate());
    voice.level = 0.0F;
}

void PhysicalInstrument::retriggerVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                        const graph::VoiceRender& render) noexcept {
    PhysicalVoice& state = stateOf(voice);
    state.velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.released = false;
    state.quietFrames = 0;
    // A new pluck or strike into what is already ringing; bow and breath just swell.
    state.exciteLeft = static_cast<std::uint32_t>(
        std::max(2.0F, static_cast<float>(sampleRate()) / std::max(state.tunedHz, 20.0F)));
    state.strikeLeft = state.strikeLength;
    state.env.trigger(state.shape, sampleRate());
    modRetrigger(state.mod, render, kPhysicalMod);
}

float PhysicalInstrument::pluck(PhysicalVoice& state, std::span<float> line,
                                std::span<float> pick) const noexcept {
    const float read = line[(state.write - state.integerDelay) & m_lineMask];
    // Fractional delay: y = eta x + x1 - eta y1.
    const float frac = (state.eta * read) + state.fracX1 - (state.eta * state.fracY1);
    state.fracX1 = read;
    state.fracY1 = frac;
    state.lossState = ((1.0F - state.lossA) * frac) + (state.lossA * state.lossState);
    float y = state.lossState;
    if (state.dispersion != 0.0F) {
        for (std::size_t s = 0; s < 2; ++s) {
            const float out = (state.dispersion * y) + state.dispX1.at(s) -
                              (state.dispersion * state.dispY1.at(s));
            state.dispX1.at(s) = y;
            state.dispY1.at(s) = out;
            y = out;
        }
    }
    y *= state.loopGain;
    float excite = 0.0F;
    if (state.exciteLeft > 0) {
        --state.exciteLeft;
        state.exciteLp = ((1.0F - state.exciteCoefficient) * state.noise.next()) +
                         (state.exciteCoefficient * state.exciteLp);
        excite = state.exciteLp * state.velocity;
    }
    // The pick position's comb: the burst minus itself a fraction of a period later.
    pick[state.write & m_lineMask] = excite;
    const float combed = excite - pick[(state.write - state.pickDelay) & m_lineMask];
    line[state.write & m_lineMask] = y + combed;
    return y;
}

float PhysicalInstrument::bow(PhysicalVoice& state, std::span<float> neck, std::span<float> bridge,
                              float velocity, float pressure) const noexcept {
    const float bridgeOut = readBehind(bridge, m_lineMask, state.write, state.lengthB);
    const float neckOut = readBehind(neck, m_lineMask, state.write, state.lengthA);
    state.lossState = ((1.0F - state.lossA) * bridgeOut) + (state.lossA * state.lossState);
    const float bridgeReflection = -state.lossState * state.loopGain;
    const float nutReflection = -neckOut;
    const float stringVelocity = bridgeReflection + nutReflection;
    const float difference = velocity - stringVelocity;
    // The friction table: (|slope (dv + offset)| + 0.75)^-4, at most 1.
    const float slope = 5.0F - (4.0F * std::clamp(pressure, 0.0F, 1.0F));
    const float x = std::abs((difference + 0.001F) * slope) + 0.75F;
    const float x2 = x * x;
    const float friction = std::min(1.0F, 1.0F / (x2 * x2));
    const float pushed = difference * friction;
    neck[state.write & m_lineMask] = bridgeReflection + pushed;
    bridge[state.write & m_lineMask] = nutReflection + pushed;
    return bridgeOut;
}

float PhysicalInstrument::modal(PhysicalVoice& state, float velocity) noexcept {
    float x = 0.0F;
    if (state.strikeLeft > 0) {
        const auto n = static_cast<float>(state.strikeLength - state.strikeLeft);
        x = velocity * dsp::sinTurnsF(0.5F * n / static_cast<float>(state.strikeLength)) *
            (std::numbers::pi_v<float> / static_cast<float>(state.strikeLength));
        --state.strikeLeft;
    }
    float out = 0.0F;
    for (std::uint32_t k = 0; k < state.modeCount; ++k) {
        const float y = (state.modeB1.at(k) * state.modeY1.at(k)) -
                        (state.modeB2.at(k) * state.modeY2.at(k)) + (state.modeGain.at(k) * x);
        state.modeY2.at(k) = state.modeY1.at(k);
        state.modeY1.at(k) = y;
        out += y;
    }
    return out;
}

float PhysicalInstrument::tube(PhysicalVoice& state, std::span<float> bore, std::span<float> jet,
                               float breath, float noise, float stiffness,
                               int kind) const noexcept {
    const float boreOut = readBehind(bore, m_lineMask, state.write, state.lengthA);
    const float pole = state.lossA;
    if (kind == 0) {
        // Flute: the jet's delay, its cubic, two reflections.
        state.filterState = (-(1.0F - pole) * boreOut) + (pole * state.filterState);
        const float blocked = state.filterState - state.dcX1 + (0.995F * state.dcY1);
        state.dcX1 = state.filterState;
        state.dcY1 = blocked;
        const float drive = breath + (noise * breath) - (0.5F * blocked);
        jet[state.write & m_lineMask] = drive;
        const float jetOut = readBehind(jet, m_lineMask, state.write, state.lengthB);
        const float cubic = std::clamp(jetOut * ((jetOut * jetOut) - 1.0F), -1.0F, 1.0F);
        bore[state.write & m_lineMask] = cubic + (0.5F * blocked);
        return 0.3F * boreOut;
    }
    // Clarinet and saxophone: a reed whose stiffness sets the table's slope.
    state.filterState = (-0.95F * (1.0F - pole) * boreOut) + (pole * state.filterState);
    const float pressure = breath * (1.0F + (noise * 0.2F));
    const float difference = state.filterState - pressure;
    const float slope = -0.44F + (0.26F * std::clamp(stiffness, 0.0F, 1.0F));
    const float reed = std::clamp(0.7F + (slope * difference), -1.0F, 1.0F);
    bore[state.write & m_lineMask] = pressure + (difference * reed);
    return 0.4F * boreOut;
}

float PhysicalInstrument::bodyOf(PhysicalVoice& state, float x, float mix) noexcept {
    if (state.body == 0 || mix <= 0.0F) {
        return x;
    }
    float resonance = 0.0F;
    for (std::size_t b = 0; b < 3; ++b) {
        const float y = (state.bodyB1.at(b) * state.bodyY1.at(b)) -
                        (state.bodyB2.at(b) * state.bodyY2.at(b)) + (state.bodyGain.at(b) * x);
        state.bodyY2.at(b) = state.bodyY1.at(b);
        state.bodyY1.at(b) = y;
        resonance += y;
    }
    return x + (mix * resonance);
}

// NOLINTNEXTLINE(readability-function-size) - one voice: modulation, model, body, amp.
bool PhysicalInstrument::renderVoice(graph::Voice& voice, std::span<float> left,
                                     std::span<float> right,
                                     const graph::VoiceRender& render) noexcept {
    PhysicalVoice& state = stateOf(voice);
    if (voice.phase == graph::VoicePhase::Released && !state.released) {
        state.released = true;
        state.env.release(state.shape, sampleRate());
    }
    modBegin(state.mod, voice, render, kPhysicalMod);
    dsp::ModFrame scratch;
    const std::span<float> a = lineA(voice);
    const std::span<float> b = lineB(voice);
    const float level = dsp::dbToGainF(render.param(idx(P::Level)));
    const bool levelAutomated =
        idx(P::Level) < render.automation.size() && render.automation[idx(P::Level)] != nullptr;
    const auto pitch = static_cast<float>(voice.pitch);
    const int tubeKind = choice(render, P::TubeKind, kPhysicalTubes - 1);
    dsp::MidiHz hzOf;

    bool alive = true;
    float env = state.env.level();
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        const bool onGrid = control(voice, i);
        const dsp::ModFrame& mod =
            modTick(state.mod, voice, render, kPhysicalMod, i, onGrid, scratch);
        if (onGrid) {
            const float cents = i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F;
            const float hz = hzOf(pitch + (cents * 0.01F) + mod.of(dsp::ModDest::Pitch));
            const bool moved = std::abs(hz - state.tunedHz) > 1e-4F * state.tunedHz;
            const bool modulated = &mod == &scratch;
            if (moved || modulated) {
                tune(state, render, i, hz, mod);
            }
        }
        env = state.env.next(state.shape);
        const float exciter = mod.of(dsp::ModDest::Exciter);
        float y = 0.0F;
        switch (state.model) {
        case PhysicalModel::Pluck:
            y = pluck(state, a, b);
            if (state.thunk > 1e-5F) {
                y += state.thunk * state.noise.next() * 0.3F;
                state.thunk *= 0.993F;
            }
            y *= env;
            break;
        case PhysicalModel::Bow: {
            const float speed = std::clamp(render.paramAt(idx(P::BowSpeed), i), 0.0F, 1.0F);
            const float velocity = (0.03F + (0.2F * speed)) * env * (0.5F + state.velocity);
            y = bow(state, a, b, velocity, render.paramAt(idx(P::BowPressure), i) + exciter) * 2.0F;
            break;
        }
        case PhysicalModel::Modal:
            y = modal(state, state.velocity) * env;
            break;
        case PhysicalModel::Tube: {
            const float breath =
                std::clamp(render.paramAt(idx(P::BreathPressure), i) + exciter, 0.0F, 1.0F) * env *
                (0.6F + (0.4F * state.velocity));
            const float noise =
                state.noise.next() * std::clamp(render.paramAt(idx(P::BreathNoise), i), 0.0F, 1.0F);
            y = tube(state, a, b, breath, noise, render.paramAt(idx(P::ReedStiffness), i),
                     tubeKind);
            break;
        }
        }
        ++state.write;
        y = bodyOf(state, y, std::clamp(render.paramAt(idx(P::BodyMix), i), 0.0F, 1.0F));
        const float gain =
            (levelAutomated ? dsp::dbToGainF(render.paramAt(idx(P::Level), i)) : level) *
            mod.of(dsp::ModDest::Level);
        float l = y * gain;
        float r = l;
        modPan(mod.of(dsp::ModDest::Pan), l, r);
        left[i] = l;
        right[i] = r;

        // Done: the envelope's release is over, or the string has stopped ringing.
        state.quietFrames = std::abs(y) < kQuiet ? state.quietFrames + 1 : 0;
        const bool sustained =
            state.model == PhysicalModel::Bow || state.model == PhysicalModel::Tube;
        if (state.env.finished() ||
            (state.quietFrames > kQuietFrames && (!sustained || state.released) &&
             state.exciteLeft == 0 && state.strikeLeft == 0)) {
            alive = false;
        }
    }
    voice.level = env * state.velocity;
    return alive;
}

} // namespace adx::instruments

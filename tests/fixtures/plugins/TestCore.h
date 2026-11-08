// adx_test_plugin's DSP and misbehaviour, shared by its CLAP and VST3 builds
// (phase_9.md §4.8).
//
// Two plugins:
//   adX Test Effect  stereo in/out: delays the input by `latency` samples (reported as
//                    its latency) and multiplies by `gain`.
//   adX Test Synth   an instrument: a sine per held key, 16 voices, a 5 ms attack and a
//                    50 ms release, times `gain`.
//
// The `mode` parameter picks a misbehaviour while running; the ADX_TEST_PLUGIN
// environment variable picks one at load, before any parameter exists:
//   crash         crash in the module's entry point
//   hang[:secs]   sleep in the entry point (default 60 s)
//   dialog        open a modal message box in the entry point
//
// Everything is deterministic: two instances given the same state and the same input
// produce the same output, bit for bit.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace adxtest {

enum Mode : int {
    kNormal = 0,
    kCrashOnProcess = 1,
    kHang = 2,
    kAllocate = 3,
    kWrongLatency = 4,
    kChangeLatency = 5,
    kMalformedState = 6,
    kNaN = 7,
    kSlow = 8,
    kModeCount = 9,
};

enum ParamId : std::uint32_t {
    kGain = 1,
    kMode = 2,
    kLatency = 3,
    kSeed = 4,
};

constexpr std::uint32_t kMaxLatency = 4096;
/// How far a change-latency plugin moves its latency, after kChangeAfterBlocks blocks.
constexpr std::uint32_t kLatencyStep = 64;
constexpr std::uint64_t kChangeAfterBlocks = 100;
constexpr std::array<char, 4> kStateMagic{'A', 'D', 'X', 'T'};

/// The load-time misbehaviour, if ADX_TEST_PLUGIN asks for one.
inline void misbehaveAtLoad() {
    std::array<char, 64> value{};
    const DWORD length = GetEnvironmentVariableA("ADX_TEST_PLUGIN", value.data(),
                                                 static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) {
        return;
    }
    const std::string mode(value.data(), length);
    if (mode == "crash") {
        volatile int* null = nullptr;
        *null = 1; // an access violation, as a real plugin's would be
    } else if (mode.rfind("hang", 0) == 0) {
        int seconds = 60;
        if (mode.size() > 5 && mode[4] == ':') {
            seconds = std::atoi(mode.c_str() + 5);
        }
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    } else if (mode == "dialog") {
        MessageBoxA(nullptr, "adx_test_plugin: a modal dialog during the scan", "adX test",
                    MB_OK | MB_ICONINFORMATION);
    }
}

struct State {
    double gain{1.0};
    int mode{kNormal};
    std::uint32_t latency{0};
    double seed{0.0};
};

/// The two plugins' shared engine. Parameter values arrive already in plain units.
class Core {
public:
    explicit Core(bool instrument) : m_instrument(instrument) {}

    void activate(double sampleRate, std::uint32_t /*maxFrames*/) {
        m_sampleRate = sampleRate;
        m_activeLatency = m_state.latency;
        m_delay.assign(kMaxLatency + kLatencyStep * 4 + 1, {0.0F, 0.0F});
        m_write = 0;
        m_blocks = 0;
        m_voices = {};
    }

    /// What the plugin reports. Wrong on purpose in kWrongLatency.
    [[nodiscard]] std::uint32_t reportedLatency() const noexcept {
        if (m_instrument || m_state.mode == kWrongLatency) {
            return 0;
        }
        return m_activeLatency;
    }

    void setParam(std::uint32_t id, double value) noexcept {
        switch (id) {
        case kGain:
            m_state.gain = value;
            break;
        case kMode:
            m_state.mode = std::clamp(static_cast<int>(std::lround(value)), 0, kModeCount - 1);
            break;
        case kLatency:
            m_state.latency = static_cast<std::uint32_t>(
                std::clamp<long>(std::lround(value), 0, static_cast<long>(kMaxLatency)));
            break;
        case kSeed:
            m_state.seed = value;
            break;
        default:
            break;
        }
    }

    [[nodiscard]] double param(std::uint32_t id) const noexcept {
        switch (id) {
        case kGain:
            return m_state.gain;
        case kMode:
            return m_state.mode;
        case kLatency:
            return m_state.latency;
        case kSeed:
            return m_state.seed;
        default:
            return 0.0;
        }
    }

    void noteOn(int key, float velocity, int noteId = -1) noexcept {
        Voice* slot = &m_voices[0];
        for (Voice& v : m_voices) {
            if (!v.active) {
                slot = &v;
                break;
            }
        }
        *slot = Voice{};
        slot->active = true;
        slot->key = key;
        slot->noteId = noteId;
        slot->velocity = velocity;
        slot->increment = 2.0 * 3.14159265358979323846 * 440.0 *
                          std::pow(2.0, (key - 69) / 12.0) / m_sampleRate;
    }

    /// A tuning expression: the voice with `noteId` (or, given -1, every voice on
    /// `key`) plays `semitones` from its key (P9-7).
    void tune(int noteId, int key, double semitones) noexcept {
        for (Voice& v : m_voices) {
            if (v.active && ((noteId >= 0 && v.noteId == noteId) || (noteId < 0 && (key < 0 || v.key == key)))) {
                v.increment = 2.0 * 3.14159265358979323846 * 440.0 *
                              std::pow(2.0, (v.key - 69 + semitones) / 12.0) / m_sampleRate;
            }
        }
    }

    void noteOff(int key) noexcept {
        for (Voice& v : m_voices) {
            if (v.active && !v.released && (key < 0 || v.key == key)) {
                v.released = true;
            }
        }
    }

    /// The start of a block: the misbehaviour, if any.
    void beginBlock(std::uint32_t frames) {
        ++m_blocks;
        switch (m_state.mode) {
        case kCrashOnProcess: {
            volatile int* null = nullptr;
            *null = 1;
            break;
        }
        case kHang:
            if (!m_hung) {
                m_hung = true;
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
            break;
        case kAllocate: {
            // A plugin that allocates on its audio thread: harmless to adX, which is
            // the point (phase_9.md section 4.5).
            std::vector<float> junk(4096, 1.0F);
            m_sink += junk[frames % junk.size()];
            break;
        }
        case kSlow:
            std::this_thread::sleep_for(std::chrono::microseconds(
                static_cast<long long>(2e6 * frames / m_sampleRate)));
            break;
        default:
            break;
        }
    }

    /// Frames [offset, offset + length) of the block. The wrappers call this between
    /// events, so a note starts on its own sample.
    void render(const float* const* in, float* const* out, std::uint32_t offset,
                std::uint32_t length) noexcept {
        const auto gain = static_cast<float>(m_state.gain);
        if (m_instrument) {
            renderVoices(out, offset, length, gain);
            return;
        }
        const std::size_t size = m_delay.size();
        for (std::uint32_t i = offset; i < offset + length; ++i) {
            m_delay[m_write] = {in[0][i], in[1][i]};
            const std::size_t read = (m_write + size - m_activeLatency) % size;
            out[0][i] = m_delay[read][0] * gain;
            out[1][i] = m_delay[read][1] * gain;
            m_write = (m_write + 1) % size;
        }
    }

    /// The end of a block. Returns true when the plugin wants a restart (its latency is
    /// about to change).
    bool endBlock(float* const* out, std::uint32_t /*frames*/) noexcept {
        if (m_state.mode == kNaN) {
            out[0][0] = std::numeric_limits<float>::quiet_NaN();
        }
        if (m_state.mode == kChangeLatency && m_blocks == kChangeAfterBlocks) {
            m_state.latency = std::min(m_state.latency + kLatencyStep, kMaxLatency);
        }
        // A new latency takes effect only through a restart, as both formats require.
        return !m_instrument && m_state.latency != m_activeLatency;
    }

    [[nodiscard]] std::vector<std::uint8_t> save() const {
        std::vector<std::uint8_t> out;
        if (m_state.mode == kMalformedState) {
            // Garbage its own load rejects (phase_9.md §5 plugin_malformed_state_handled).
            out.assign({'n', 'o', 't', ' ', 'a', ' ', 's', 't', 'a', 't', 'e'});
            return out;
        }
        out.insert(out.end(), kStateMagic.begin(), kStateMagic.end());
        append(out, m_state.gain);
        append(out, static_cast<double>(m_state.latency));
        append(out, m_state.seed);
        return out;
    }

    /// The mode is not state: a project reopened does not crash again by itself.
    bool load(const std::uint8_t* data, std::size_t size) {
        constexpr std::size_t kSize = 4 + 3 * sizeof(double);
        if (size != kSize || std::memcmp(data, kStateMagic.data(), 4) != 0) {
            return false;
        }
        double latency = 0.0;
        std::size_t at = 4;
        read(data, at, m_state.gain);
        read(data, at, latency);
        read(data, at, m_state.seed);
        m_state.latency = static_cast<std::uint32_t>(std::clamp(latency, 0.0, double{kMaxLatency}));
        // State arrives before processing starts, so it may set the latency outright.
        m_activeLatency = m_state.latency;
        return true;
    }

    [[nodiscard]] bool instrument() const noexcept {
        return m_instrument;
    }

private:
    struct Voice {
        bool active{false};
        bool released{false};
        int key{0};
        int noteId{-1};
        float velocity{0.0F};
        double phase{0.0};
        double increment{0.0};
        float level{0.0F};
    };

    void renderVoices(float* const* out, std::uint32_t offset, std::uint32_t length,
                      float gain) noexcept {
        std::fill_n(out[0] + offset, length, 0.0F);
        std::fill_n(out[1] + offset, length, 0.0F);
        const auto attack = static_cast<float>(1.0 / (0.005 * m_sampleRate));
        const auto release = static_cast<float>(1.0 / (0.050 * m_sampleRate));
        for (Voice& v : m_voices) {
            if (!v.active) {
                continue;
            }
            for (std::uint32_t i = offset; i < offset + length; ++i) {
                v.level = v.released ? std::max(0.0F, v.level - release)
                                     : std::min(1.0F, v.level + attack);
                const auto sample =
                    static_cast<float>(std::sin(v.phase)) * v.level * v.velocity * 0.25F * gain;
                v.phase += v.increment;
                if (v.phase > 2.0 * 3.14159265358979323846) {
                    v.phase -= 2.0 * 3.14159265358979323846;
                }
                out[0][i] += sample;
                out[1][i] += sample;
            }
            if (v.released && v.level <= 0.0F) {
                v.active = false;
            }
        }
    }

    static void append(std::vector<std::uint8_t>& out, double value) {
        std::array<std::uint8_t, sizeof(double)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof value);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    static void read(const std::uint8_t* data, std::size_t& at, double& out) {
        std::memcpy(&out, data + at, sizeof out);
        at += sizeof out;
    }

    bool m_instrument;
    State m_state;
    double m_sampleRate{48000.0};
    std::uint32_t m_activeLatency{0};
    std::vector<std::array<float, 2>> m_delay;
    std::size_t m_write{0};
    std::uint64_t m_blocks{0};
    bool m_hung{false};
    float m_sink{0.0F};
    std::array<Voice, 16> m_voices{};
};

} // namespace adxtest

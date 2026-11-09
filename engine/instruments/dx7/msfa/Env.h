/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// Ported from msfa's env.h/env.cc, pitchenv.h/pitchenv.cc and lfo.h/lfo.cc.
// Changes (adX, 2026): namespaced; inline; the rate units come from RateTables instead
// of statics; Env and PitchEnv gain update() (new rates/levels on a sounding note, the
// level kept: adX's live editing, phase_11.5.md §4.3) and finished(); Lfo reads its
// unit from RateTables and gains resetPhase(). The arithmetic is upstream's.
#pragma once

#include <array>
#include <cstdint>

#include "engine/instruments/dx7/msfa/Tables.h"

namespace adx::dx7::msfa {

/// DX7 operator envelope. Levels in Q24 log2 (2^24 is one doubling).
class Env {
public:
    void init(const std::array<int, 4>& rates, const std::array<int, 4>& levels, int outlevel,
              int rateScaling) noexcept {
        m_rates = rates;
        m_levels = levels;
        m_outlevel = outlevel;
        m_rateScaling = rateScaling;
        m_level = 0;
        m_down = true;
        advance(0);
    }

    /// adX: new parameters on a sounding note; the level and stage are kept and the
    /// current stage's target and rate recomputed.
    void update(const std::array<int, 4>& rates, const std::array<int, 4>& levels, int outlevel,
                int rateScaling) noexcept {
        m_rates = rates;
        m_levels = levels;
        m_outlevel = outlevel;
        m_rateScaling = rateScaling;
        if (m_ix < 4) {
            advance(m_ix);
        }
    }

    std::int32_t getsample() noexcept {
        if (m_ix < 3 || ((m_ix < 4) && !m_down)) {
            if (m_rising) {
                constexpr int kJumpTarget = 1716;
                if (m_level < (kJumpTarget << 16)) {
                    m_level = kJumpTarget << 16;
                }
                m_level += (((17 << 24) - m_level) >> 24) * m_inc;
                if (m_level >= m_targetLevel) {
                    m_level = m_targetLevel;
                    advance(m_ix + 1);
                }
            } else {
                m_level -= m_inc;
                if (m_level <= m_targetLevel) {
                    m_level = m_targetLevel;
                    advance(m_ix + 1);
                }
            }
        }
        return m_level;
    }

    void keydown(bool down) noexcept {
        if (m_down != down) {
            m_down = down;
            advance(down ? 0 : 3);
        }
    }

    /// adX: released and at its last level (stage 4 done).
    [[nodiscard]] bool finished() const noexcept {
        return !m_down && m_ix >= 4;
    }

    [[nodiscard]] static int scaleoutlevel(int outlevel) noexcept {
        static constexpr std::array<int, 20> kLevelLut = {0,  5,  9,  13, 17, 20, 23, 25, 27, 29,
                                                          31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
        return outlevel >= 20 ? 28 + outlevel : kLevelLut[static_cast<std::size_t>(outlevel)];
    }

private:
    void advance(int newix) noexcept {
        m_ix = newix;
        if (m_ix < 4) {
            const int newlevel = m_levels[static_cast<std::size_t>(m_ix)];
            int actuallevel = scaleoutlevel(newlevel) >> 1;
            actuallevel = (actuallevel << 6) + m_outlevel - 4256;
            actuallevel = actuallevel < 16 ? 16 : actuallevel;
            m_targetLevel = actuallevel << 16;
            m_rising = (m_targetLevel > m_level);
            int qrate = (m_rates[static_cast<std::size_t>(m_ix)] * 41) >> 6;
            qrate += m_rateScaling;
            qrate = qrate < 63 ? qrate : 63;
            m_inc = (4 + (qrate & 3)) << (2 + kLgN + (qrate >> 2));
        }
    }

    std::array<int, 4> m_rates{};
    std::array<int, 4> m_levels{};
    int m_outlevel{0};
    int m_rateScaling{0};
    std::int32_t m_level{0};
    int m_targetLevel{0};
    bool m_rising{false};
    int m_ix{0};
    int m_inc{0};
    bool m_down{false};
};

/// DX7 pitch envelope, Q24 per octave.
class PitchEnv {
public:
    void set(const RateTables& rate, const std::array<int, 4>& rates,
             const std::array<int, 4>& levels) noexcept {
        m_unit = rate.pitchEnvUnit;
        m_rates = rates;
        m_levels = levels;
        m_level = pitch(levels[3]) << 19;
        m_down = true;
        advance(0);
    }

    /// adX: new rates and levels on a sounding note, the level kept.
    void update(const std::array<int, 4>& rates, const std::array<int, 4>& levels) noexcept {
        m_rates = rates;
        m_levels = levels;
        if (m_ix < 4) {
            advance(m_ix);
        }
    }

    std::int32_t getsample() noexcept {
        if (m_ix < 3 || ((m_ix < 4) && !m_down)) {
            if (m_rising) {
                m_level += m_inc;
                if (m_level >= m_targetLevel) {
                    m_level = m_targetLevel;
                    advance(m_ix + 1);
                }
            } else {
                m_level -= m_inc;
                if (m_level <= m_targetLevel) {
                    m_level = m_targetLevel;
                    advance(m_ix + 1);
                }
            }
        }
        return m_level;
    }

    void keydown(bool down) noexcept {
        if (m_down != down) {
            m_down = down;
            advance(down ? 0 : 3);
        }
    }

private:
    [[nodiscard]] static int pitch(int level) noexcept;
    [[nodiscard]] static int rateOf(int rate) noexcept;

    void advance(int newix) noexcept {
        m_ix = newix;
        if (m_ix < 4) {
            const int newlevel = m_levels[static_cast<std::size_t>(m_ix)];
            m_targetLevel = pitch(newlevel) << 19;
            m_rising = (m_targetLevel > m_level);
            m_inc = rateOf(m_rates[static_cast<std::size_t>(m_ix)]) * m_unit;
        }
    }

    std::int32_t m_unit{0};
    std::array<int, 4> m_rates{};
    std::array<int, 4> m_levels{};
    std::int32_t m_level{0};
    int m_targetLevel{0};
    bool m_rising{false};
    int m_ix{0};
    int m_inc{0};
    bool m_down{false};
};

/// The DX7's LFO. Params are the voice's six LFO bytes: speed, delay, PMD, AMD, sync,
/// wave.
class Lfo {
public:
    void reset(const RateTables& rate, const std::uint8_t* params) noexcept;

    /// 0..1 in Q24.
    std::int32_t getsample() noexcept;
    /// 0..1 in Q24.
    std::int32_t getdelay() noexcept;
    void keydown() noexcept {
        if (m_sync) {
            m_phase = (1U << 31) - 1;
        }
        m_delayState = 0;
    }
    /// adX: back to upstream's initial state (zero-initialised members), so a render is
    /// the same however long the instrument sat idle before it.
    void resetPhase() noexcept {
        m_phase = 0;
        m_randState = 0;
        m_delayState = 0;
    }

private:
    std::uint32_t m_unit{0};
    std::uint32_t m_phase{0};
    std::uint32_t m_delta{0};
    std::uint8_t m_waveform{0};
    std::uint8_t m_randState{0};
    bool m_sync{false};
    std::uint32_t m_delayState{0};
    std::uint32_t m_delayInc{0};
    std::uint32_t m_delayInc2{0};
};

} // namespace adx::dx7::msfa

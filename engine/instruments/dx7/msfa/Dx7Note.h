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
// Ported from msfa's dx7note.h/.cc.
//
// Changes (adX, 2026), each marked "adX:" in Dx7Note.cpp. Upstream (2017) leaves these
// as TODOs; a DX7 voice uses them, so a faithful home for one needs them:
// - pitch bend range and step (upstream hard-codes 3 semitones);
// - amplitude modulation: the LFO's AMD through each operator's AMS, and the mod
//   wheel, foot, breath and aftertouch assigns (pitch, amp, EG bias);
// - operator on/off (VCED parameter 155);
// - a pitch offset per note in Q24 log units, for portamento, the channel's tuning
//   and slides; update() for parameter changes on a sounding note; finished().
// Every addition is a pure addition that is zero at its neutral setting (bend range 3,
// AMD 0 and no controller assigned, all operators on, no offset), where the output is
// upstream's bit for bit: dx7_engine_matches_msfa holds the port to that.
#pragma once

#include <array>
#include <cstdint>

#include "engine/instruments/dx7/msfa/Env.h"
#include "engine/instruments/dx7/msfa/FmCore.h"
#include "engine/instruments/dx7/msfa/Tables.h"

namespace adx::dx7::msfa {

/// The DX7 function parameters and controller state the note reads each block (adX).
struct Controllers {
    /// 14-bit pitch bend, 0x2000 centre (upstream's values_[kControllerPitch]).
    int pitchBend{0x2000};
    /// Semitones for full bend; 3 is upstream's fixed range.
    int bendRange{3};
    /// 0: smooth; otherwise the bend moves in steps of this many semitones.
    int bendStep{0};
    /// Controller values 0..127 and their DX7 range (0..99) and assign bits
    /// (1 pitch, 2 amp, 4 EG bias): mod wheel, foot, breath, aftertouch.
    std::array<int, 4> value{};
    std::array<int, 4> range{};
    std::array<int, 4> assign{};
};

class Dx7Note {
public:
    /// `patch` is the unpacked 156-byte voice (VCED plus the operator on/off byte).
    void init(const RateTables& rate, const std::uint8_t* patch, int midinote,
              int velocity) noexcept;

    /// adX: a parameter change on the sounding note; envelopes keep their level.
    void update(const std::uint8_t* patch) noexcept;
    /// adX: a legato note in mono mode: the pitch (and key scaling) of `midinote`,
    /// envelopes not retriggered.
    void retune(const std::uint8_t* patch, int midinote) noexcept {
        m_midinote = midinote;
        update(patch);
    }

    /// Adds this note's 64 samples to `buf`.
    void compute(std::int32_t* buf, std::int32_t lfoVal, std::int32_t lfoDelay,
                 const Controllers& ctrls) noexcept;

    void keyup() noexcept;

    /// adX: Q24 log2 pitch offset (one octave = 1 << 24), applied to every operator.
    void setPitchOffset(std::int32_t offset) noexcept {
        m_pitchOffset = offset;
    }
    /// adX: every carrier envelope has released to its end at a level too low to be
    /// heard (every operator's gain under msfa's skip threshold).
    [[nodiscard]] bool finished() const noexcept;

    [[nodiscard]] int midinote() const noexcept {
        return m_midinote;
    }

private:
    void setOperators(const std::uint8_t* patch, bool retrigger) noexcept;

    const RateTables* m_rate{nullptr};
    FmCore m_core;
    std::array<Env, 6> m_env{};
    std::array<FmOpParams, 6> m_params{};
    PitchEnv m_pitchenv;
    std::array<std::int32_t, 6> m_basepitch{};
    std::array<std::int32_t, 2> m_fbBuf{};
    std::int32_t m_fbShift{16};
    int m_algorithm{0};
    int m_pitchmoddepth{0};
    int m_pitchmodsens{0};
    // adX:
    int m_midinote{60};
    int m_velocity{100};
    int m_ampmoddepth{0};
    std::array<int, 6> m_ampmodsens{};
    std::uint8_t m_opEnable{0x3f};
    std::int32_t m_pitchOffset{0};
};

} // namespace adx::dx7::msfa

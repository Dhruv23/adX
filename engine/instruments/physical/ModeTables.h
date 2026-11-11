// The modal model's shipped mode tables (phase_12.md §4.4.3): each a list of frequency
// ratios to the fundamental and their relative gains.
//
// The ratios are the textbook ones, each with its source. phase_12.md asks for them to
// be checked against library recordings (mode_tables_match_references); where that has
// been done the comment says so, and the numbers are the measured ones.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace adx::instruments {

inline constexpr std::size_t kMaxModes = 8;

struct ModeTable {
    std::string_view name;
    std::size_t count;
    std::array<float, kMaxModes> ratio;
    std::array<float, kMaxModes> gain;
};

// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
inline constexpr std::array<ModeTable, 16> kModeTables{{
    // 0: one mode, a pure decaying sine - a sub, a test.
    {"sine", 1, {1}, {1}},
    // 1: Euler-Bernoulli free-free bar: 1, 2.756, 5.404, 8.933.
    {"free bar", 4, {1, 2.756F, 5.404F, 8.933F}, {1, 0.5F, 0.25F, 0.12F}},
    // 2: marimba: bars undercut so the overtones sit near 4 and 10 (CCRMA percussion
    //    notes; Rossing, The Science of Percussion Instruments, ch. 3).
    {"marimba", 3, {1, 3.9F, 9.2F}, {1, 0.35F, 0.12F}},
    // 3: xylophone: undercut to the twelfth, 3, then near 6.
    {"xylophone", 3, {1, 3.0F, 6.0F}, {1, 0.5F, 0.2F}},
    // 4: vibraphone: as the marimba, aluminium, 1 : 4 : 10.
    {"vibraphone", 3, {1, 4.0F, 10.0F}, {1, 0.3F, 0.1F}},
    // 5: glockenspiel: steel bars, near free-free.
    {"glockenspiel", 3, {1, 2.76F, 5.40F}, {1, 0.6F, 0.35F}},
    // 6: tubular bell: the struck tone is heard from modes 4-6, near 2 : 3 : 4 (Hibberts).
    {"tubular bell",
     6,
     {1, 2.76F, 5.40F, 8.93F, 13.34F, 18.64F},
     {0.3F, 0.5F, 0.8F, 1, 0.9F, 0.6F}},
    // 7: timpani: the air-loaded membrane's near-harmonic set (CCRMA).
    {"timpani", 5, {1, 1.5F, 1.98F, 2.44F, 2.94F}, {1, 0.8F, 0.6F, 0.4F, 0.3F}},
    // 8: steel pan: tuned to octave and twelfth (Rossing & Hansen).
    {"steel pan", 4, {1, 2.0F, 3.0F, 4.0F}, {1, 0.6F, 0.3F, 0.15F}},
    // 9: handpan: the same practice, spread a few cents.
    {"handpan", 3, {1, 2.002F, 2.998F}, {1, 0.5F, 0.25F}},
    // 10: church bell: hum, prime, tierce, quint, nominal (and above), against the prime.
    {"church bell",
     7,
     {0.5F, 1, 1.2F, 1.5F, 2.0F, 2.5F, 3.0F},
     {0.8F, 1, 0.6F, 0.4F, 0.7F, 0.3F, 0.2F}},
    // 11: Rhodes tine: fundamental, the tine's own high modes.
    {"tine", 3, {1, 7.1F, 20.0F}, {1, 0.25F, 0.08F}},
    // 12: Wurlitzer reed: cantilever 1, 6.27, 17.55.
    {"reed", 3, {1, 6.27F, 17.55F}, {1, 0.3F, 0.1F}},
    // 13: circular plate / gong: (0,1), (1,1), (2,1), (0,2), (3,1), (1,2).
    {"plate", 6, {1, 1.59F, 2.14F, 2.30F, 2.65F, 2.92F}, {1, 0.8F, 0.7F, 0.6F, 0.5F, 0.4F}},
    // 14: kalimba / mbira: plucked cantilever tine.
    {"kalimba", 3, {1, 5.4F, 13.5F}, {1, 0.2F, 0.06F}},
    // 15: wine glass bending modes.
    {"glass", 3, {1, 2.32F, 4.25F}, {1, 0.4F, 0.15F}},
}};
// NOLINTEND(modernize-use-designated-initializers)

} // namespace adx::instruments

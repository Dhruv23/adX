// adx-thread: main
//
// SF2 and DLS banks read into a Bank (phase_12.md §4.7.4).
//
//   SF2 2.01/2.04  phdr -> pbag -> pgen -> inst -> ibag -> igen -> shdr, global zones
//                  included; preset generators add to the instrument's, key and velocity
//                  ranges intersect (SoundFont 2.04 §9.4); 16-bit samples, 24 with sm24.
//   DLS 1 and 2    lins instruments, rgn/rgn2 ranges, wsmp, wlnk into the wvpl pool, and
//                  art1/art2 connection blocks for the articulation SF2 has as generators.
//
// adX does its own parsing rather than taking FluidSynth's LGPL code: both are small,
// documented RIFF schemas, and the zones are needed in adX's own types anyway. What is
// not applied - a modulator outside SF2's default set, an SF3 compressed sample, a DLS
// connection adX does not map - is a BankNote, never dropped silently.
#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

#include "engine/format/soundfont/BankModel.h"

namespace adx::format::soundfont {

/// False, with `error` saying why, when the bytes are not a bank this can read. A file
/// that parses with problems is true, with the problems in `out.notes`.
[[nodiscard]] bool parseSf2(std::span<const std::byte> file, Bank& out, std::string& error);
[[nodiscard]] bool parseDls(std::span<const std::byte> file, Bank& out, std::string& error);

/// Reads a file and picks the parser by its RIFF form type ('sfbk' or 'DLS ').
[[nodiscard]] bool loadBank(const std::filesystem::path& path, Bank& out, std::string& error);

/// A folder of SFZ programs as a bank: `NNN/PPP_Name.sfz`, NNN the bank (128 the kits)
/// and PPP the program (SfzBank.cpp). loadBank reads a folder through this.
[[nodiscard]] bool loadSfzFolder(const std::filesystem::path& folder, Bank& out,
                                 std::string& error);

/// The Windows GS Wavetable bank, `%SystemRoot%\System32\drivers\gm.dls`; empty when the
/// file is not there. Read in place: it is Microsoft's and is never copied.
[[nodiscard]] std::filesystem::path systemGmBank();

/// The import report: one line per note.
[[nodiscard]] std::string reportOf(const Bank& bank);

} // namespace adx::format::soundfont

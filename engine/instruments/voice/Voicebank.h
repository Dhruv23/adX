// adx-thread: main
//
// An UTAU voicebank: oto.ini + character.txt, parsed; aliases resolved (phase_4.md
// §4.13).
//
// A bank is a tree of sub-banks - Kasane Teto ships three, continuous (VCV), solo (CV)
// and extra - each a folder with its own oto.ini. A line of oto.ini is
//
//     <wav>=<alias>,<offset>,<consonant>,<cutoff>,<preutterance>,<overlap>
//
// in milliseconds, fractional. One WAV carries many aliases (Teto's `_あ.wav` has `あ`,
// `- あ` and `* あ`). A negative cutoff is measured from the offset (the segment's
// length is |cutoff|); a positive one from the end of the file. oto.ini, character.txt
// and readme.txt are Shift-JIS as often as not, and are detected and transcoded; the
// user's files are never rewritten. Lines that do not parse, or name a WAV that does
// not exist, are skipped with a warning - a bank loads partially rather than not at
// all (ADX4300).
//
// Alias resolution is driven by the previous note (resolveAlias): after a vowel, the
// VCV alias `<vowel> <kana>` (`a い`); after a rest, `- <kana>`; otherwise the plain
// alias - each falling back to the others, across the sub-banks in search order (VCV,
// then CV, then the rest). A romaji lyric (`ka`) is tried as kana too.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace adx::instruments {

struct OtoEntry {
    std::string alias;
    std::filesystem::path wav;
    double offset{0.0};
    double consonant{0.0};
    double cutoff{0.0};
    double preutterance{0.0};
    double overlap{0.0};
    std::size_t subBank{0};

    /// Where the segment ends, in ms from the start of the file, for a file
    /// `fileMs` long.
    [[nodiscard]] double endMs(double fileMs) const noexcept {
        return cutoff < 0.0 ? offset - cutoff : fileMs - cutoff;
    }
};

struct SubBank {
    std::string name;
    std::filesystem::path directory;
    std::size_t lines{0};
    /// How many of its aliases are VCV (`<vowel> <kana>`): what orders the search.
    std::size_t vcvAliases{0};
};

class Voicebank {
public:
    /// Loads the bank rooted at `root` - a folder holding oto.ini, or holding
    /// sub-bank folders that do. False when no oto.ini is found at all.
    [[nodiscard]] bool load(const std::filesystem::path& root, std::vector<std::string>& warnings);

    [[nodiscard]] const std::string& name() const noexcept {
        return m_name;
    }
    [[nodiscard]] const std::string& readme() const noexcept {
        return m_readme;
    }
    [[nodiscard]] const std::vector<SubBank>& subBanks() const noexcept {
        return m_subBanks;
    }
    [[nodiscard]] const std::vector<OtoEntry>& entries() const noexcept {
        return m_entries;
    }
    /// Hash of every oto.ini and character.txt, and every referenced WAV's size: what
    /// a render key carries for the bank, cheap enough to compute on load.
    [[nodiscard]] std::uint64_t contentHash() const noexcept {
        return m_hash;
    }

    /// The entry for `alias`, first in sub-bank search order; null when none.
    [[nodiscard]] const OtoEntry* find(std::string_view alias) const;

private:
    void loadSubBank(const std::filesystem::path& directory, std::vector<std::string>& warnings);

    std::string m_name;
    std::string m_readme;
    std::vector<SubBank> m_subBanks;
    std::vector<OtoEntry> m_entries;
    /// alias -> entry indices, in search order.
    std::unordered_map<std::string, std::vector<std::size_t>> m_index;
    std::uint64_t m_hash{0};
};

/// The vowel a lyric ends on - "a", "i", "u", "e", "o", "n" - or empty when it is not
/// a kana or romaji syllable this knows.
[[nodiscard]] std::string vowelOf(std::string_view lyric);

/// Hiragana for a romaji syllable ("ka" -> "か"), or empty.
[[nodiscard]] std::string romajiToKana(std::string_view lyric);

/// The entry a lyric sings after `previousLyric` (empty: after a rest).
[[nodiscard]] const OtoEntry* resolveAlias(const Voicebank& bank, std::string_view lyric,
                                           std::string_view previousLyric);

} // namespace adx::instruments

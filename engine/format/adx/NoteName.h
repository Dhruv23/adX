// Note names.
//
// The lexing discipline is inherited from iteration one and tightened: v1
// upper-cased the whole token and then treated a `B` in second position as a flat,
// which makes `Bb3` and `BB3` the same thing and `Cb4` a note the author almost
// certainly did not mean. v2 is case-insensitive on the letter and case-*sensitive*
// on the accidental: `#` is sharp, lowercase `b` is flat.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace adx::format {

/// MIDI note number of C4. Middle C is 60, which is the convention every DAW this
/// interoperates with uses, and disagreeing with it by an octave is the classic way
/// to make an import sound wrong in a way nobody can name.
inline constexpr std::uint8_t kMiddleC = 60;

/// "F#5" or "Bb3" or "C-1" to a MIDI number. A bare integer 0..127 is also accepted,
/// so a file may write pitches numerically.
///
/// Returns false and leaves `out` untouched when `text` is not a note name.
/// `badOffset` receives the character offset where parsing gave up, for the
/// diagnostic's underline.
[[nodiscard]] bool noteNameToMidi(std::string_view text, std::uint8_t& out,
                                  std::uint32_t& badOffset) noexcept;

/// The inverse, always spelled with sharps. Writing both spellings back would need
/// the key, and the model does not carry one.
[[nodiscard]] std::string midiToNoteName(std::uint8_t midi);

} // namespace adx::format

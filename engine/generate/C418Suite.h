// adx-thread: main
//
// The C418 suite (docs/C418.md): sparse, modal, wide, soft-attacked. A warm pad holds the
// chords, a kalimba breaks them into rising sixteenths through a ping-pong echo, a giant
// piano plays a sparse line on top, pizzicato marks the roots, and a long hall on a send
// is the space they share - "space as an instrument".
//
//   tempo     84-92 bpm from the seed, unless given
//   key       C, G or D major, or A or E minor, from the seed, unless given
//   form      intro (pad alone), theme (pad and arps), development (the piano enters,
//             the bass under it), outro (the arps at half speed, the piano thinning)
#pragma once

#include "engine/generate/GenreSuite.h"

namespace adx::generate {

[[nodiscard]] GeneratedTrack c418Track(const preset::PresetLibrary& presets,
                                       const StyleOptions& options, bool withNotes);

} // namespace adx::generate

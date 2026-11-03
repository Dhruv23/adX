// adx-thread: main
//
// The STAKILLAZ suite (docs/STAKILLAZ.md): trashwave, phonk and hardstyle - blown out,
// four on the floor, pumping. A hardstyle kick whose pitch follows the root, an 808 sub
// with its pitch drop on the off-beats, a rolling reverse bass between the kicks, a
// screeching minor lead, and the low end ducked from the kick's insert; the master is
// driven into the blown-out distortion and a little Memphis tape.
//
//   tempo     150 bpm (hardstyle), or 130 from the seed (phonk), unless given
//   key       F, G, A or C# minor from the seed, unless given
//   form      intro (lead over the sub), build (the kick on the half), drop (everything),
//             break (the lead alone), drop again
#pragma once

#include "engine/generate/GenreSuite.h"

namespace adx::generate {

[[nodiscard]] GeneratedTrack stakillazTrack(const preset::PresetLibrary& presets,
                                            const StyleOptions& options, bool withNotes);

} // namespace adx::generate

// adx-thread: main
#include "engine/generate/GenreSuite.h"

#include <stdexcept>
#include <string>

#include "engine/generate/C418Suite.h"
#include "engine/generate/StakillazSuite.h"

namespace adx::generate {

const std::vector<StyleInfo>& styles() {
    static const std::vector<StyleInfo> kStyles{
        {.id = "c418",
         .name = "C418",
         .description =
             "Sparse, modal, wide: a warm pad, kalimba arpeggios through a ping-pong echo, a "
             "giant piano line and a long hall (docs/C418.md).",
         .pack = "c418"},
        {.id = "stakillaz",
         .name = "STAKILLAZ",
         .description =
             "Trashwave, phonk and hardstyle: a pitched hardstyle kick, an 808 with its pitch "
             "drop, a rolling bass, a screech lead, everything pumping and blown out "
             "(docs/STAKILLAZ.md).",
         .pack = "stakillaz"},
    };
    return kStyles;
}

namespace {

GeneratedTrack build(std::string_view style, const preset::PresetLibrary& presets,
                     const StyleOptions& options, bool withNotes) {
    if (style == "c418") {
        return c418Track(presets, options, withNotes);
    }
    if (style == "stakillaz") {
        return stakillazTrack(presets, options, withNotes);
    }
    throw std::invalid_argument("no genre suite '" + std::string(style) + "'");
}

} // namespace

GeneratedTrack styleTemplate(std::string_view style, const preset::PresetLibrary& presets,
                             const StyleOptions& options) {
    return build(style, presets, options, false);
}

GeneratedTrack generateTrack(std::string_view style, const preset::PresetLibrary& presets,
                             const StyleOptions& options) {
    return build(style, presets, options, true);
}

} // namespace adx::generate

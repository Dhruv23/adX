// adx-thread: main
//
// Pad controllers (phase_11.md §4.7): declarative device profiles, compiled to the map
// the audio thread triggers clips from, and learn mode, which writes a profile from a
// press sequence for a controller nobody shipped one for.
//
// A profile is text in the project format's family:
//
//   [PAD_PROFILE]
//   NAME="Launchpad Mini MK3"
//   MATCH_DEVICE="LPMiniMK3 MIDI"      a substring of the input port's name
//   GRID=8x8                           columns (tracks) x rows (scenes)
//   NOTE_BASE=81 ROW_STRIDE=-10        the top-left pad, then the step down a row
//   COLUMN_STRIDE=1                    and across a column (default 1)
//   NOTES=...                          or every pad's note, row by row, for an irregular grid
//   CHANNEL=1                          1-16, or any (the default)
//   VELOCITY_SENSITIVE=yes  VELOCITY_CURVE=linear|soft|hard
//   CONSUME_ALL=yes                    nothing from the device plays an instrument
//   [FEEDBACK]
//   MODE=note|sysex_rgb|none  CHANNEL=1
//   SYSEX_PREFIX=F0 00 20 29 02 0D 03 03  SYSEX_SUFFIX=F7
//   STOPPED=#101010  QUEUED=#FFB000,blink  PLAYING=#00FF40,pulse  (sysex_rgb: colours)
//   STOPPED=vel:1    QUEUED=vel:5,blink                            (note: velocities)
//   QUEUED_STOP=...  EMPTY=...  RECORDING=...
//   [CONTROLS]
//   SCENE_LAUNCH=notes:89,79,69   or cc:...   one per row, top first
//   STOP_TRACK=notes:...          one per column
//   STOP_ALL=cc:19  TAP_TEMPO=cc:20          (note:N for a pad)
//
// A key the parser does not know warns and is kept, and written back where it was.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/midi/MidiLearn.h"
#include "engine/midi/PadMap.h"

namespace adx::midi {

enum class FeedbackMode : std::uint8_t { None, Note, SysexRgb };

/// How a cell is lit: a colour (sysex_rgb) or a velocity (note), steady or blinking.
struct PadColor {
    std::uint32_t rgb{0};
    std::uint8_t velocity{0};
    bool blink{false};
    bool pulse{false};

    [[nodiscard]] friend bool operator==(const PadColor&, const PadColor&) noexcept = default;
};

/// The colours, by what the cell shows.
enum class PadLook : std::uint8_t { Empty, Stopped, Queued, Playing, QueuedStop, Recording };
inline constexpr std::size_t kPadLooks = 6;

/// One control: pads (notes) or knobs (controller numbers), in order.
struct PadControl {
    bool cc{false};
    std::vector<std::uint8_t> numbers;

    [[nodiscard]] friend bool operator==(const PadControl&, const PadControl&) noexcept = default;
};

struct PadProfile {
    std::string name;
    std::string matchDevice;
    std::uint32_t columns{8};
    std::uint32_t rows{8};
    /// The grid by stride, or by `notes` (row by row) when that is non-empty.
    int noteBase{0};
    int rowStride{8};
    int columnStride{1};
    std::vector<std::uint8_t> notes;
    /// 1-16, or 0 for any channel.
    std::uint8_t channel{0};
    bool velocitySensitive{false};
    std::string velocityCurve{"linear"};
    bool consumeAll{true};

    FeedbackMode feedback{FeedbackMode::None};
    std::uint8_t feedbackChannel{1};
    /// Added to a pad's note for its light: the FM-1 lights key k with note k - 48
    /// (phase_11.5.md §4.10). Zero for controllers that light the note they send.
    int feedbackOffset{0};
    std::vector<std::uint8_t> sysexPrefix;
    std::vector<std::uint8_t> sysexSuffix;
    std::array<PadColor, kPadLooks> colors{};

    PadControl sceneLaunch;
    PadControl stopTrack;
    PadControl stopAll;
    PadControl tapTempo;

    /// Lines the parser did not know, "SECTION|KEY=VALUE", kept for the writer.
    std::vector<std::string> unknown;

    /// The note of the pad at (column, row), or -1 outside the grid.
    [[nodiscard]] int noteAt(std::uint32_t column, std::uint32_t row) const noexcept;

    [[nodiscard]] friend bool operator==(const PadProfile&, const PadProfile&) noexcept = default;
};

struct PadParse {
    PadProfile profile;
    std::vector<std::string> warnings;
    /// False when the text is not a profile at all (no [PAD_PROFILE], no grid).
    bool ok{false};
};

/// "note", "sysex_rgb" or "none".
[[nodiscard]] const char* toString(FeedbackMode mode) noexcept;

[[nodiscard]] PadParse parsePadProfile(std::string_view text);
[[nodiscard]] std::string writePadProfile(const PadProfile& profile);
/// The audio thread's map: grid pads launch cells (column = track, row = scene), and
/// the controls do theirs. A gain table for the velocity curve.
[[nodiscard]] PadMap compilePadMap(const PadProfile& profile);
/// Whether an input port of this name is the profile's device.
[[nodiscard]] bool profileMatches(const PadProfile& profile, std::string_view port);

/// The gain a press of `velocity` gives through `curve` (linear, soft, hard): 1 at full
/// velocity, never 0 for a press.
[[nodiscard]] float padGain(std::uint8_t velocity, std::string_view curve) noexcept;

/// Learn mode: press each pad of the grid in order (row by row, from the top left),
/// then each row's scene button, then stop-all and tap tempo; skip() passes over a
/// control the device does not have. Writes NOTE_BASE and strides when the presses fit
/// a regular grid, NOTES otherwise.
class PadLearn {
public:
    PadLearn(std::string name, std::uint32_t columns, std::uint32_t rows);

    /// What to press next, for the dialog: "pad 3, 1", "scene 2", "stop all", "tap tempo",
    /// or empty when done.
    [[nodiscard]] std::string prompt() const;
    /// A message from the device. True when it was taken as the next step (a press: a
    /// note-on or a controller above zero, not one already used).
    bool press(const ControlMessage& message);
    void skip();
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] PadProfile result() const;

private:
    enum class Stage : std::uint8_t { Grid, Scenes, StopAll, Tap, Done };
    void advance();

    PadProfile m_profile;
    Stage m_stage{Stage::Grid};
    std::uint32_t m_index{0};
    std::vector<std::uint8_t> m_grid;
    std::vector<std::uint8_t> m_scenes;
    bool m_scenesCc{false};
    bool m_sceneSkipped{false};
};

} // namespace adx::midi

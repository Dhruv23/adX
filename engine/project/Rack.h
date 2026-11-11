// adx-thread: main
//
// A rack: a small project fragment that becomes ordinary entities (phase_12.md §4.1).
//
// A preset is one instrument or one effect. A hardstyle kick is three layers into an
// eight-slot chain with a ducked reverb send and two macros, and that is a rack:
// layers, a chain, sends and macros. InsertRack expands one into channels, inserts,
// routes, sends and [MACRO] entries, in one command and so one undo step. Afterwards
// nothing in the project is a rack: every path is an ordinary path, and the `.adx`
// text has no rack indirection to read through.
//
// Rack-relative paths exist only in a rack's macro targets:
//
//   layer.<layer>.<param>            the layer's instrument parameter
//   layer.<layer>.volume|pan|pitch   the layer's channel strip
//   layer.<layer>.chain.<n>.<param>  slot n (from 1) of the layer's own chain
//   chain.<n>.<param>                slot n of the rack's chain (`mix` included)
//   send.<send>.level                the send's level
//   send.<send>.<n>.<param>          slot n of the send's return
//
// rackFrom is the inverse, for saving a sound built in the GUI as a rack.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/project/Channel.h"
#include "engine/project/Mixer.h"
#include "engine/project/Project.h"
#include "engine/project/SampleZone.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

/// A key the file had and this build does not know: kept, and written back.
using RackExtra = std::pair<std::string, std::string>;

struct RackSlot {
    std::string type;
    float mix{1.0F};
    bool bypass{false};
    std::vector<SlotParam> params;
    /// `key=rack`: keyed from the rack's own insert (a ducked return).
    bool keyRack{false};
    /// `ir=`: an impulse response file, as a zone names its sample.
    std::string impulse;

    [[nodiscard]] friend bool operator==(const RackSlot&, const RackSlot&) noexcept = default;
};

struct RackZone {
    SampleZone zone;
    std::string path;

    [[nodiscard]] friend bool operator==(const RackZone&, const RackZone&) noexcept = default;
};

struct RackLayer {
    std::string name;
    std::string type{"additive"};
    /// `PRESET=pack/name`: the layer takes that preset's type, parameters and zones,
    /// with this layer's own PARAM lines on top. Resolved before insertion
    /// (preset::resolveRackPresets); empty afterwards.
    std::string preset;
    NoteFilter filter;
    float gainDb{0.0F};
    float pan{0.0F};
    float pitchCents{0.0F};
    std::uint16_t polyphony{16};
    std::vector<ParamValue> params;
    std::vector<RackZone> zones;
    /// The layer's own inserts. Empty: the layer feeds the rack's chain directly.
    std::vector<RackSlot> chain;
    std::vector<RackExtra> extra;

    [[nodiscard]] friend bool operator==(const RackLayer&, const RackLayer&) noexcept = default;
};

struct RackSend {
    std::string name;
    float levelDb{-12.0F};
    std::vector<RackSlot> slots;
    std::vector<RackExtra> extra;

    [[nodiscard]] friend bool operator==(const RackSend&, const RackSend&) noexcept = default;
};

struct RackMacroTarget {
    std::string path;
    float minimum{0.0F};
    float maximum{1.0F};
    core::Curve curve;

    [[nodiscard]] friend bool operator==(const RackMacroTarget&,
                                         const RackMacroTarget&) noexcept = default;
};

struct RackMacro {
    std::string name;
    float value{0.5F};
    std::vector<RackMacroTarget> targets;

    [[nodiscard]] friend bool operator==(const RackMacro&, const RackMacro&) noexcept = default;
};

struct Rack {
    std::string name;
    std::string pack;
    std::string category;
    std::vector<std::string> tags;
    double tempoHint{0.0};
    std::string rootHint;
    int version{1};
    /// A synthesized preset (`pack/name`) to play when a library this rack's zones
    /// need is not installed (phase_12.md §4.7.3).
    std::string fallback;
    std::vector<RackExtra> extra;

    std::vector<RackLayer> layers;
    std::vector<RackSlot> chain;
    std::vector<RackSend> sends;
    std::vector<RackMacro> macros;

    [[nodiscard]] friend bool operator==(const Rack&, const Rack&) noexcept = default;
};

/// Expands `rack` into the project: one command, one undo entry.
class InsertRack final : public Command {
public:
    explicit InsertRack(Rack rack);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    /// The rack's own insert, and the name it was given (suffixed when taken).
    [[nodiscard]] core::InsertId insert() const noexcept {
        return m_insert;
    }
    [[nodiscard]] const std::string& instanceName() const noexcept {
        return m_instance;
    }
    /// One channel per layer, in the rack's order.
    [[nodiscard]] const std::vector<core::ChannelId>& channels() const noexcept {
        return m_channels;
    }
    /// Macro targets that named nothing in the rack: path text, for a diagnostic.
    [[nodiscard]] const std::vector<std::string>& unresolved() const noexcept {
        return m_unresolved;
    }

private:
    Rack m_rack;
    IdMarks m_marks;
    std::size_t m_channelCount{0};
    std::size_t m_insertCount{0};
    std::size_t m_routeCount{0};
    std::size_t m_sampleCount{0};
    std::size_t m_macroCount{0};
    core::InsertId m_insert;
    std::string m_instance;
    std::vector<core::ChannelId> m_channels;
    std::vector<std::string> m_unresolved;
};

/// The rack whose own insert is `insert`: its chain, the channels that feed it (through
/// their own insert or directly) as layers, its sends and their returns, and the macros
/// InsertRack named after it. Metadata a project does not hold (pack, category, tags)
/// is left empty. A project insert that no rack made still saves: its channels are the
/// layers.
[[nodiscard]] Rack rackFrom(const Project& project, core::InsertId insert);

} // namespace adx::project

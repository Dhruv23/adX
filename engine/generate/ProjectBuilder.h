// adx-thread: main
//
// Assembles a suite's project from pack presets (GenreSuite.h): channels on instrument
// presets, inserts with effect presets on their slots, sends, sidechains, patterns of
// generated notes, placements and markers. Bars are 4/4 at core::kPpq per beat.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/generate/RiffGenerator.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/project/Project.h"

namespace adx::generate {

inline constexpr std::int64_t kBar = core::kPpq * 4;

class ProjectBuilder {
public:
    ProjectBuilder(const preset::PresetLibrary& presets, std::string pack, double bpm,
                   std::string title);

    [[nodiscard]] core::InsertId master() const noexcept {
        return m_project.mixer.master;
    }
    /// A new insert routed to `to` (the master when invalid).
    core::InsertId insert(std::string name, core::InsertId to = {}, float gain = 1.0F);
    /// A channel playing `preset` from the pack (its type's defaults if not found).
    core::ChannelId channel(std::string name, std::string_view preset, core::InsertId output,
                            float volume = 1.0F, float pan = 0.0F, std::uint16_t polyphony = 16);
    /// An effect preset on a new slot of `insert`, keyed from `sidechain` when valid.
    core::SlotId effect(core::InsertId insert, std::string_view preset,
                        core::InsertId sidechain = {});
    void send(core::InsertId from, core::InsertId to, float level);
    /// A slot of `type` on its defaults: the master's limiter.
    core::SlotId effectType(core::InsertId insert, std::string type);

    /// A pattern `bars` long holding each channel's notes.
    core::PatternId
    pattern(std::string name, std::int64_t bars,
            const std::vector<std::pair<core::ChannelId, std::vector<GenNote>>>& parts);
    /// Places `pattern` on the playlist track named `track` (made on first use).
    void place(std::string_view track, core::PatternId pattern, std::int64_t startBar,
               std::int64_t bars = 0);
    void marker(std::int64_t bar, std::string name);

    [[nodiscard]] std::vector<std::string>& notes() noexcept {
        return m_notes;
    }
    [[nodiscard]] project::Project take() && {
        return std::move(m_project);
    }

private:
    [[nodiscard]] const preset::Preset* find(std::string_view name);

    const preset::PresetLibrary& m_presets;
    std::string m_pack;
    project::Project m_project;
    std::vector<std::string> m_notes;
};

} // namespace adx::generate

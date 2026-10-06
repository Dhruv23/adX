// adx-thread: main
//
// Building a Voice node: from a channel's notes to rendered clips (phase_4.md §4.13).
//
// For every note the channel plays - every pattern's note clip for it - this resolves
// the lyric's alias against the note before it (a rest, or a vowel to make a VCV
// transition from), works out the note's length in milliseconds at the tempo where its
// pattern is first placed, cuts it where the next note's overlap ends when the next
// note follows without a rest, and asks the render cache for it. The node gets a table
// from note id to clip; the clips fill in as the workers finish.
//
// Notes are keyed by id, and one pattern placed twice plays the same render twice:
// the previous note, and so the alias, is the previous note *in the pattern*, and the
// length is taken at the first placement's tempo (phase_4.md §11).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/instruments/voice/VoiceInstrument.h"
#include "engine/instruments/voice/Voicebank.h"

namespace adx::project {
class Project;
struct Channel;
struct NoteClip;
struct Resources;
} // namespace adx::project

namespace adx::instruments {

/// The bank at `path` (cached by path; loaded once). Null when it does not load.
[[nodiscard]] std::shared_ptr<const Voicebank>
loadVoicebank(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);

/// Resolves, renders (asynchronously) and binds every note `channel` plays in
/// `project` to `node`. Without a project or a bank the node plays nothing.
void configureVoice(VoiceInstrument& node, const project::Channel& channel,
                    const project::Project* project, const project::Resources* resources);

/// The alias each note of `clip` will sing, in the clip's note order: what the piano
/// roll's lyric lane shows dimmed under each lyric, so a wrong join is visible before
/// rendering (phase_5.md §4.7). Resolved exactly as configureVoice resolves it - against
/// the previous note in the pattern when it joins without a rest. Empty for a note the
/// bank cannot sing, and every entry empty when the channel has no bank.
[[nodiscard]] std::vector<std::string> resolvedAliases(const project::Project& project,
                                                       const project::Channel& channel,
                                                       const project::NoteClip& clip);

/// True when `node` was built from exactly what configureVoice would build now.
[[nodiscard]] bool voiceMatches(const VoiceInstrument& node, const project::Channel& channel,
                                const project::Project* project,
                                const project::Resources* resources);

} // namespace adx::instruments

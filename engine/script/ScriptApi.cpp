// adx-thread: main
#include "engine/script/ScriptApi.h"

#include <memory>
#include <stdexcept>

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Value.h"
#include "engine/format/mininotation/PatternCompiler.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"

namespace adx::script {

namespace {

std::string firstMessage(const format::DiagnosticList& diagnostics) {
    return diagnostics.empty() ? std::string("not understood") : diagnostics.all().front().message;
}

} // namespace

core::Ticks positionOf(const project::Project& project, std::string_view text) {
    format::DiagnosticList diagnostics;
    core::Ticks out;
    if (!format::parsePosition(text, project.tempo, format::Span{}, diagnostics, out) ||
        diagnostics.hasErrors()) {
        throw std::invalid_argument("'" + std::string(text) +
                                    "' is not a position: " + firstMessage(diagnostics));
    }
    return out;
}

core::Ticks durationOf(const project::Project& project, std::string_view text) {
    format::DiagnosticList diagnostics;
    core::Ticks out;
    if (!format::parseDuration(text, project.tempo, format::Span{}, diagnostics, out) ||
        diagnostics.hasErrors()) {
        throw std::invalid_argument("'" + std::string(text) +
                                    "' is not a duration: " + firstMessage(diagnostics));
    }
    return out;
}

std::vector<project::Note> miniNotes(std::string_view text, core::Ticks length, core::Ticks cycle) {
    std::vector<mini::Problem> problems;
    const std::vector<mini::RealizedNote> realized =
        mini::realize(text, core::Ticks{0}, length, cycle, &problems);
    for (const mini::Problem& problem : problems) {
        if (problem.code == format::code::kMiniSyntax) {
            throw std::invalid_argument("mini-notation, column " +
                                        std::to_string(problem.start + 1) + ": " + problem.message);
        }
    }
    std::vector<project::Note> notes;
    notes.reserve(realized.size());
    for (const mini::RealizedNote& r : realized) {
        project::Note note;
        note.start = r.note.start;
        // A note is cut where the pattern ends, as a placement cuts it.
        note.length = core::Ticks{std::min(r.note.length.value, length.value - r.note.start.value)};
        note.pitch = r.note.pitch;
        note.velocity = r.note.velocity;
        notes.push_back(note);
    }
    return notes;
}

std::size_t addMiniNotes(project::Project& project, project::CommandStack& stack,
                         core::PatternId pattern, core::ChannelId channel, std::string_view text) {
    const project::Pattern* target = project.find(pattern);
    if (target == nullptr || project.find(channel) == nullptr) {
        throw std::invalid_argument("no such pattern or channel");
    }
    std::vector<project::Note> notes =
        miniNotes(text, target->length, mini::cycleLength(project.tempo));
    const std::size_t count = notes.size();
    if (count > 0) {
        stack.execute(std::make_unique<project::AddNotes>(pattern, channel, std::move(notes)),
                      project);
    }
    return count;
}

void setMini(project::Project& project, project::CommandStack& stack, core::PatternId pattern,
             core::ChannelId channel, std::string_view text) {
    if (project.find(pattern) == nullptr || project.find(channel) == nullptr) {
        throw std::invalid_argument("no such pattern or channel");
    }
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string_view line =
            text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (!line.empty()) {
            lines.emplace_back(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    stack.execute(std::make_unique<project::SetMiniNotation>(pattern, channel, std::move(lines)),
                  project);
}

core::SlotId addEffect(project::Project& project, project::CommandStack& stack,
                       core::InsertId insert, std::string type,
                       const std::vector<std::pair<std::string, double>>& params, float mix) {
    if (project.mixer.find(insert) == nullptr) {
        throw std::invalid_argument("no insert " + std::to_string(insert.value));
    }
    project::Slot prototype;
    prototype.type = std::move(type);
    prototype.mix = mix;
    for (const auto& [name, value] : params) {
        prototype.params.push_back(project::SlotParam{.name = name, .value = value});
    }
    auto command = std::make_unique<project::AddSlot>(insert, std::move(prototype));
    const project::AddSlot* raw = command.get();
    stack.execute(std::move(command), project);
    return raw->created();
}

} // namespace adx::script

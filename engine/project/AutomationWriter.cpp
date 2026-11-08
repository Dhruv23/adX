// adx-thread: main
#include "engine/project/AutomationWriter.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "engine/core/Time.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"
#include "engine/project/commands/AutomationCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"

namespace adx::project {

namespace {

std::int64_t barFloor(const Project& project, std::int64_t tick) {
    std::int64_t from = 0;
    std::int64_t bar = core::kPpq * 4;
    for (const core::MeterEvent& meter : project.tempo.meterEvents()) {
        if (meter.at.value > tick) {
            break;
        }
        from = meter.at.value;
        bar = std::max<std::int64_t>(1, core::kPpq * 4 * meter.numerator /
                                            std::max(1, static_cast<int>(meter.denominator)));
    }
    return from + (((std::max<std::int64_t>(tick, from) - from) / bar) * bar);
}

} // namespace

void AutomationWriter::begin(std::int64_t tick) {
    m_writing = true;
    m_begin = tick;
    m_lanes.clear();
}

void AutomationWriter::add(ParamRef target, std::int64_t tick, float value) {
    if (!m_writing || tick < m_begin || !target.valid()) {
        return;
    }
    auto lane = std::ranges::find(m_lanes, target, &Lane::target);
    if (lane == m_lanes.end()) {
        m_lanes.push_back(Lane{.target = target, .points = {}});
        lane = m_lanes.end() - 1;
    }
    std::vector<Breakpoint>& points = lane->points;
    if (!points.empty() && points.back().at.value >= tick) {
        // Two changes in one UI frame: the later value at that tick.
        points.back().value = value;
        return;
    }
    if (points.size() >= 2 && points.back().value == value &&
        points[points.size() - 2].value == value) {
        points.back().at = core::Ticks{tick}; // a held value: one segment, not many points
        return;
    }
    points.push_back(Breakpoint{.at = core::Ticks{tick}, .value = value, .curve = {}});
}

void AutomationWriter::discard() noexcept {
    m_writing = false;
    m_lanes.clear();
}

std::string AutomationWriter::commit(Project& project, CommandStack& stack, std::int64_t tick) {
    m_writing = false;
    const std::vector<Lane> lanes = std::exchange(m_lanes, {});
    if (lanes.empty()) {
        return {};
    }
    const std::int64_t start = barFloor(project, m_begin);
    std::int64_t end = std::max(tick, m_begin);
    for (const Lane& lane : lanes) {
        end = std::max(end, lane.points.back().at.value);
    }
    Pattern prototype;
    for (std::size_t n = 1;; ++n) {
        prototype.name = "Automation " + std::to_string(n);
        if (project.findPatternByName(prototype.name) == nullptr) {
            break;
        }
    }
    const std::int64_t bar = core::kPpq * 4;
    const std::int64_t span = std::max<std::int64_t>(end - start, 1);
    prototype.length = core::Ticks{((span + bar - 1) / bar) * bar};

    stack.beginGroup("Record automation");
    auto pattern = std::make_unique<AddPattern>(prototype);
    const AddPattern* patternRaw = pattern.get();
    stack.execute(std::move(pattern), project);
    for (const Lane& lane : lanes) {
        AutomationClip clip;
        clip.target = lane.target;
        clip.targetPath = ParamRegistry::pathOf(lane.target, project);
        for (const Breakpoint& point : lane.points) {
            clip.points.push_back(Breakpoint{.at = core::Ticks{point.at.value - start},
                                             .value = point.value,
                                             .curve = point.curve});
        }
        stack.execute(std::make_unique<AddAutomationClip>(patternRaw->created(), std::move(clip)),
                      project);
    }
    PlaylistTrack row;
    row.name = prototype.name;
    auto addTrack = std::make_unique<AddPlaylistTrack>(row);
    const AddPlaylistTrack* trackRaw = addTrack.get();
    stack.execute(std::move(addTrack), project);
    PlaylistItem item;
    item.start = core::Ticks{start};
    item.content = PatternRef{.pattern = patternRaw->created()};
    stack.execute(std::make_unique<AddPlaylistItem>(trackRaw->created(), item), project);
    stack.endGroup();
    return prototype.name;
}

} // namespace adx::project

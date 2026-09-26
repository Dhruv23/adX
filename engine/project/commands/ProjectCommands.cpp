#include "engine/project/commands/ProjectCommands.h"

#include <algorithm>
#include <utility>

namespace adx::project {
namespace {

/// Finds the tempo or meter event at exactly `at`, so a "set" can remember what it
/// replaced. Both lists are sorted and small.
template<class Events> [[nodiscard]] const auto* eventAt(const Events& events, core::Ticks at) {
    const auto match = std::ranges::find(events, at, [](const auto& event) { return event.at; });
    return match == events.end() ? nullptr : &*match;
}

} // namespace

// --- SetMeta -----------------------------------------------------------------

SetMeta::SetMeta(ProjectMeta meta) : m_next(std::move(meta)) {}

void SetMeta::apply(Project& project) {
    m_previous = project.meta;
    project.meta = m_next;
}

void SetMeta::revert(Project& project) {
    project.meta = m_previous;
}

std::string_view SetMeta::name() const noexcept {
    return "Set project properties";
}

CommandId SetMeta::kind() const noexcept {
    return CommandId::kSetMeta;
}

DirtyMask SetMeta::dirty() const noexcept {
    return dirty::kMeta;
}

bool SetMeta::coalesceWith(const Command& next) {
    // Typing in a title field produces one command per keystroke; without this the
    // history panel fills with them.
    m_next = static_cast<const SetMeta&>(next).m_next;
    return true;
}

// --- SetTempoEvent -----------------------------------------------------------

SetTempoEvent::SetTempoEvent(core::Ticks at, double bpm, bool ramp)
    : m_at(at), m_bpm(bpm), m_ramp(ramp) {}

void SetTempoEvent::apply(Project& project) {
    if (const auto* existing = eventAt(project.tempo.tempoEvents(), m_at)) {
        m_hadPrevious = true;
        m_previous = *existing;
    } else {
        m_hadPrevious = false;
    }
    project.tempo.setTempo(m_at, m_bpm, m_ramp);
}

void SetTempoEvent::revert(Project& project) {
    if (m_hadPrevious) {
        project.tempo.setTempo(m_previous.at, m_previous.bpm, m_previous.ramp);
    } else {
        // removeTempo refuses to remove the event at tick 0, which is correct: the
        // map always has one. A SetTempoEvent at tick 0 on a fresh project therefore
        // reverts to the default 120, which is what m_previous holds in that case.
        if (!project.tempo.removeTempo(m_at)) {
            project.tempo.setTempo(m_at, 120.0, false);
        }
    }
}

std::string_view SetTempoEvent::name() const noexcept {
    return "Set tempo";
}

CommandId SetTempoEvent::kind() const noexcept {
    return CommandId::kSetTempoEvent;
}

DirtyMask SetTempoEvent::dirty() const noexcept {
    return dirty::kTempo;
}

std::uint64_t SetTempoEvent::targetKey() const noexcept {
    return static_cast<std::uint64_t>(m_at.value);
}

bool SetTempoEvent::coalesceWith(const Command& next) {
    const auto& other = static_cast<const SetTempoEvent&>(next);
    m_bpm = other.m_bpm;
    m_ramp = other.m_ramp;
    return true;
}

// --- RemoveTempoEvent --------------------------------------------------------

RemoveTempoEvent::RemoveTempoEvent(core::Ticks at) : m_at(at) {}

void RemoveTempoEvent::apply(Project& project) {
    if (const auto* existing = eventAt(project.tempo.tempoEvents(), m_at)) {
        m_previous = *existing;
        m_removed = project.tempo.removeTempo(m_at);
    } else {
        m_removed = false;
    }
}

void RemoveTempoEvent::revert(Project& project) {
    if (m_removed) {
        project.tempo.setTempo(m_previous.at, m_previous.bpm, m_previous.ramp);
    }
}

std::string_view RemoveTempoEvent::name() const noexcept {
    return "Remove tempo change";
}

CommandId RemoveTempoEvent::kind() const noexcept {
    return CommandId::kRemoveTempoEvent;
}

DirtyMask RemoveTempoEvent::dirty() const noexcept {
    return dirty::kTempo;
}

// --- SetMeterEvent -----------------------------------------------------------

SetMeterEvent::SetMeterEvent(core::Ticks at, std::uint16_t numerator, std::uint16_t denominator)
    : m_at(at), m_numerator(numerator), m_denominator(denominator) {}

void SetMeterEvent::apply(Project& project) {
    if (const auto* existing = eventAt(project.tempo.meterEvents(), m_at)) {
        m_hadPrevious = true;
        m_previous = *existing;
    } else {
        m_hadPrevious = false;
    }
    project.tempo.setMeter(m_at, m_numerator, m_denominator);
}

void SetMeterEvent::revert(Project& project) {
    if (m_hadPrevious) {
        project.tempo.setMeter(m_previous.at, m_previous.numerator, m_previous.denominator);
    } else if (!project.tempo.removeMeter(m_at)) {
        project.tempo.setMeter(m_at, 4, 4);
    }
}

std::string_view SetMeterEvent::name() const noexcept {
    return "Set time signature";
}

CommandId SetMeterEvent::kind() const noexcept {
    return CommandId::kSetMeterEvent;
}

DirtyMask SetMeterEvent::dirty() const noexcept {
    return dirty::kTempo;
}

std::uint64_t SetMeterEvent::targetKey() const noexcept {
    return static_cast<std::uint64_t>(m_at.value);
}

// --- RemoveMeterEvent --------------------------------------------------------

RemoveMeterEvent::RemoveMeterEvent(core::Ticks at) : m_at(at) {}

void RemoveMeterEvent::apply(Project& project) {
    if (const auto* existing = eventAt(project.tempo.meterEvents(), m_at)) {
        m_previous = *existing;
        m_removed = project.tempo.removeMeter(m_at);
    } else {
        m_removed = false;
    }
}

void RemoveMeterEvent::revert(Project& project) {
    if (m_removed) {
        project.tempo.setMeter(m_previous.at, m_previous.numerator, m_previous.denominator);
    }
}

std::string_view RemoveMeterEvent::name() const noexcept {
    return "Remove time signature change";
}

CommandId RemoveMeterEvent::kind() const noexcept {
    return CommandId::kRemoveMeterEvent;
}

DirtyMask RemoveMeterEvent::dirty() const noexcept {
    return dirty::kTempo;
}

// --- AddMarker ---------------------------------------------------------------

AddMarker::AddMarker(core::Ticks at, std::string markerName)
    : m_at(at), m_name(std::move(markerName)) {}

void AddMarker::apply(Project& project) {
    m_marks = project.idMarks();
    m_id = project.newMarkerId();
    project.markers.push_back(Marker{.id = m_id, .at = m_at, .name = m_name});
}

void AddMarker::revert(Project& project) {
    std::erase_if(project.markers, [this](const Marker& marker) { return marker.id == m_id; });
    project.restoreIdMarks(m_marks);
    m_id = core::MarkerId{};
}

std::string_view AddMarker::name() const noexcept {
    return "Add marker";
}

CommandId AddMarker::kind() const noexcept {
    return CommandId::kAddMarker;
}

DirtyMask AddMarker::dirty() const noexcept {
    return dirty::kMarkers;
}

// --- RemoveMarker ------------------------------------------------------------

RemoveMarker::RemoveMarker(core::MarkerId id) : m_id(id) {}

void RemoveMarker::apply(Project& project) {
    const auto match = std::ranges::find(project.markers, m_id, &Marker::id);
    m_removed = match != project.markers.end();
    if (!m_removed) {
        return;
    }
    // The index matters: restoring at the end would reorder the vector, and while
    // the writer sorts markers by position anyway, the undo gate compares the whole
    // project and would see the difference.
    m_index = static_cast<std::size_t>(std::distance(project.markers.begin(), match));
    m_previous = *match;
    project.markers.erase(match);
}

void RemoveMarker::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, project.markers.size()));
    project.markers.insert(project.markers.begin() + at, m_previous);
}

std::string_view RemoveMarker::name() const noexcept {
    return "Remove marker";
}

CommandId RemoveMarker::kind() const noexcept {
    return CommandId::kRemoveMarker;
}

DirtyMask RemoveMarker::dirty() const noexcept {
    return dirty::kMarkers;
}

// --- MoveMarker --------------------------------------------------------------

MoveMarker::MoveMarker(core::MarkerId id, core::Ticks at, std::string markerName)
    : m_id(id), m_at(at), m_name(std::move(markerName)) {}

void MoveMarker::apply(Project& project) {
    Marker* marker = project.find(m_id);
    if (marker == nullptr) {
        return;
    }
    m_previousAt = marker->at;
    m_previousName = marker->name;
    marker->at = m_at;
    marker->name = m_name;
}

void MoveMarker::revert(Project& project) {
    Marker* marker = project.find(m_id);
    if (marker == nullptr) {
        return;
    }
    marker->at = m_previousAt;
    marker->name = m_previousName;
}

std::string_view MoveMarker::name() const noexcept {
    return "Move marker";
}

CommandId MoveMarker::kind() const noexcept {
    return CommandId::kMoveMarker;
}

DirtyMask MoveMarker::dirty() const noexcept {
    return dirty::kMarkers;
}

std::uint64_t MoveMarker::targetKey() const noexcept {
    return m_id.value;
}

bool MoveMarker::coalesceWith(const Command& next) {
    const auto& other = static_cast<const MoveMarker&>(next);
    m_at = other.m_at;
    m_name = other.m_name;
    return true;
}

// --- AddSample ---------------------------------------------------------------

AddSample::AddSample(std::string path) : m_path(std::move(path)) {}

void AddSample::apply(Project& project) {
    if (const SampleRef* existing = project.resources.findByPath(m_path)) {
        m_id = existing->id;
        m_inserted = false;
        return;
    }
    m_marks = project.idMarks();
    m_id = project.newSampleId();
    m_inserted = true;
    project.resources.samples.push_back(SampleRef{.id = m_id, .path = m_path});
}

void AddSample::revert(Project& project) {
    if (!m_inserted) {
        return;
    }
    std::erase_if(project.resources.samples,
                  [this](const SampleRef& sample) { return sample.id == m_id; });
    project.restoreIdMarks(m_marks);
    m_id = core::SampleId{};
}

std::string_view AddSample::name() const noexcept {
    return "Add sample";
}

CommandId AddSample::kind() const noexcept {
    return CommandId::kAddSample;
}

DirtyMask AddSample::dirty() const noexcept {
    return dirty::kResources;
}

} // namespace adx::project

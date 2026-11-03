// adx-thread: main
#include "engine/record/RecordSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <variant>

#include "engine/project/Project.h"
#include "engine/project/commands/ArrangeCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"
#include "engine/record/DiskWriter.h"
#include "engine/record/InputCapture.h"
#include "engine/render/RenderEngine.h"

namespace adx::record {

namespace {

/// The audio thread reads the capture pointer once per block, so once it is cleared
/// the capture is safe to destroy after the next block has begun. A stream that is not
/// running has no audio thread to wait for - an offline render runs on its caller.
void uninstall(render::RenderEngine& engine) {
    engine.core().setCapture(nullptr);
    const audio::StreamInfo info = engine.audio().info();
    if (!info.isRunning) {
        return;
    }
    const std::uint64_t before = info.callbackCount;
    for (int i = 0; i < 500 && engine.audio().info().callbackCount < before + 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::int64_t itemEnd(const project::Project& project, const project::PlaylistItem& item,
                     std::uint32_t rate, std::uint64_t frames) {
    if (item.length.value > 0) {
        return item.start.value + item.length.value;
    }
    const std::int64_t start = project.tempo.toSamples(item.start, rate).value;
    return project.tempo.toTicks(core::Samples{start + static_cast<std::int64_t>(frames)}, rate)
        .value;
}

} // namespace

RecordSession::RecordSession(render::RenderEngine& engine, project::Project& project,
                             project::CommandStack& stack, RecordSettings settings)
    : m_engine(engine), m_project(project), m_stack(stack), m_settings(std::move(settings)),
      m_capture(std::make_unique<InputCapture>()) {}

RecordSession::~RecordSession() {
    if (m_running) {
        // A take abandoned mid-recording is placed if it can be; a destructor that
        // threw would end the program instead.
        try {
            static_cast<void>(stop());
        } catch (...) { // NOLINT(bugprone-empty-catch) - nothing left to tell
        }
    }
}

core::Ticks RecordSession::recordAt() const noexcept {
    return m_settings.punchIn.value_or(m_settings.from);
}

bool RecordSession::start(std::string& error) {
    if (m_project.playlist.find(m_settings.track) == nullptr) {
        error = "no playlist track to record on";
        return false;
    }
    const std::uint32_t rate = m_engine.options().sampleRate;
    m_latency =
        latencyOf(m_engine.audio().info(), m_engine.latencySamples(), m_settings.measuredRoundTrip);
    const core::Ticks at = recordAt();
    WriterWindow window{.from = m_project.tempo.toSamples(at, rate).value,
                        .latency = m_latency.compensation()};
    if (m_settings.punchOut) {
        if (*m_settings.punchOut <= at) {
            error = "the punch-out is not after the punch-in";
            return false;
        }
        window.to = m_project.tempo.toSamples(*m_settings.punchOut, rate).value;
    }

    // "<name> 1.wav", "<name> 2.wav"... the first that is free: a take is never written
    // over another.
    const std::filesystem::path folder = m_settings.projectDirectory / "recordings";
    std::filesystem::path file;
    for (int n = 1;; ++n) {
        const std::string name = m_settings.name + " " + std::to_string(n) + ".wav";
        file = folder / name;
        if (!std::filesystem::exists(file)) {
            m_relative = std::filesystem::path("recordings") / name;
            break;
        }
    }
    m_writer = std::make_unique<DiskWriter>(*m_capture, file, rate, window);
    if (!m_writer->start(error)) {
        return false;
    }
    m_engine.core().setCapture(m_capture.get());
    m_capture->arm(true);
    if (m_settings.monitor == MonitorMode::Software) {
        static_cast<void>(m_engine.setMonitor(m_project, m_settings.track));
    }
    if (m_settings.countIn.click) {
        const core::MeterEvent meter = m_project.tempo.meterAt(at);
        m_engine.setMetronome(true, core::Ticks{core::ticksPerBeat(meter.denominator)},
                              meter.numerator);
    }
    m_engine.setLoop(transport::LoopRegion{});
    m_engine.seek(preRollStart(m_project.tempo, at, m_settings.countIn));
    m_engine.play();
    m_running = true;
    return true;
}

TakeResult RecordSession::stop() {
    TakeResult result;
    if (!m_running) {
        result.error = "not recording";
        return result;
    }
    m_running = false;
    m_engine.stopPlayback();
    m_capture->arm(false);
    uninstall(m_engine);
    if (!m_writer->stop(result.error)) {
        return result;
    }
    if (m_settings.monitor == MonitorMode::Software) {
        static_cast<void>(m_engine.setMonitor(m_project, core::PlaylistTrackId{}));
    }
    if (m_settings.countIn.click) {
        m_engine.setMetronome(false, core::Ticks{core::kPpq}, 4);
    }
    result.file = m_writer->path();
    result.frames = m_writer->framesWritten();
    result.overran = m_capture->overran();
    result.latency = m_latency.compensation();
    if (result.frames == 0) {
        std::error_code ignored;
        std::filesystem::remove(result.file, ignored);
        result.error = "nothing was recorded";
        return result;
    }

    const std::uint32_t rate = m_engine.options().sampleRate;
    project::PlaylistItem item;
    item.start = recordAt();
    if (m_settings.punchOut) {
        item.length = *m_settings.punchOut - item.start;
    }
    const std::int64_t end = itemEnd(m_project, item, rate, result.frames);
    // On the lane after every take it overlaps, which go quiet: the newest plays.
    const project::PlaylistTrack* track = m_project.playlist.find(m_settings.track);
    std::vector<core::ItemId> covered;
    std::uint16_t lane = 1;
    for (const project::PlaylistItem& other : track->items) {
        if (!std::holds_alternative<project::AudioClipRef>(other.content) || other.lane == 0) {
            continue;
        }
        const std::int64_t otherEnd = itemEnd(m_project, other, rate, 0);
        if (other.start.value < end &&
            (otherEnd <= other.start.value || item.start.value < otherEnd)) {
            lane = std::max<std::uint16_t>(lane, static_cast<std::uint16_t>(other.lane + 1));
            if (!other.muted) {
                covered.push_back(other.id);
            }
        }
    }
    item.lane = lane;
    result.lane = lane;

    m_stack.beginGroup("Record " + m_relative.stem().string());
    auto sample = std::make_unique<project::AddSample>(m_relative.generic_string());
    const project::AddSample* raw = sample.get();
    m_stack.execute(std::move(sample), m_project);
    item.content = project::audioClipOf(raw->created());
    auto add = std::make_unique<project::AddPlaylistItem>(m_settings.track, item);
    const project::AddPlaylistItem* added = add.get();
    m_stack.execute(std::move(add), m_project);
    for (const core::ItemId id : covered) {
        const project::PlaylistTrack* now = m_project.playlist.find(m_settings.track);
        const auto found = std::ranges::find(now->items, id, &project::PlaylistItem::id);
        project::PlaylistItem quiet = *found;
        quiet.muted = true;
        m_stack.execute(std::make_unique<project::SetPlaylistItem>(m_settings.track, id, quiet,
                                                                   m_settings.track),
                        m_project);
    }
    m_stack.endGroup();
    result.item = added->created();
    result.ok = true;
    return result;
}

void comp(project::Project& project, project::CommandStack& stack, core::PlaylistTrackId trackId,
          std::span<const CompRange> ranges) {
    const project::PlaylistTrack* track = project.playlist.find(trackId);
    if (track == nullptr) {
        return;
    }
    std::vector<project::PlaylistItem> pieces;
    std::vector<core::ItemId> takes;
    for (const CompRange& range : ranges) {
        const auto take = std::ranges::find(track->items, range.take, &project::PlaylistItem::id);
        if (take == track->items.end() ||
            !std::holds_alternative<project::AudioClipRef>(take->content) ||
            range.to <= range.from) {
            continue;
        }
        // The take's own audio, windowed to the range: slipped by how far into the take
        // the range begins.
        project::PlaylistItem piece = *take;
        // A fresh id: AddPlaylistItem keeps a prototype's, and two items sharing one would
        // be undone together.
        piece.id = core::ItemId{};
        piece.start = range.from;
        piece.length = range.to - range.from;
        piece.sourceOffset = take->sourceOffset + (range.from - take->start);
        piece.muted = false;
        piece.lane = 0;
        piece.envelopes.clear();
        pieces.push_back(std::move(piece));
        if (std::ranges::find(takes, take->id) == takes.end()) {
            takes.push_back(take->id);
        }
    }
    stack.beginGroup("Comp takes");
    for (project::PlaylistItem& piece : pieces) {
        stack.execute(std::make_unique<project::AddPlaylistItem>(trackId, std::move(piece)),
                      project);
    }
    for (const core::ItemId id : takes) {
        const project::PlaylistTrack* now = project.playlist.find(trackId);
        const auto found = std::ranges::find(now->items, id, &project::PlaylistItem::id);
        if (found != now->items.end() && !found->muted) {
            project::PlaylistItem quiet = *found;
            quiet.muted = true;
            stack.execute(std::make_unique<project::SetPlaylistItem>(trackId, id, quiet, trackId),
                          project);
        }
    }
    stack.endGroup();
}

Calibration::Calibration(render::RenderEngine& engine)
    : m_engine(engine), m_capture(std::make_unique<InputCapture>()) {}

Calibration::~Calibration() {
    uninstall(m_engine);
}

void Calibration::begin(const project::Project& /*project*/, std::int64_t pingAt) {
    m_pingAt = pingAt;
    m_engine.core().setCapture(m_capture.get());
    m_capture->arm(true);
    m_engine.setLoop(transport::LoopRegion{});
    m_engine.seek(core::Ticks{0});
    m_engine.play();
    m_engine.ping(pingAt, 0.5F);
}

std::optional<std::int64_t> Calibration::finish() {
    m_engine.stopPlayback();
    m_capture->arm(false);
    uninstall(m_engine);
    std::vector<float> heard(kCaptureRingSamples);
    heard.resize(m_capture->drain(heard.data(), heard.size()));
    for (std::size_t f = 0; (f * 2) + 1 < heard.size(); ++f) {
        if (std::abs(heard[f * 2]) > 0.25F || std::abs(heard[(f * 2) + 1]) > 0.25F) {
            return m_capture->firstPosition() + static_cast<std::int64_t>(f) - m_pingAt;
        }
    }
    return std::nullopt;
}

} // namespace adx::record

// adx-thread: main
//
// Recording, end to end (phase_8.md §3.1): arm a playlist track, count in, capture,
// write, and put the take where it was heard.
//
//   start()     seeks to the pre-roll, turns the click on for a count-in, installs an
//               InputCapture in the engine and a DiskWriter behind it, and plays
//   stop()      stops, uninstalls, finishes the file, and adds the take to the track
//               as an ordinary audio item - one undo step
//
// The take's file starts exactly at the record point: the punch-in, or where recording
// was asked to begin, which is a tick. Frames before it (the pre-roll) and after the
// punch-out (post-roll) are not written - the transport plays them so the performer
// has a run-up, and the file holds only the take (record_punch_region).
//
// Takes stack: a take that overlaps earlier ones on its track goes on the next lane,
// and the earlier ones are muted, so the newest plays. comp() then builds lane 0 from
// ranges of any of them; each piece is an ordinary audio clip on the take's file, so
// nothing downstream has to know a comp is a comp.
//
// The engine is driven by whoever owns it - the app's timer, or a test rendering an
// offline backend between start() and stop().
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/record/CountIn.h"
#include "engine/record/Monitoring.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project
namespace adx::render {
class RenderEngine;
}

namespace adx::record {

class InputCapture;
class DiskWriter;

struct RecordSettings {
    core::PlaylistTrackId track;
    /// Where the take begins; ignored when there is a punch-in.
    core::Ticks from;
    std::optional<core::Ticks> punchIn;
    std::optional<core::Ticks> punchOut;
    CountIn countIn{.bars = 0, .click = false};
    MonitorMode monitor{MonitorMode::Direct};
    /// The project's folder; takes are written under its `recordings/`.
    std::filesystem::path projectDirectory;
    std::string name{"Take"};
    /// A loopback calibration for this device, overriding what the driver reports.
    std::optional<std::int64_t> measuredRoundTrip;
};

struct TakeResult {
    bool ok{false};
    std::string error;
    core::ItemId item;
    std::filesystem::path file;
    std::uint64_t frames{0};
    /// The writer fell behind and the take ended early (InputCapture.h); the take is
    /// kept as far as it got and flagged here.
    bool overran{false};
    std::int64_t latency{0};
    std::uint16_t lane{0};
};

class RecordSession {
public:
    RecordSession(render::RenderEngine& engine, project::Project& project,
                  project::CommandStack& stack, RecordSettings settings);
    ~RecordSession();
    RecordSession(const RecordSession&) = delete;
    RecordSession& operator=(const RecordSession&) = delete;
    RecordSession(RecordSession&&) = delete;
    RecordSession& operator=(RecordSession&&) = delete;

    bool start(std::string& error);
    TakeResult stop();

    [[nodiscard]] InputCapture& capture() noexcept {
        return *m_capture;
    }
    [[nodiscard]] DiskWriter* writer() noexcept {
        return m_writer.get();
    }
    [[nodiscard]] const LatencyReport& latency() const noexcept {
        return m_latency;
    }
    /// The record point: where the file's first frame plays.
    [[nodiscard]] core::Ticks recordAt() const noexcept;

private:
    render::RenderEngine& m_engine;
    project::Project& m_project;
    project::CommandStack& m_stack;
    RecordSettings m_settings;
    std::unique_ptr<InputCapture> m_capture;
    std::unique_ptr<DiskWriter> m_writer;
    LatencyReport m_latency;
    std::filesystem::path m_relative;
    bool m_running{false};
};

/// One range of a comp: [from, to) of the arrangement, taken from `take`.
struct CompRange {
    core::Ticks from;
    core::Ticks to;
    core::ItemId take;
};

/// Builds lane 0 of `track` from `ranges`, mutes the takes, as one undo step. Ranges
/// that name no audio item on the track are skipped.
void comp(project::Project& project, project::CommandStack& stack, core::PlaylistTrackId track,
          std::span<const CompRange> ranges);

/// Loopback calibration (phase_8.md §9): with the interface's output wired to its
/// input, play one impulse and listen for it. begin() starts the transport at 0 with a
/// capture installed and an impulse queued at `pingAt`; the caller lets the engine run
/// for a second or so; finish() stops and returns the round trip in samples, or nullopt
/// when nothing came back.
class Calibration {
public:
    explicit Calibration(render::RenderEngine& engine);
    ~Calibration();
    Calibration(const Calibration&) = delete;
    Calibration& operator=(const Calibration&) = delete;
    Calibration(Calibration&&) = delete;
    Calibration& operator=(Calibration&&) = delete;

    void begin(const project::Project& project, std::int64_t pingAt = 4800);
    [[nodiscard]] std::optional<std::int64_t> finish();

private:
    render::RenderEngine& m_engine;
    std::unique_ptr<InputCapture> m_capture;
    std::int64_t m_pingAt{0};
};

} // namespace adx::record

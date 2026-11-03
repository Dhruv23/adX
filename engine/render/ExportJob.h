// adx-thread: main
//
// One export request, run off the UI thread (phase_8.md §3.5): the whole matrix -
// format, source (master, stems), range, tail, dither, normalize, sample rate - behind
// one struct and one function, which the export dialog, `adx render` and the tests all
// call. The render is OfflineSession, the realtime engine's own path (FINAL_PLAN §3.1).
//
// The order of operations is fixed, because each step's correctness depends on the
// ones before it:
//
//   render -> resample -> normalize -> dither -> encode
//
// Normalization measures what will be written, at the rate it will be written at.
// Dither goes last, after anything that changes a sample, and only for a format that
// stores fewer bits than a float. Stems take the master's normalization gain rather
// than their own, so they still sum to the normalized master.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/Time.h"
#include "engine/render/Dither.h"
#include "engine/render/Encoders.h"
#include "engine/render/OfflineRender.h"
#include "engine/render/StemRender.h"

namespace adx::project {
class Project;
}

namespace adx::render {

enum class RangeKind : std::uint8_t {
    /// From the start to the end of the last placement or clip.
    Project,
    /// The project's loop region.
    Loop,
    /// From the marker named `fromMarker` to the one named `toMarker` (or the end).
    Markers,
    /// [from, to), explicit.
    Selection,
};

enum class NormalizeKind : std::uint8_t {
    Off,
    /// Sample peak to `normalizeTarget` dBFS.
    Peak,
    /// Integrated loudness (BS.1770) to `normalizeTarget` LUFS.
    Lufs,
};

[[nodiscard]] const char* toString(RangeKind kind) noexcept;
[[nodiscard]] const char* toString(NormalizeKind kind) noexcept;

struct ExportRequest {
    /// The master's file. Stems and split pieces are named beside it:
    /// `<stem> - <name><ext>`.
    std::filesystem::path output;
    /// format, bitrate, quality; `sampleRate` 0 means the render rate.
    EncodeSettings encode;
    /// The engine's rate and block size. The render is bit-identical per block size
    /// (phase_3.md §4.10), so reproducing one needs both.
    std::uint32_t renderRate{48000};
    std::uint32_t blockFrames{256};

    RangeKind range{RangeKind::Project};
    core::Ticks from;
    core::Ticks to;
    std::string fromMarker;
    std::string toMarker;
    /// One file per stretch between the markers inside the range.
    bool splitAtMarkers{false};

    /// Write the master. False with stems set writes stems only.
    bool master{true};
    std::optional<StemMode> stems;

    double tailSeconds{0.0};
    /// Keep rendering after the range until the output stays below `silenceDb` for
    /// `silenceHoldSeconds`, up to `maxTailSeconds`; then end at the last sound.
    bool tailUntilSilence{false};
    float silenceDb{-80.0F};
    double silenceHoldSeconds{0.5};
    double maxTailSeconds{30.0};

    DitherKind dither{DitherKind::Tpdf};
    NormalizeKind normalize{NormalizeKind::Off};
    float normalizeTarget{-1.0F};

    /// Also write the arrangement as `<output stem>.mid` (MidiExporter.h). Collecting a
    /// bundle is the caller's - it works on the saved file, which this never sees
    /// (Bundle.h).
    bool writeMidi{false};
};

struct ExportedFile {
    std::filesystem::path path;
    /// "master", a stem's name, or a split piece's.
    std::string label;
    std::uint64_t frames{0};
    std::uint32_t sampleRate{0};
    /// Of what was encoded: after resampling, normalization and dither.
    RenderStats stats;
};

struct ExportResult {
    bool ok{false};
    bool cancelled{false};
    std::string error;
    std::vector<ExportedFile> files;
    /// The normalization gain applied, in dB (0 when off).
    double gainDb{0.0};
    /// The measured integrated loudness of the master before normalization.
    double loudness{0.0};
};

/// Progress and cancel, shared with whoever started the job.
struct ExportControl {
    std::atomic<float> progress{0.0F};
    std::atomic<bool> cancel{false};
};

/// Runs the export on the calling thread.
[[nodiscard]] ExportResult runExport(const project::Project& project, const ExportRequest& request,
                                     ExportControl* control = nullptr);

/// The arrangement range `request` names, in ticks, for `project`; false with `error`
/// when it names nothing (no loop, an unknown marker).
[[nodiscard]] bool resolveRange(const project::Project& project, const ExportRequest& request,
                                core::Ticks& from, core::Ticks& to, std::string& error);

/// The same, on a worker thread of its own. Holds its own copy of the project, so the
/// UI can go on editing while it runs.
class ExportJob {
public:
    ExportJob(const project::Project& project, ExportRequest request);
    ~ExportJob();
    ExportJob(const ExportJob&) = delete;
    ExportJob& operator=(const ExportJob&) = delete;
    ExportJob(ExportJob&&) = delete;
    ExportJob& operator=(ExportJob&&) = delete;

    [[nodiscard]] float progress() const noexcept {
        return m_control.progress.load(std::memory_order_relaxed);
    }
    void cancel() noexcept {
        m_control.cancel.store(true, std::memory_order_relaxed);
    }
    [[nodiscard]] bool done() const noexcept {
        return m_done.load(std::memory_order_acquire);
    }
    /// Blocks until the job has finished and returns its result.
    const ExportResult& wait();

private:
    std::unique_ptr<project::Project> m_project;
    ExportRequest m_request;
    ExportControl m_control;
    ExportResult m_result;
    std::atomic<bool> m_done{false};
    std::thread m_thread;
};

} // namespace adx::render

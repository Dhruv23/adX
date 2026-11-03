// adx-thread: main
//
// Analysis runs as a job, never inline (phase_10.md §4.1): off the UI thread, with
// progress, and cancellable. The work is any function of an AnalysisControl; it reports
// progress through it and polls `cancelled()` often enough to stop within 200 ms
// (analysis_jobs_cancellable). What it computes is the caller's: the job only runs it.
//
// Results never touch the project here. The app applies a finished job's result as one
// undoable command (or one group of them), on the main thread, so a job that is
// cancelled or fails leaves the project exactly as it was.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace adx::analysis {

class AnalysisControl {
public:
    void report(double fraction) noexcept {
        m_progress.store(static_cast<float>(fraction), std::memory_order_relaxed);
    }
    [[nodiscard]] bool cancelled() const noexcept {
        return m_cancel.load(std::memory_order_relaxed);
    }
    /// `report(fraction)`, then whether to go on: the shape the detectors' progress
    /// callbacks take.
    [[nodiscard]] bool proceed(double fraction) noexcept {
        report(fraction);
        return !cancelled();
    }
    [[nodiscard]] float progress() const noexcept {
        return m_progress.load(std::memory_order_relaxed);
    }
    void cancel() noexcept {
        m_cancel.store(true, std::memory_order_relaxed);
    }

private:
    std::atomic<float> m_progress{0.0F};
    std::atomic<bool> m_cancel{false};
};

class AnalysisJob {
public:
    /// Throws nothing out of the thread: an exception from `work` becomes error().
    using Work = std::function<void(AnalysisControl&)>;

    /// Starts `work` on a thread of its own.
    explicit AnalysisJob(Work work);
    /// Cancels and joins.
    ~AnalysisJob();
    AnalysisJob(const AnalysisJob&) = delete;
    AnalysisJob& operator=(const AnalysisJob&) = delete;
    AnalysisJob(AnalysisJob&&) = delete;
    AnalysisJob& operator=(AnalysisJob&&) = delete;

    [[nodiscard]] float progress() const noexcept {
        return m_control.progress();
    }
    void cancel() noexcept {
        m_control.cancel();
    }
    [[nodiscard]] bool cancelled() const noexcept {
        return m_control.cancelled();
    }
    [[nodiscard]] bool done() const noexcept {
        return m_done.load(std::memory_order_acquire);
    }
    /// Blocks until the work has returned.
    void wait();
    /// What the work threw, or "".
    [[nodiscard]] const std::string& error() const noexcept {
        return m_error;
    }

private:
    AnalysisControl m_control;
    std::string m_error;
    std::atomic<bool> m_done{false};
    std::thread m_thread;
};

} // namespace adx::analysis

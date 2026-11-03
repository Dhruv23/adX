// adx-thread: main
#include "engine/analysis/AnalysisJob.h"

#include <exception>
#include <utility>

namespace adx::analysis {

AnalysisJob::AnalysisJob(Work work)
    : m_thread([this, work = std::move(work)] {
          try {
              work(m_control);
          } catch (const std::exception& e) {
              m_error = e.what();
          } catch (...) {
              m_error = "analysis failed";
          }
          m_control.report(1.0);
          m_done.store(true, std::memory_order_release);
      }) {}

AnalysisJob::~AnalysisJob() {
    cancel();
    wait();
}

void AnalysisJob::wait() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

} // namespace adx::analysis

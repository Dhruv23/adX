// Recording, exposed to Python (phase_8.md §3.1): a session over an engine and a
// project, the comp of its takes, and the loopback calibration.
//
// A session holds references to both handles; the Python wrapper keeps them alive for
// as long as the session (py::keep_alive below).

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "bindings/ProjectHandle.h"
#include "engine/record/DiskWriter.h"
#include "engine/record/InputCapture.h"
#include "engine/record/RecordSession.h"

namespace py = pybind11;
namespace record = adx::record;

namespace {

template<class T> std::optional<T> maybe(const py::dict& d, const char* key) {
    if (d.contains(key) && !d[key].is_none()) {
        return d[key].cast<T>();
    }
    return std::nullopt;
}

record::RecordSettings settingsOf(const py::dict& d) {
    record::RecordSettings s;
    s.track = adx::core::PlaylistTrackId{d["track"].cast<std::uint32_t>()};
    s.from = adx::core::Ticks{maybe<std::int64_t>(d, "from").value_or(0)};
    if (const auto at = maybe<std::int64_t>(d, "punch_in")) {
        s.punchIn = adx::core::Ticks{*at};
    }
    if (const auto at = maybe<std::int64_t>(d, "punch_out")) {
        s.punchOut = adx::core::Ticks{*at};
    }
    s.countIn.bars = maybe<std::uint32_t>(d, "count_in_bars").value_or(0);
    s.countIn.click = maybe<bool>(d, "click").value_or(s.countIn.bars > 0);
    s.monitor = maybe<std::string>(d, "monitor").value_or("direct") == "software"
                    ? record::MonitorMode::Software
                    : record::MonitorMode::Direct;
    s.projectDirectory = maybe<std::string>(d, "project_directory").value_or(".");
    s.name = maybe<std::string>(d, "name").value_or("Take");
    s.measuredRoundTrip = maybe<std::int64_t>(d, "round_trip");
    return s;
}

/// A session and the handles it borrows.
struct SessionHandle {
    std::unique_ptr<record::RecordSession> session;
};

struct CalibrationHandle {
    std::unique_ptr<record::Calibration> calibration;
    adx::bindings::ProjectHandle* project{nullptr};
};

} // namespace

void registerRecordBindings(py::module_& m) {
    py::class_<SessionHandle>(m, "RecordSession",
                              "One take: arm, count in, capture, write, and place it where it "
                              "was heard (phase_8.md §3.1).")
        // GIL: trivial - builds the session; nothing runs until start()
        .def(py::init([](adx::bindings::EngineHandle& engine, adx::bindings::ProjectHandle& project,
                         const py::dict& settings) {
                 auto handle = std::make_unique<SessionHandle>();
                 handle->session = std::make_unique<record::RecordSession>(
                     *engine.engine, project.project, project.stack, settingsOf(settings));
                 return handle;
             }),
             py::arg("engine"), py::arg("project"), py::arg("settings"), py::keep_alive<1, 2>(),
             py::keep_alive<1, 3>(),
             "settings: track, from, punch_in, punch_out, count_in_bars, click, monitor "
             "(direct, software), project_directory, name, round_trip (samples, from a "
             "calibration).")
        // GIL: trivial - installs the capture, starts the writer thread and rolls
        .def("start",
             [](SessionHandle& self) {
                 std::string error;
                 if (!self.session->start(error)) {
                     throw std::runtime_error(error);
                 }
             })
        .def("stop",
             [](SessionHandle& self) {
                 record::TakeResult take;
                 {
                     const py::gil_scoped_release released;
                     take = self.session->stop();
                 }
                 py::dict out;
                 out["ok"] = take.ok;
                 out["error"] = take.error;
                 out["item"] = take.item.value;
                 out["file"] = take.file.string();
                 out["frames"] = take.frames;
                 out["overran"] = take.overran;
                 out["latency"] = take.latency;
                 out["lane"] = take.lane;
                 return out;
             })
        .def_property_readonly(
            "latency",
            [](SessionHandle& self) {
                const record::LatencyReport& report = self.session->latency();
                py::dict out;
                out["input"] = report.inputSamples;
                out["output"] = report.outputSamples;
                out["graph"] = report.graphSamples;
                out["measured"] = report.measuredRoundTrip;
                out["compensation"] = report.compensation();
                return out;
            },
            "What the take is moved back by, and where it comes from. Valid after start().")
        .def_property_readonly(
            "frames", [](SessionHandle& self) { return self.session->capture().framesCaptured(); })
        .def_property_readonly(
            "overruns", [](SessionHandle& self) { return self.session->capture().overrunCount(); });

    py::class_<CalibrationHandle>(m, "Calibration",
                                  "Loopback calibration: wire the interface's output to its "
                                  "input, begin(), let the engine run a second, finish().")
        // GIL: trivial - builds the calibration; nothing runs until begin()
        .def(py::init([](adx::bindings::EngineHandle& engine) {
                 auto handle = std::make_unique<CalibrationHandle>();
                 handle->calibration = std::make_unique<record::Calibration>(*engine.engine);
                 return handle;
             }),
             py::arg("engine"), py::keep_alive<1, 2>())
        // GIL: trivial - installs a capture and queues one impulse
        .def(
            "begin",
            [](CalibrationHandle& self, adx::bindings::ProjectHandle& project,
               std::int64_t pingAt) { self.calibration->begin(project.project, pingAt); },
            py::arg("project"), py::arg("ping_at") = 4800)
        // GIL: trivial - stops the transport and scans the captured second
        .def(
            "finish", [](CalibrationHandle& self) { return self.calibration->finish(); },
            "The measured round trip in samples, or None when the impulse never came back.");

    // GIL: trivial - a handful of commands on the project
    m.def(
        "comp_takes",
        [](adx::bindings::ProjectHandle& project, std::uint32_t track, const py::list& ranges) {
            std::vector<record::CompRange> parsed;
            for (const py::handle& item : ranges) {
                const auto range = item.cast<py::tuple>();
                parsed.push_back(
                    record::CompRange{.from = adx::core::Ticks{range[0].cast<std::int64_t>()},
                                      .to = adx::core::Ticks{range[1].cast<std::int64_t>()},
                                      .take = adx::core::ItemId{range[2].cast<std::uint32_t>()}});
            }
            record::comp(project.project, project.stack, adx::core::PlaylistTrackId{track}, parsed);
        },
        py::arg("project"), py::arg("track"), py::arg("ranges"),
        "Build lane 0 of `track` from (from, to, take item) ranges and mute the takes, as "
        "one undo step.");
}

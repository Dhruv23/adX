// adx-thread: main
//
// adx_plugin_host.exe: the sandbox child (phase_9.md §4.2, §4.3).
//
//   adx_plugin_host --scan <clap|vst3> <path>
//       Loads the file, instantiates each plugin in it once, writes a ScanResult as
//       JSON to stdout and exits. The host gives it a deadline and treats a crash, a
//       hang or malformed output as a failed scan.
//
//   adx_plugin_host --serve
//       Hosts plugin instances for one adX process: control requests on stdin, replies
//       on stdout (Protocol.h), audio through each instance's shared region
//       (SharedAudio.h). Exits when stdin closes - when adX goes away, so does this.
//
// Three threads. A reader turns stdin into a queue. The main thread answers requests
// and pumps window messages for plugin editors; every call into a plugin other than
// process() happens on it, as CLAP and VST3 both require. The audio thread waits on
// every instance's "go" event and processes the one that fired.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <ole2.h>

#include "engine/plugin/Bridge/EditorWindow.h"
#include "engine/plugin/Bridge/Platform.h"
#include "engine/plugin/Bridge/Protocol.h"
#include "engine/plugin/Bridge/SharedAudio.h"
#include "engine/plugin/Instance.h"

namespace bridge = adx::plugin::bridge;
namespace plugin = adx::plugin;

namespace {

struct Request {
    bridge::Op op{bridge::Op::Quit};
    std::uint32_t id{0};
    std::vector<std::uint8_t> payload;
};

struct Hosted {
    std::uint32_t instance{0};
    std::unique_ptr<plugin::Instance> plugin;
    bridge::SharedRegion region;
    bridge::Event go;
    bridge::Event done;
    bridge::SharedAudio* shared{nullptr};
    /// Held by the audio thread while it processes, and by the main thread while it
    /// does anything that must not overlap a process() call.
    std::mutex busy;
    std::uint32_t lastRequest{0};
    /// Set by process() from the plugin's own requests; acted on by the main thread.
    std::atomic<std::uint32_t> pending{0};
};

class Child {
public:
    explicit Child(bridge::ChildStdio stdio) : m_stdio(stdio) {
        m_requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Child() {
        CloseHandle(m_requestEvent);
        CloseHandle(m_wake);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&&) = delete;
    Child& operator=(Child&&) = delete;

    int run() {
        std::thread reader([this] { readLoop(); });
        std::thread audio([this] { audioLoop(); });
        while (!m_quit) {
            MsgWaitForMultipleObjects(1, &m_requestEvent, FALSE, 5, QS_ALLINPUT);
            bridge::pumpMessages();
            for (Request& request : takeRequests()) {
                handle(request);
                if (m_quit) {
                    break;
                }
            }
            idle();
        }
        m_audioRunning = false;
        SetEvent(m_wake);
        audio.join();
        {
            const std::scoped_lock lock(m_listMutex);
            m_hosted.clear();
        }
        m_modules.clear();
        // The reader is blocked in ReadFile on a pipe that may stay open; it dies with
        // the process.
        reader.detach();
        return 0;
    }

private:
    void readLoop() {
        while (true) {
            std::array<std::uint8_t, 4> header{};
            Request request;
            if (!bridge::readExact(m_stdio.in, header)) {
                push(std::move(request)); // EOF: quit
                return;
            }
            std::uint32_t length = 0;
            std::memcpy(&length, header.data(), 4);
            if (length < 8 || length > bridge::kMaxFrameBytes) {
                push(std::move(request));
                return;
            }
            std::vector<std::uint8_t> body(length);
            if (!bridge::readExact(m_stdio.in, body)) {
                push(std::move(request));
                return;
            }
            bridge::ByteReader in(body);
            request.op = static_cast<bridge::Op>(in.u32());
            request.id = in.u32();
            request.payload.assign(body.begin() + 8, body.end());
            const bool quit = request.op == bridge::Op::Quit;
            push(std::move(request));
            if (quit) {
                return;
            }
        }
    }

    void push(Request request) {
        {
            const std::scoped_lock lock(m_queueMutex);
            m_queue.push_back(std::move(request));
        }
        SetEvent(m_requestEvent);
    }

    std::vector<Request> takeRequests() {
        const std::scoped_lock lock(m_queueMutex);
        std::vector<Request> out(std::make_move_iterator(m_queue.begin()),
                                 std::make_move_iterator(m_queue.end()));
        m_queue.clear();
        return out;
    }

    void reply(std::uint32_t id, bridge::Status status, const bridge::ByteWriter& payload) const {
        const auto bytes = bridge::replyFrame(id, status, payload.data());
        static_cast<void>(bridge::writeAll(m_stdio.out, bytes));
    }

    Hosted* find(std::uint32_t instance) {
        const std::scoped_lock lock(m_listMutex);
        for (auto& hosted : m_hosted) {
            if (hosted->instance == instance) {
                return hosted.get();
            }
        }
        return nullptr;
    }

    plugin::Module* module(plugin::PluginFormat format, const std::string& path,
                           std::string& error) {
        const std::string key = std::string(plugin::toString(format)) + "|" + path;
        auto found = m_modules.find(key);
        if (found == m_modules.end()) {
            auto loaded = plugin::loadModule(format, path, error);
            if (!loaded) {
                return nullptr;
            }
            found = m_modules.emplace(key, std::move(loaded)).first;
        }
        return found->second.get();
    }

    void handle(Request& request) {
        bridge::ByteReader in(request.payload);
        bridge::ByteWriter out;
        auto status = bridge::Status::Ok;
        switch (request.op) {
        case bridge::Op::Reply:
            return;
        case bridge::Op::Hello:
            out.u32(bridge::kProtocolVersion);
            break;
        case bridge::Op::Load:
            status = load(in, out);
            break;
        case bridge::Op::Unload:
            unload(in.u32());
            break;
        case bridge::Op::SetState: {
            const std::uint32_t instance = in.u32();
            const std::vector<std::uint8_t> bytes = in.bytes();
            Hosted* hosted = find(instance);
            bool loaded = false;
            if (hosted != nullptr) {
                const std::scoped_lock lock(hosted->busy);
                loaded = hosted->plugin->loadState(bytes);
                hosted->shared->control.latency.store(hosted->plugin->latency());
            }
            status = loaded ? bridge::Status::Ok : bridge::Status::Failed;
            break;
        }
        case bridge::Op::GetState: {
            Hosted* hosted = find(in.u32());
            plugin::StateBlob blob;
            if (hosted == nullptr || !hosted->plugin->saveState(blob)) {
                status = bridge::Status::Failed;
            }
            out.bytes(blob);
            break;
        }
        case bridge::Op::GetParamValues: {
            Hosted* hosted = find(in.u32());
            if (hosted == nullptr) {
                status = bridge::Status::Failed;
                out.u32(0);
                break;
            }
            const plugin::PluginDescriptor d = hosted->plugin->describe();
            out.u32(static_cast<std::uint32_t>(d.params.size()));
            for (const plugin::ParamInfo& p : d.params) {
                double value = p.defaultValue;
                static_cast<void>(hosted->plugin->paramValue(p.id, value));
                out.u32(p.id);
                out.f64(value);
            }
            break;
        }
        case bridge::Op::ParamText: {
            Hosted* hosted = find(in.u32());
            const std::uint32_t id = in.u32();
            const double value = in.f64();
            std::string text;
            if (hosted == nullptr || !hosted->plugin->paramText(id, value, text)) {
                status = bridge::Status::Failed;
            }
            out.str(text);
            break;
        }
        case bridge::Op::OpenEditor: {
            Hosted* hosted = find(in.u32());
            std::uint64_t window = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (hosted == nullptr || !hosted->plugin->openEditor(window, width, height)) {
                status = bridge::Status::Failed;
            }
            out.u64(window);
            out.u32(width);
            out.u32(height);
            break;
        }
        case bridge::Op::CloseEditor:
            if (Hosted* hosted = find(in.u32())) {
                hosted->plugin->closeEditor();
            }
            break;
        case bridge::Op::Restart: {
            Hosted* hosted = find(in.u32());
            std::string error;
            if (hosted == nullptr || !restart(*hosted, error)) {
                status = bridge::Status::Failed;
            }
            out.u32(hosted != nullptr ? hosted->shared->control.latency.load() : 0);
            break;
        }
        case bridge::Op::Quit:
            m_quit = true;
            break;
        }
        reply(request.id, status, out);
    }

    bridge::Status load(bridge::ByteReader& in, bridge::ByteWriter& out) {
        bridge::LoadRequest request;
        bridge::LoadReply result;
        if (!request.read(in)) {
            result.error = "malformed load request";
            result.write(out);
            return bridge::Status::Failed;
        }
        auto hosted = std::make_unique<Hosted>();
        hosted->instance = request.instance;
        if (!hosted->region.open(request.region, sizeof(bridge::SharedAudio)) ||
            !hosted->go.open(request.goEvent) || !hosted->done.open(request.doneEvent)) {
            result.error = "could not open the shared region or its events";
            result.write(out);
            return bridge::Status::Failed;
        }
        hosted->shared = static_cast<bridge::SharedAudio*>(hosted->region.data());
        std::string error;
        plugin::Module* mod = module(request.format, request.path, error);
        if (mod == nullptr) {
            result.error = error;
            result.write(out);
            return bridge::Status::Failed;
        }
        hosted->plugin = mod->create(request.uid, error);
        if (!hosted->plugin) {
            result.error = error;
            result.write(out);
            return bridge::Status::Failed;
        }
        const std::uint32_t maxFrames = std::min(request.maxFrames, bridge::kMaxBlockFrames);
        if (!hosted->plugin->activate(request.sampleRate, maxFrames, error)) {
            result.error = error;
            result.write(out);
            return bridge::Status::Failed;
        }
        result.descriptor = hosted->plugin->describe();
        hosted->shared->control.latency.store(result.descriptor.latency);
        hosted->lastRequest = hosted->shared->control.request.load(std::memory_order_acquire);
        {
            const std::scoped_lock lock(m_listMutex);
            m_hosted.push_back(std::move(hosted));
            m_listVersion.fetch_add(1);
        }
        SetEvent(m_wake);
        result.write(out);
        return bridge::Status::Ok;
    }

    void unload(std::uint32_t instance) {
        std::unique_ptr<Hosted> removed;
        std::uint64_t version = 0;
        {
            const std::scoped_lock lock(m_listMutex);
            const auto match = std::ranges::find_if(
                m_hosted, [instance](const auto& h) { return h->instance == instance; });
            if (match == m_hosted.end()) {
                return;
            }
            removed = std::move(*match);
            m_hosted.erase(match);
            version = m_listVersion.fetch_add(1) + 1;
        }
        SetEvent(m_wake);
        // The audio thread may still hold the pointer from its last list. Wait until it
        // has taken the new one; one stuck inside a hung plugin never will, and then the
        // instance is kept rather than freed under it - the host kills this child soon.
        for (int waited = 0; m_seenVersion.load() < version; ++waited) {
            if (waited >= 2000) {
                m_graveyard.push_back(std::move(removed));
                return;
            }
            Sleep(1);
        }
        const std::scoped_lock lock(removed->busy);
        removed->plugin->closeEditor();
        removed->plugin->deactivate();
        removed->plugin.reset();
    }

    static bool restart(Hosted& hosted, std::string& error) {
        const std::scoped_lock lock(hosted.busy);
        const bool ok = hosted.plugin->restart(error);
        // Blocks that ran between the request and this lock asked again for the same
        // restart; the plugin now has its new latency, so those asks are stale.
        hosted.pending.store(0);
        hosted.shared->control.latency.store(hosted.plugin->latency());
        hosted.shared->control.status.fetch_or(bridge::kLatencyChanged);
        return ok;
    }

    /// The plugins' main-thread housekeeping, and the restarts their process() asked
    /// for.
    void idle() {
        std::vector<Hosted*> list;
        {
            const std::scoped_lock lock(m_listMutex);
            for (auto& hosted : m_hosted) {
                list.push_back(hosted.get());
            }
        }
        for (Hosted* hosted : list) {
            hosted->plugin->idle();
            const std::uint32_t pending = hosted->pending.exchange(0);
            if ((pending & (bridge::kLatencyChanged | bridge::kRestartRequested)) != 0) {
                std::string error;
                static_cast<void>(restart(*hosted, error));
            }
        }
    }

    void audioLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        std::vector<void*> handles;
        std::vector<Hosted*> order;
        while (m_audioRunning) {
            handles.assign(1, m_wake);
            order.clear();
            {
                const std::scoped_lock lock(m_listMutex);
                m_seenVersion.store(m_listVersion.load());
                for (auto& hosted : m_hosted) {
                    if (handles.size() < MAXIMUM_WAIT_OBJECTS) {
                        handles.push_back(hosted->go.handle());
                        order.push_back(hosted.get());
                    }
                }
            }
            while (m_audioRunning) {
                const int index = bridge::waitAny(handles, 100);
                if (index == 0) {
                    break; // the list changed
                }
                if (index < 0) {
                    continue;
                }
                Hosted* hosted = order.at(static_cast<std::size_t>(index - 1));
                // Waits out a main-thread call on this instance (a state load) rather
                // than skipping: the go event is consumed, so a skipped block would
                // never be answered and the host would wait on it for good.
                const std::scoped_lock lock(hosted->busy);
                if (hosted->plugin) {
                    processBlock(*hosted);
                }
            }
        }
    }

    static void processBlock(Hosted& hosted) noexcept {
        bridge::SharedAudio& shared = *hosted.shared;
        const std::uint32_t request = shared.control.request.load(std::memory_order_acquire);
        if (request == hosted.lastRequest) {
            return;
        }
        hosted.lastRequest = request;
        const std::uint32_t frames = std::min(shared.control.frames, bridge::kMaxBlockFrames);
        std::array<const float*, bridge::kMaxChannels> inputs{};
        std::array<float*, bridge::kMaxChannels> outputs{};
        for (std::size_t c = 0; c < bridge::kMaxChannels; ++c) {
            inputs.at(c) = shared.input.at(c).data();
            outputs.at(c) = shared.output.at(c).data();
        }
        plugin::ProcessIo io;
        io.inputs = inputs;
        io.outputs = outputs;
        io.frames = frames;
        io.events = std::span<const bridge::WireEvent>(
            shared.events.data(), std::min(shared.control.eventCount, bridge::kMaxEvents));
        io.params = std::span<const bridge::WireParam>(
            shared.params.data(), std::min(shared.control.paramCount, bridge::kMaxParamChanges));
        io.transport = shared.control.transport;
        io.outParams = shared.outParams;
        hosted.plugin->process(io);
        shared.control.outParamCount = io.outCount;
        if ((io.status & bridge::kProcessFailed) != 0) {
            shared.control.status.fetch_or(bridge::kProcessFailed);
        }
        if ((io.status & (bridge::kLatencyChanged | bridge::kRestartRequested)) != 0) {
            hosted.pending.fetch_or(io.status);
        }
        shared.control.processed.fetch_add(1, std::memory_order_relaxed);
        shared.control.reply.store(request, std::memory_order_release);
        hosted.done.set();
    }

    bridge::ChildStdio m_stdio;
    HANDLE m_requestEvent{nullptr};
    HANDLE m_wake{nullptr};
    std::mutex m_queueMutex;
    std::deque<Request> m_queue;
    std::mutex m_listMutex;
    std::vector<std::unique_ptr<Hosted>> m_hosted;
    std::vector<std::unique_ptr<Hosted>> m_graveyard;
    std::atomic<std::uint64_t> m_listVersion{0};
    std::atomic<std::uint64_t> m_seenVersion{0};
    std::map<std::string, std::unique_ptr<plugin::Module>> m_modules;
    std::atomic<bool> m_audioRunning{true};
    bool m_quit{false};
};

int scan(bridge::ChildStdio stdio, const std::string& formatName, const std::string& path) {
    plugin::ScanResult result;
    result.path = path;
    static_cast<void>(plugin::formatFromString(formatName, result.format));
    static_cast<void>(bridge::fileStamp(path, result.mtime, result.size));
    std::string error;
    auto module = plugin::loadModule(result.format, path, error);
    if (module) {
        result.plugins = module->scan(error);
        result.ok = !result.plugins.empty();
        if (!result.ok && error.empty()) {
            error = "the file contains no plugins";
        }
    }
    result.error = error;
    const std::string text = plugin::toJson(result);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    static_cast<void>(
        bridge::writeAll(stdio.out, std::span<const std::uint8_t>(bytes, text.size())));
    // Exit without unloading: some plugins crash in their own teardown, and the scan is
    // already written.
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

} // namespace

namespace {

int run(int argc, char** argv) {
    // A crashing plugin must not open the Windows error dialog: the host is watching
    // for the exit, and a dialog would turn a crash into a hang.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    const bridge::ChildStdio stdio = bridge::claimStdio();
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 3 && args[0] == "--scan") {
        return scan(stdio, args[1], args[2]);
    }
    if (!args.empty() && args[0] == "--serve") {
        OleInitialize(nullptr); // plugin editors use COM and drag and drop
        Child child(stdio);
        const int code = child.run();
        OleUninitialize();
        return code;
    }
    std::fprintf(stderr, "usage: adx_plugin_host --scan <clap|vst3> <path> | --serve\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "adx_plugin_host: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "adx_plugin_host: unknown failure\n");
    }
    return 3;
}

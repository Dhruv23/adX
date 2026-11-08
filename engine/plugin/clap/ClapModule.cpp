// adx-thread: main
#include "engine/plugin/clap/ClapModule.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <clap/clap.h>

#include "engine/plugin/Bridge/EditorWindow.h"
#include "engine/plugin/Bridge/Platform.h"

namespace adx::plugin::clap {

namespace {

class ClapInstance;

/// The host side of one plugin. CLAP hands every callback the clap_host it was given,
/// so each instance has its own, with `host_data` pointing back at the instance.
struct HostCallbacks {
    static const void* getExtension(const clap_host_t* host, const char* id);
    static void requestRestart(const clap_host_t* host);
    static void requestProcess(const clap_host_t* host);
    static void requestCallback(const clap_host_t* host);
    static void latencyChanged(const clap_host_t* host);
    static void paramsRescan(const clap_host_t* host, clap_param_rescan_flags flags);
    static void paramsClear(const clap_host_t* host, clap_id id, clap_param_clear_flags flags);
    static void paramsFlush(const clap_host_t* host);
    static void stateDirty(const clap_host_t* host);
    static void log(const clap_host_t* host, clap_log_severity severity, const char* message);
    static bool isMainThread(const clap_host_t* host);
    static bool isAudioThread(const clap_host_t* host);
    static void guiResizeHintsChanged(const clap_host_t* host);
    static bool guiRequestResize(const clap_host_t* host, uint32_t width, uint32_t height);
    static bool guiRequestShow(const clap_host_t* host);
    static bool guiRequestHide(const clap_host_t* host);
    static void guiClosed(const clap_host_t* host, bool destroyed);
};

constexpr clap_host_latency kHostLatency{HostCallbacks::latencyChanged};
constexpr clap_host_params kHostParams{.rescan = HostCallbacks::paramsRescan,
                                       .clear = HostCallbacks::paramsClear,
                                       .request_flush = HostCallbacks::paramsFlush};
constexpr clap_host_state kHostState{HostCallbacks::stateDirty};
constexpr clap_host_log kHostLog{HostCallbacks::log};
constexpr clap_host_thread_check kHostThreadCheck{.is_main_thread = HostCallbacks::isMainThread,
                                                  .is_audio_thread = HostCallbacks::isAudioThread};
constexpr clap_host_gui kHostGui{.resize_hints_changed = HostCallbacks::guiResizeHintsChanged,
                                 .request_resize = HostCallbacks::guiRequestResize,
                                 .request_show = HostCallbacks::guiRequestShow,
                                 .request_hide = HostCallbacks::guiRequestHide,
                                 .closed = HostCallbacks::guiClosed};

/// The ordered input events of one block: note, MIDI and parameter events in
/// preallocated storage, and the time-sorted list of pointers CLAP reads.
struct InputEvents {
    std::vector<clap_event_note> notes;
    std::vector<clap_event_midi> midi;
    std::vector<clap_event_param_value> params;
    std::vector<clap_event_note_expression> expressions;
    std::vector<const clap_event_header_t*> order;
    clap_input_events list{};

    void reserve(std::size_t events, std::size_t paramChanges) {
        notes.reserve(events);
        midi.reserve(events);
        params.reserve(paramChanges);
        expressions.reserve(events);
        order.reserve(events + events + paramChanges);
        list.ctx = this;
        list.size = [](const clap_input_events* l) -> uint32_t {
            return static_cast<uint32_t>(static_cast<const InputEvents*>(l->ctx)->order.size());
        };
        list.get = [](const clap_input_events* l, uint32_t index) -> const clap_event_header_t* {
            const auto& o = static_cast<const InputEvents*>(l->ctx)->order;
            return index < o.size() ? o[index] : nullptr;
        };
    }
};

struct OutputEvents {
    ProcessIo* io{nullptr};
    clap_output_events list{};
    /// Parameters inside a gesture now: their values are the user's (P9-7).
    std::array<clap_id, 32> gestures{};
    std::size_t gestureCount{0};

    [[nodiscard]] bool inGesture(clap_id id) const noexcept {
        return std::find(gestures.begin(),
                         gestures.begin() + static_cast<std::ptrdiff_t>(gestureCount),
                         id) != gestures.begin() + static_cast<std::ptrdiff_t>(gestureCount);
    }

    OutputEvents() {
        list.ctx = this;
        list.try_push = [](const clap_output_events* l, const clap_event_header_t* event) -> bool {
            auto* self = static_cast<OutputEvents*>(l->ctx);
            if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) {
                return true;
            }
            if (event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ||
                event->type == CLAP_EVENT_PARAM_GESTURE_END) {
                const clap_id id =
                    reinterpret_cast<const clap_event_param_gesture*>(event)->param_id;
                const auto end =
                    self->gestures.begin() + static_cast<std::ptrdiff_t>(self->gestureCount);
                const auto at = std::find(self->gestures.begin(), end, id);
                if (event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
                    if (at == end && self->gestureCount < self->gestures.size()) {
                        self->gestures.at(self->gestureCount++) = id;
                    }
                } else if (at != end) {
                    *at = self->gestures.at(--self->gestureCount);
                }
                return true;
            }
            if (self->io == nullptr || event->type != CLAP_EVENT_PARAM_VALUE) {
                return true; // note-end and the rest: accepted and ignored
            }
            ProcessIo& io = *self->io;
            if (io.outCount >= io.outParams.size()) {
                return false;
            }
            const auto* value = reinterpret_cast<const clap_event_param_value*>(event);
            io.outParams[io.outCount++] = bridge::WireParam{
                .offset = event->time,
                .id = value->param_id,
                .value = value->value,
                .flags = self->inGesture(value->param_id) ? bridge::kParamGesture : 0U,
                .reserved = 0};
            return true;
        };
    }
};

class ClapInstance final : public Instance {
public:
    ClapInstance(std::shared_ptr<bridge::Library> library, std::string path)
        : m_library(std::move(library)), m_path(std::move(path)) {
        m_host.clap_version = CLAP_VERSION;
        m_host.host_data = this;
        m_host.name = "adX";
        m_host.vendor = "adX";
        m_host.url = "";
        m_host.version = "0.1";
        m_host.get_extension = HostCallbacks::getExtension;
        m_host.request_restart = HostCallbacks::requestRestart;
        m_host.request_process = HostCallbacks::requestProcess;
        m_host.request_callback = HostCallbacks::requestCallback;
        m_mainThread = std::this_thread::get_id();
    }

    ~ClapInstance() override {
        closeEditor();
        if (m_plugin != nullptr) {
            deactivate();
            m_plugin->destroy(m_plugin);
        }
    }
    ClapInstance(const ClapInstance&) = delete;
    ClapInstance& operator=(const ClapInstance&) = delete;
    ClapInstance(ClapInstance&&) = delete;
    ClapInstance& operator=(ClapInstance&&) = delete;

    bool create(const clap_plugin_factory* factory, const std::string& uid, std::string& error) {
        m_plugin = factory->create_plugin(factory, &m_host, uid.c_str());
        if (m_plugin == nullptr) {
            error = "the factory could not create '" + uid + "'";
            return false;
        }
        if (!m_plugin->init(m_plugin)) {
            m_plugin->destroy(m_plugin);
            m_plugin = nullptr;
            error = "'" + uid + "' failed to initialise";
            return false;
        }
        m_params = static_cast<const clap_plugin_params*>(
            m_plugin->get_extension(m_plugin, CLAP_EXT_PARAMS));
        m_state = static_cast<const clap_plugin_state*>(
            m_plugin->get_extension(m_plugin, CLAP_EXT_STATE));
        m_latency = static_cast<const clap_plugin_latency*>(
            m_plugin->get_extension(m_plugin, CLAP_EXT_LATENCY));
        m_ports = static_cast<const clap_plugin_audio_ports*>(
            m_plugin->get_extension(m_plugin, CLAP_EXT_AUDIO_PORTS));
        m_gui =
            static_cast<const clap_plugin_gui*>(m_plugin->get_extension(m_plugin, CLAP_EXT_GUI));
        return true;
    }

    bool activate(double sampleRate, std::uint32_t maxFrames, std::string& error) override {
        if (m_active) {
            deactivate();
        }
        m_sampleRate = sampleRate;
        m_maxFrames = maxFrames;
        m_events.reserve(bridge::kMaxEvents, bridge::kMaxParamChanges);
        for (auto& channel : m_scratchIn) {
            channel.assign(maxFrames, 0.0F);
        }
        for (auto& channel : m_scratchOut) {
            channel.assign(maxFrames, 0.0F);
        }
        if (!m_plugin->activate(m_plugin, sampleRate, 1, maxFrames)) {
            error = "the plugin refused to activate";
            return false;
        }
        m_active = true;
        m_processing = false;
        return true;
    }

    void deactivate() override {
        if (!m_active) {
            return;
        }
        // stop_processing belongs to the audio thread. The child's audio thread is not
        // running this instance while the main thread deactivates it (ChildMain), so
        // this thread stands in for it for the one call, and says so when the plugin
        // asks (thread-check: Dexed, among others, checks and complains).
        if (m_processing) {
            m_standingInForAudio = true;
            m_plugin->stop_processing(m_plugin);
            m_standingInForAudio = false;
            m_processing = false;
        }
        m_plugin->deactivate(m_plugin);
        m_active = false;
    }

    void process(ProcessIo& io) noexcept override {
        if (!m_active) {
            return;
        }
        if (!m_processing) {
            m_processing = m_plugin->start_processing(m_plugin);
            if (!m_processing) {
                for (float* out : io.outputs) {
                    std::fill_n(out, io.frames, 0.0F);
                }
                return;
            }
        }
        buildEvents(io);
        m_flushRequested.store(false); // this process() is the flush it asked for

        std::array<float*, bridge::kMaxChannels> inPtrs{};
        std::array<float*, bridge::kMaxChannels> outPtrs{};
        for (std::size_t c = 0; c < bridge::kMaxChannels; ++c) {
            // A plugin may write its input buffers; it gets copies, never the region.
            const float* source = c < io.inputs.size() ? io.inputs[c] : nullptr;
            if (source != nullptr) {
                std::copy_n(source, io.frames, m_scratchIn.at(c).data());
            } else {
                std::fill_n(m_scratchIn.at(c).data(), io.frames, 0.0F);
            }
            inPtrs.at(c) = m_scratchIn.at(c).data();
            outPtrs.at(c) = c < io.outputs.size() ? io.outputs[c] : m_scratchOut.at(c).data();
        }
        clap_audio_buffer input{};
        input.data32 = inPtrs.data();
        input.channel_count = m_inputChannels;
        clap_audio_buffer output{};
        output.data32 = outPtrs.data();
        output.channel_count = m_outputChannels;

        clap_event_transport transport{};
        transport.header.size = sizeof transport;
        transport.header.type = CLAP_EVENT_TRANSPORT;
        transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                          CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        if ((io.transport.flags & bridge::kPlaying) != 0) {
            transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
        }
        transport.tempo = io.transport.tempo;
        transport.song_pos_beats = static_cast<clap_beattime>(
            io.transport.beats * static_cast<double>(CLAP_BEATTIME_FACTOR));
        transport.tsig_num = io.transport.numerator;
        transport.tsig_denom = io.transport.denominator;

        m_out.io = &io;
        clap_process process{};
        process.steady_time = static_cast<int64_t>(m_steadyTime);
        process.frames_count = io.frames;
        process.transport = &transport;
        process.audio_inputs = &input;
        process.audio_inputs_count = m_inputChannels > 0 ? 1 : 0;
        process.audio_outputs = &output;
        process.audio_outputs_count = m_outputChannels > 0 ? 1 : 0;
        process.in_events = &m_events.list;
        process.out_events = &m_out.list;
        const clap_process_status status = m_plugin->process(m_plugin, &process);
        m_out.io = nullptr;
        m_steadyTime += io.frames;
        if (status == CLAP_PROCESS_ERROR) {
            for (float* out : io.outputs) {
                std::fill_n(out, io.frames, 0.0F);
            }
            io.status |= bridge::kProcessFailed;
        }
        // A mono plugin: give its one channel to both sides.
        if (m_outputChannels == 1 && io.outputs.size() > 1) {
            std::copy_n(io.outputs[0], io.frames, io.outputs[1]);
        }
        if (m_latencyChanged.exchange(false, std::memory_order_acq_rel)) {
            io.status |= bridge::kLatencyChanged;
        }
        if (m_restartRequested.load(std::memory_order_acquire)) {
            io.status |= bridge::kRestartRequested;
        }
    }

    PluginDescriptor describe() override {
        PluginDescriptor d;
        const clap_plugin_descriptor* desc = m_plugin->desc;
        d.id.format = PluginFormat::Clap;
        d.id.uid = desc->id != nullptr ? desc->id : "";
        d.id.path = m_path;
        d.id.name = desc->name != nullptr ? desc->name : d.id.uid;
        d.id.version = desc->version != nullptr ? desc->version : "";
        d.vendor = desc->vendor != nullptr ? desc->vendor : "";
        d.instrument = false;
        if (desc->features != nullptr) {
            for (const char* const* f = desc->features; *f != nullptr; ++f) {
                if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) {
                    d.instrument = true;
                }
            }
        }
        d.audioInputs = m_inputChannels;
        d.audioOutputs = m_outputChannels;
        d.hasEditor =
            m_gui != nullptr && m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_WIN32, false);
        d.latency = latency();
        if (m_params != nullptr) {
            const uint32_t count = m_params->count(m_plugin);
            for (uint32_t i = 0; i < count; ++i) {
                clap_param_info info{};
                if (!m_params->get_info(m_plugin, i, &info)) {
                    continue;
                }
                ParamInfo p;
                p.id = info.id;
                p.name = info.name;
                p.minimum = info.min_value;
                p.maximum = info.max_value;
                p.defaultValue = info.default_value;
                if ((info.flags & CLAP_PARAM_IS_STEPPED) != 0) {
                    p.steps =
                        static_cast<std::uint32_t>(std::max(0.0, info.max_value - info.min_value));
                }
                p.automatable = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0 &&
                                (info.flags & CLAP_PARAM_IS_READONLY) == 0;
                p.hidden = (info.flags & CLAP_PARAM_IS_HIDDEN) != 0;
                d.params.push_back(std::move(p));
            }
        }
        assignParamKeys(d.params);
        return d;
    }

    bool paramValue(std::uint32_t id, double& out) override {
        return m_params != nullptr && m_params->get_value(m_plugin, id, &out);
    }

    bool paramText(std::uint32_t id, double value, std::string& out) override {
        if (m_params == nullptr) {
            return false;
        }
        std::array<char, 256> text{};
        if (!m_params->value_to_text(m_plugin, id, value, text.data(),
                                     static_cast<uint32_t>(text.size()))) {
            return false;
        }
        out = text.data();
        return true;
    }

    bool saveState(StateBlob& out) override {
        out.clear();
        if (m_state == nullptr) {
            return true;
        }
        clap_ostream stream{};
        stream.ctx = &out;
        stream.write = [](const clap_ostream* s, const void* buffer, uint64_t size) -> int64_t {
            auto* blob = static_cast<StateBlob*>(s->ctx);
            const auto* bytes = static_cast<const std::uint8_t*>(buffer);
            blob->insert(blob->end(), bytes, bytes + size);
            return static_cast<int64_t>(size);
        };
        return m_state->save(m_plugin, &stream);
    }

    bool loadState(std::span<const std::uint8_t> bytes) override {
        if (m_state == nullptr) {
            return bytes.empty();
        }
        struct Cursor {
            std::span<const std::uint8_t> bytes;
            std::size_t at{0};
        } cursor{.bytes = bytes};
        clap_istream stream{};
        stream.ctx = &cursor;
        stream.read = [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
            auto* c = static_cast<Cursor*>(s->ctx);
            const std::size_t n = std::min<std::size_t>(size, c->bytes.size() - c->at);
            std::memcpy(buffer, c->bytes.data() + c->at, n);
            c->at += n;
            return static_cast<int64_t>(n);
        };
        return m_state->load(m_plugin, &stream);
    }

    std::uint32_t latency() override {
        return m_latency != nullptr && m_active ? m_latency->get(m_plugin) : 0;
    }

    void idle() override {
        if (m_callbackRequested.exchange(false)) {
            m_plugin->on_main_thread(m_plugin);
        }
        // CLAP: flush() is the main thread's only while the plugin is inactive; once
        // active, process() is the flush (P10.5-2, closed with P9-7: a value report from
        // it carries no gesture, and adX does not record those as edits).
        if (m_flushRequested.exchange(false) && m_params != nullptr && !m_active) {
            InputEvents none;
            none.reserve(0, 0);
            OutputEvents const out;
            m_params->flush(m_plugin, &none.list, &out.list);
        }
    }

    bool restart(std::string& error) override {
        m_restartRequested.store(false);
        deactivate();
        return activate(m_sampleRate, m_maxFrames, error);
    }

    bool openEditor(std::uint64_t& window, std::uint32_t& width, std::uint32_t& height) override {
        if (m_gui == nullptr || !m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_WIN32, false)) {
            return false;
        }
        if (!m_editorOpen) {
            if (!m_gui->create(m_plugin, CLAP_WINDOW_API_WIN32, false)) {
                return false;
            }
            uint32_t w = 400;
            uint32_t h = 300;
            m_gui->get_size(m_plugin, &w, &h);
            if (!m_window.create(m_plugin->desc->name, w, h)) {
                m_gui->destroy(m_plugin);
                return false;
            }
            clap_window parent{};
            parent.api = CLAP_WINDOW_API_WIN32;
            parent.win32 = m_window.handle();
            if (!m_gui->set_parent(m_plugin, &parent)) {
                m_gui->destroy(m_plugin);
                m_window.destroy();
                return false;
            }
            m_gui->show(m_plugin);
            m_window.show();
            m_editorWidth = w;
            m_editorHeight = h;
            m_editorOpen = true;
        }
        window = reinterpret_cast<std::uintptr_t>(m_window.handle());
        width = m_editorWidth;
        height = m_editorHeight;
        return true;
    }

    void closeEditor() override {
        if (m_editorOpen) {
            m_gui->hide(m_plugin);
            m_gui->destroy(m_plugin);
            m_window.destroy();
            m_editorOpen = false;
        }
    }

    /// Bus layout, read before activation (CLAP allows it only then).
    void readPorts() {
        m_inputChannels = 0;
        m_outputChannels = 0;
        if (m_ports == nullptr) {
            return;
        }
        clap_audio_port_info info{};
        if (m_ports->count(m_plugin, true) > 0 && m_ports->get(m_plugin, 0, true, &info)) {
            m_inputChannels = std::min<std::uint32_t>(info.channel_count, bridge::kMaxChannels);
        }
        if (m_ports->count(m_plugin, false) > 0 && m_ports->get(m_plugin, 0, false, &info)) {
            m_outputChannels = std::min<std::uint32_t>(info.channel_count, bridge::kMaxChannels);
        }
    }

    // Host callbacks land here.
    void onRestart() noexcept {
        m_restartRequested.store(true);
    }
    void onCallback() noexcept {
        m_callbackRequested.store(true);
    }
    void onLatency() noexcept {
        m_latencyChanged.store(true);
    }
    void onFlush() noexcept {
        m_flushRequested.store(true);
    }
    void onResize(std::uint32_t width, std::uint32_t height) noexcept {
        m_editorWidth = width;
        m_editorHeight = height;
        m_window.resize(width, height);
    }
    [[nodiscard]] bool onMainThread() const noexcept {
        return std::this_thread::get_id() == m_mainThread && !m_standingInForAudio;
    }

private:
    // Never allocates: every vector was reserved at activate() for the region's own
    // limits (kMaxEvents, kMaxParamChanges), which bound what io can hold.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    void buildEvents(const ProcessIo& io) noexcept {
        m_events.notes.clear();
        m_events.midi.clear();
        m_events.params.clear();
        m_events.expressions.clear();
        m_events.order.clear();
        for (const bridge::WireEvent& e : io.events) {
            const std::uint32_t time = std::min(e.offset, io.frames == 0 ? 0 : io.frames - 1);
            if (e.kind == bridge::WireEventKind::Midi) {
                clap_event_midi midi{};
                midi.header = {.size = sizeof midi,
                               .time = time,
                               .space_id = CLAP_CORE_EVENT_SPACE_ID,
                               .type = CLAP_EVENT_MIDI,
                               .flags = 0};
                midi.port_index = 0;
                midi.data[0] = e.status;
                midi.data[1] = e.key;
                midi.data[2] = e.velocity;
                m_events.midi.push_back(midi);
                continue;
            }
            if (e.kind == bridge::WireEventKind::Tuning) {
                clap_event_note_expression expression{};
                expression.header = {.size = sizeof expression,
                                     .time = time,
                                     .space_id = CLAP_CORE_EVENT_SPACE_ID,
                                     .type = CLAP_EVENT_NOTE_EXPRESSION,
                                     .flags = 0};
                expression.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
                expression.note_id = e.noteId;
                expression.port_index = 0;
                expression.channel = e.channel;
                expression.key = e.key;
                expression.value = static_cast<double>(e.value);
                m_events.expressions.push_back(expression);
                continue;
            }
            clap_event_note note{};
            const bool on = e.kind == bridge::WireEventKind::NoteOn;
            note.header = {.size = sizeof note,
                           .time = time,
                           .space_id = CLAP_CORE_EVENT_SPACE_ID,
                           .type =
                               static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF),
                           .flags = 0};
            if (e.kind == bridge::WireEventKind::AllNotesOff) {
                // CLAP's wildcards: every note on every channel.
                note.note_id = -1;
                note.port_index = -1;
                note.channel = -1;
                note.key = -1;
            } else {
                note.note_id = e.noteId;
                note.port_index = 0;
                note.channel = e.channel;
                note.key = e.key;
            }
            note.velocity = static_cast<double>(e.velocity) / 127.0;
            m_events.notes.push_back(note);
        }
        for (const bridge::WireParam& p : io.params) {
            clap_event_param_value value{};
            value.header = {.size = sizeof value,
                            .time = std::min(p.offset, io.frames == 0 ? 0 : io.frames - 1),
                            .space_id = CLAP_CORE_EVENT_SPACE_ID,
                            .type = CLAP_EVENT_PARAM_VALUE,
                            .flags = 0};
            value.param_id = p.id;
            value.note_id = -1;
            value.port_index = -1;
            value.channel = -1;
            value.key = -1;
            value.value = p.value;
            m_events.params.push_back(value);
        }
        for (const auto& n : m_events.notes) {
            m_events.order.push_back(&n.header);
        }
        for (const auto& m : m_events.midi) {
            m_events.order.push_back(&m.header);
        }
        for (const auto& p : m_events.params) {
            m_events.order.push_back(&p.header);
        }
        for (const auto& x : m_events.expressions) {
            m_events.order.push_back(&x.header);
        }
        // Stable: at one time, parameter values after the notes they follow in the
        // host's list is not promised, but notes keep their own order.
        std::ranges::stable_sort(m_events.order,
                                 [](const clap_event_header_t* a, const clap_event_header_t* b) {
                                     return a->time < b->time;
                                 });
    }

    std::shared_ptr<bridge::Library> m_library;
    std::string m_path;
    clap_host_t m_host{};
    const clap_plugin_t* m_plugin{nullptr};
    const clap_plugin_params* m_params{nullptr};
    const clap_plugin_state* m_state{nullptr};
    const clap_plugin_latency* m_latency{nullptr};
    const clap_plugin_audio_ports* m_ports{nullptr};
    const clap_plugin_gui* m_gui{nullptr};
    std::thread::id m_mainThread;
    /// deactivate()'s stop_processing call, made for the stopped audio thread.
    bool m_standingInForAudio{false};
    double m_sampleRate{48000.0};
    std::uint32_t m_maxFrames{4096};
    std::uint32_t m_inputChannels{2};
    std::uint32_t m_outputChannels{2};
    bool m_active{false};
    bool m_processing{false};
    std::uint64_t m_steadyTime{0};
    InputEvents m_events;
    OutputEvents m_out;
    std::array<std::vector<float>, bridge::kMaxChannels> m_scratchIn;
    std::array<std::vector<float>, bridge::kMaxChannels> m_scratchOut;
    std::atomic<bool> m_restartRequested{false};
    std::atomic<bool> m_callbackRequested{false};
    std::atomic<bool> m_latencyChanged{false};
    std::atomic<bool> m_flushRequested{false};
    bridge::EditorWindow m_window;
    bool m_editorOpen{false};
    std::uint32_t m_editorWidth{0};
    std::uint32_t m_editorHeight{0};
};

ClapInstance* self(const clap_host_t* host) {
    return static_cast<ClapInstance*>(host->host_data);
}

const void* HostCallbacks::getExtension(const clap_host_t* /*host*/, const char* id) {
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) {
        return &kHostLatency;
    }
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &kHostParams;
    }
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
        return &kHostState;
    }
    if (std::strcmp(id, CLAP_EXT_LOG) == 0) {
        return &kHostLog;
    }
    if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0) {
        return &kHostThreadCheck;
    }
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
        return &kHostGui;
    }
    return nullptr;
}
void HostCallbacks::requestRestart(const clap_host_t* host) {
    self(host)->onRestart();
}
void HostCallbacks::requestProcess(const clap_host_t* /*host*/) {}
void HostCallbacks::requestCallback(const clap_host_t* host) {
    self(host)->onCallback();
}
void HostCallbacks::latencyChanged(const clap_host_t* host) {
    self(host)->onLatency();
}
void HostCallbacks::paramsRescan(const clap_host_t* /*host*/, clap_param_rescan_flags /*flags*/) {}
void HostCallbacks::paramsClear(const clap_host_t* /*host*/, clap_id /*id*/,
                                clap_param_clear_flags /*flags*/) {}
void HostCallbacks::paramsFlush(const clap_host_t* host) {
    self(host)->onFlush();
}
void HostCallbacks::stateDirty(const clap_host_t* /*host*/) {}
void HostCallbacks::log(const clap_host_t* /*host*/, clap_log_severity severity,
                        const char* message) {
    if (severity >= CLAP_LOG_WARNING && message != nullptr) {
        std::fprintf(stderr, "plugin: %s\n", message);
    }
}
bool HostCallbacks::isMainThread(const clap_host_t* host) {
    return self(host)->onMainThread();
}
bool HostCallbacks::isAudioThread(const clap_host_t* host) {
    return !self(host)->onMainThread();
}
void HostCallbacks::guiResizeHintsChanged(const clap_host_t* /*host*/) {}
bool HostCallbacks::guiRequestResize(const clap_host_t* host, uint32_t width, uint32_t height) {
    self(host)->onResize(width, height);
    return true;
}
bool HostCallbacks::guiRequestShow(const clap_host_t* /*host*/) {
    return true;
}
bool HostCallbacks::guiRequestHide(const clap_host_t* /*host*/) {
    return true;
}
void HostCallbacks::guiClosed(const clap_host_t* /*host*/, bool /*destroyed*/) {}

class ClapModule final : public Module {
public:
    ClapModule(std::shared_ptr<bridge::Library> library, const clap_plugin_entry* entry,
               std::string path)
        : m_library(std::move(library)), m_entry(entry), m_path(std::move(path)) {}

    ~ClapModule() override {
        m_entry->deinit();
    }
    ClapModule(const ClapModule&) = delete;
    ClapModule& operator=(const ClapModule&) = delete;
    ClapModule(ClapModule&&) = delete;
    ClapModule& operator=(ClapModule&&) = delete;

    bool init(std::string& error) {
        if (!m_entry->init(m_path.c_str())) {
            error = "clap_entry.init failed";
            return false;
        }
        m_factory =
            static_cast<const clap_plugin_factory*>(m_entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (m_factory == nullptr) {
            m_entry->deinit();
            error = "the file has no plugin factory";
            return false;
        }
        return true;
    }

    std::vector<PluginDescriptor> scan(std::string& error) override {
        std::vector<PluginDescriptor> out;
        const uint32_t count = m_factory->get_plugin_count(m_factory);
        for (uint32_t i = 0; i < count; ++i) {
            const clap_plugin_descriptor* desc = m_factory->get_plugin_descriptor(m_factory, i);
            if (desc == nullptr || desc->id == nullptr) {
                continue;
            }
            std::string why;
            auto instance = create(desc->id, why);
            if (!instance) {
                error += (error.empty() ? "" : "; ") + why;
                continue;
            }
            std::string activateError;
            // Latency is only reported while active.
            if (instance->activate(48000.0, 512, activateError)) {
                out.push_back(instance->describe());
                instance->deactivate();
            } else {
                out.push_back(instance->describe());
            }
        }
        return out;
    }

    std::unique_ptr<Instance> create(const std::string& uid, std::string& error) override {
        auto instance = std::make_unique<ClapInstance>(m_library, m_path);
        if (!instance->create(m_factory, uid, error)) {
            return nullptr;
        }
        instance->readPorts();
        return instance;
    }

private:
    std::shared_ptr<bridge::Library> m_library;
    const clap_plugin_entry* m_entry;
    std::string m_path;
    const clap_plugin_factory* m_factory{nullptr};
};

} // namespace

std::unique_ptr<Module> loadClap(const std::string& path, std::string& error) {
    auto library = std::make_shared<bridge::Library>();
    if (!library->load(path, error)) {
        return nullptr;
    }
    const auto* entry = static_cast<const clap_plugin_entry*>(library->symbol("clap_entry"));
    if (entry == nullptr) {
        error = "not a CLAP plugin: no clap_entry";
        return nullptr;
    }
    if (!clap_version_is_compatible(entry->clap_version)) {
        error = "incompatible CLAP version";
        return nullptr;
    }
    auto module = std::make_unique<ClapModule>(library, entry, path);
    if (!module->init(error)) {
        return nullptr;
    }
    return module;
}

} // namespace adx::plugin::clap

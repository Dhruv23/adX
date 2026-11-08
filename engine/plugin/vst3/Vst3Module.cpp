// adx-thread: main
#include "engine/plugin/vst3/Vst3Module.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "engine/plugin/Bridge/EditorWindow.h"

#if defined(_MSC_VER)
#    pragma warning(push, 0)
#endif
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

namespace adx::plugin::vst3 {

namespace {

namespace st = Steinberg;
namespace vst = Steinberg::Vst;

/// "AV3S", then the component's state and the controller's, each length-prefixed: a
/// VST3 plugin's state is two blobs, and a project keeps them as one.
constexpr std::array<std::uint8_t, 4> kStateMagic{'A', 'V', '3', 'S'};

std::string fromTChar(const vst::TChar* text) {
    return Steinberg::Vst::StringConvert::convert(text);
}

/// One host application object for the whole child, as the SDK expects.
vst::HostApplication& hostApplication() {
    static st::IPtr<vst::HostApplication> const kHost = st::owned(new vst::HostApplication());
    return *kHost;
}

class Vst3Instance;

class ComponentHandler final : public st::U::Implements<st::U::Directly<vst::IComponentHandler>> {
public:
    explicit ComponentHandler(Vst3Instance& owner) : m_owner(owner) {}
    st::tresult PLUGIN_API beginEdit(vst::ParamID /*id*/) override {
        return st::kResultOk;
    }
    st::tresult PLUGIN_API performEdit(vst::ParamID id, vst::ParamValue value) override;
    st::tresult PLUGIN_API endEdit(vst::ParamID /*id*/) override {
        return st::kResultOk;
    }
    st::tresult PLUGIN_API restartComponent(st::int32 flags) override;

private:
    Vst3Instance& m_owner;
};

class PlugFrame final : public st::U::Implements<st::U::Directly<st::IPlugFrame>> {
public:
    explicit PlugFrame(bridge::EditorWindow& window) : m_window(window) {}
    st::tresult PLUGIN_API resizeView(st::IPlugView* view, st::ViewRect* size) override {
        if (view == nullptr || size == nullptr) {
            return st::kInvalidArgument;
        }
        m_window.resize(static_cast<std::uint32_t>(size->getWidth()),
                        static_cast<std::uint32_t>(size->getHeight()));
        view->onSize(size);
        return st::kResultOk;
    }

private:
    bridge::EditorWindow& m_window;
};

class Vst3Instance final : public Instance {
public:
    Vst3Instance(VST3::Hosting::Module::Ptr module, VST3::Hosting::ClassInfo info, std::string path)
        : m_module(std::move(module)), m_info(std::move(info)), m_path(std::move(path)) {}

    ~Vst3Instance() override {
        closeEditor();
        deactivate();
        if ((m_componentPoint != nullptr) && (m_controllerPoint != nullptr)) {
            m_componentPoint->disconnect(m_controllerPoint);
            m_controllerPoint->disconnect(m_componentPoint);
        }
        if (m_controller != nullptr) {
            m_controller->setComponentHandler(nullptr);
            if (!m_singleComponent) {
                m_controller->terminate();
            }
        }
        if (m_component != nullptr) {
            m_component->terminate();
        }
    }
    Vst3Instance(const Vst3Instance&) = delete;
    Vst3Instance& operator=(const Vst3Instance&) = delete;
    Vst3Instance(Vst3Instance&&) = delete;
    Vst3Instance& operator=(Vst3Instance&&) = delete;

    bool create(std::string& error) {
        const auto& factory = m_module->getFactory();
        m_component = factory.createInstance<vst::IComponent>(m_info.ID());
        if (m_component == nullptr) {
            error = "the factory could not create the component";
            return false;
        }
        if (m_component->initialize(&hostApplication()) != st::kResultOk) {
            error = "the component failed to initialise";
            return false;
        }
        m_controller = st::U::cast<vst::IEditController>(m_component);
        m_singleComponent = static_cast<bool>(m_controller);
        if (m_controller == nullptr) {
            st::TUID cid{};
            if (m_component->getControllerClassId(cid) == st::kResultOk) {
                m_controller = factory.createInstance<vst::IEditController>(VST3::UID(cid));
                if ((m_controller != nullptr) &&
                    m_controller->initialize(&hostApplication()) != st::kResultOk) {
                    m_controller = nullptr;
                }
            }
        }
        m_processor = st::U::cast<vst::IAudioProcessor>(m_component);
        if (m_processor == nullptr) {
            error = "the component is not an audio processor";
            return false;
        }
        if (m_controller != nullptr) {
            m_handler = st::owned(new ComponentHandler(*this));
            m_controller->setComponentHandler(m_handler);
            if (!m_singleComponent) {
                m_componentPoint = st::U::cast<vst::IConnectionPoint>(m_component);
                m_controllerPoint = st::U::cast<vst::IConnectionPoint>(m_controller);
                if ((m_componentPoint != nullptr) && (m_controllerPoint != nullptr)) {
                    m_componentPoint->connect(m_controllerPoint);
                    m_controllerPoint->connect(m_componentPoint);
                }
                // The controller starts from what the component holds.
                st::MemoryStream stream;
                if (m_component->getState(&stream) == st::kResultOk) {
                    stream.seek(0, st::IBStream::kIBSeekSet, nullptr);
                    m_controller->setComponentState(&stream);
                }
            }
        }
        readBuses();
        return true;
    }

    bool activate(double sampleRate, std::uint32_t maxFrames, std::string& error) override {
        deactivate();
        m_sampleRate = sampleRate;
        m_maxFrames = maxFrames;
        vst::ProcessSetup setup{.processMode = vst::kRealtime,
                                .symbolicSampleSize = vst::kSample32,
                                .maxSamplesPerBlock = static_cast<st::int32>(maxFrames),
                                .sampleRate = sampleRate};
        if (m_processor->setupProcessing(setup) != st::kResultOk) {
            error = "the plugin refused the processing setup";
            return false;
        }
        std::array<vst::SpeakerArrangement, 1> inArr{vst::SpeakerArr::kStereo};
        std::array<vst::SpeakerArrangement, 1> outArr{vst::SpeakerArr::kStereo};
        static_cast<void>(m_processor->setBusArrangements(m_inputs > 0 ? inArr.data() : nullptr,
                                                          m_inputs > 0 ? 1 : 0, outArr.data(), 1));
        if (m_component->getBusCount(vst::kAudio, vst::kInput) > 0) {
            m_component->activateBus(vst::kAudio, vst::kInput, 0, 1U);
        }
        if (m_component->getBusCount(vst::kAudio, vst::kOutput) > 0) {
            m_component->activateBus(vst::kAudio, vst::kOutput, 0, 1U);
        }
        if (m_component->getBusCount(vst::kEvent, vst::kInput) > 0) {
            m_component->activateBus(vst::kEvent, vst::kInput, 0, 1U);
        }
        if (m_component->setActive(1U) != st::kResultOk) {
            error = "the plugin refused to activate";
            return false;
        }
        m_data.prepare(*m_component, 0, vst::kSample32);
        m_inChanges.setMaxParameters(static_cast<st::int32>(bridge::kMaxParamChanges));
        m_outChanges.setMaxParameters(static_cast<st::int32>(bridge::kMaxOutParams));
        m_events.setMaxSize(static_cast<st::int32>(bridge::kMaxEvents));
        for (auto& channel : m_silence) {
            channel.assign(maxFrames, 0.0F);
        }
        for (auto& channel : m_scratch) {
            channel.assign(maxFrames, 0.0F);
        }
        m_processor->setProcessing(1U);
        m_active = true;
        return true;
    }

    void deactivate() override {
        if (!m_active) {
            return;
        }
        m_processor->setProcessing(0U);
        m_component->setActive(0U);
        m_data.unprepare();
        m_active = false;
    }

    bool restart(std::string& error) override {
        m_restart.store(false);
        return activate(m_sampleRate, m_maxFrames, error);
    }

    void process(ProcessIo& io) noexcept override {
        if (!m_active) {
            return;
        }
        m_inChanges.clearQueue();
        m_outChanges.clearQueue();
        m_events.clear();
        for (const bridge::WireParam& p : io.params) {
            st::int32 index = 0;
            if (auto* queue = m_inChanges.addParameterData(p.id, index)) {
                st::int32 point = 0;
                queue->addPoint(static_cast<st::int32>(p.offset), p.value, point);
            }
        }
        // Edits made in the plugin's own editor reach its processor here (the host's job
        // in VST3), and were already echoed to adX by performEdit.
        {
            const std::scoped_lock lock(m_editMutex);
            for (const auto& [id, value] : m_edits) {
                st::int32 index = 0;
                if (auto* queue = m_inChanges.addParameterData(id, index)) {
                    st::int32 point = 0;
                    queue->addPoint(0, value, point);
                }
            }
            for (const auto& [id, value] : m_edits) {
                if (io.outCount < io.outParams.size()) {
                    // performEdit is the editor's: a user's change by definition.
                    io.outParams[io.outCount++] = bridge::WireParam{.offset = 0,
                                                                    .id = id,
                                                                    .value = value,
                                                                    .flags = bridge::kParamGesture,
                                                                    .reserved = 0};
                }
            }
            m_edits.clear();
        }
        for (const bridge::WireEvent& e : io.events) {
            vst::Event event{};
            event.busIndex = 0;
            event.sampleOffset = static_cast<st::int32>(e.offset);
            if (e.kind == bridge::WireEventKind::NoteOn) {
                event.type = vst::Event::kNoteOnEvent;
                event.noteOn.channel = e.channel;
                event.noteOn.pitch = e.key;
                event.noteOn.velocity = static_cast<float>(e.velocity) / 127.0F;
                event.noteOn.noteId = e.noteId;
                m_events.addEvent(event);
            } else if (e.kind == bridge::WireEventKind::NoteOff) {
                event.type = vst::Event::kNoteOffEvent;
                event.noteOff.channel = e.channel;
                event.noteOff.pitch = e.key;
                event.noteOff.velocity = static_cast<float>(e.velocity) / 127.0F;
                event.noteOff.noteId = e.noteId;
                m_events.addEvent(event);
            } else if (e.kind == bridge::WireEventKind::Tuning) {
                // VST3's tuning expression: plain semitones = 240 * (normalised - 0.5).
                event.type = vst::Event::kNoteExpressionValueEvent;
                event.noteExpressionValue.typeId = vst::kTuningTypeID;
                event.noteExpressionValue.noteId = e.noteId;
                event.noteExpressionValue.value =
                    std::clamp(0.5 + (static_cast<double>(e.value) / 240.0), 0.0, 1.0);
                m_events.addEvent(event);
            } else if (e.kind == bridge::WireEventKind::AllNotesOff) {
                // VST3 has no wildcard: release every key we have sent on.
                for (int key = 0; key < 128; ++key) {
                    if (m_held.at(static_cast<std::size_t>(key))) {
                        event.type = vst::Event::kNoteOffEvent;
                        event.noteOff.channel = 0;
                        event.noteOff.pitch = static_cast<st::int16>(key);
                        event.noteOff.velocity = 0.0F;
                        event.noteOff.noteId = -1;
                        m_events.addEvent(event);
                    }
                }
                m_held.fill(false);
            }
            if (e.kind == bridge::WireEventKind::NoteOn) {
                m_held.at(e.key & 0x7FU) = true;
            } else if (e.kind == bridge::WireEventKind::NoteOff) {
                m_held.at(e.key & 0x7FU) = false;
            }
        }

        vst::ProcessContext context{};
        context.state = vst::ProcessContext::kTempoValid |
                        vst::ProcessContext::kProjectTimeMusicValid |
                        vst::ProcessContext::kTimeSigValid;
        if ((io.transport.flags & bridge::kPlaying) != 0) {
            context.state |= vst::ProcessContext::kPlaying;
        }
        context.sampleRate = m_sampleRate;
        context.projectTimeSamples = io.transport.samples;
        context.tempo = io.transport.tempo;
        context.projectTimeMusic = io.transport.beats;
        context.timeSigNumerator = io.transport.numerator;
        context.timeSigDenominator = io.transport.denominator;

        for (std::size_t c = 0; c < bridge::kMaxChannels; ++c) {
            const float* in = c < io.inputs.size() && io.inputs[c] != nullptr
                                  ? io.inputs[c]
                                  : m_silence.at(c).data();
            std::copy_n(in, io.frames, m_scratch.at(c).data());
            if (m_inputs > 0) {
                m_data.setChannelBuffer(vst::kInput, 0, static_cast<st::int32>(c),
                                        m_scratch.at(c).data());
            }
            m_data.setChannelBuffer(vst::kOutput, 0, static_cast<st::int32>(c),
                                    c < io.outputs.size() ? io.outputs[c] : m_silence.at(c).data());
        }
        m_data.numSamples = static_cast<st::int32>(io.frames);
        m_data.inputParameterChanges = &m_inChanges;
        m_data.outputParameterChanges = &m_outChanges;
        m_data.inputEvents = &m_events;
        m_data.processContext = &context;
        if (m_processor->process(m_data) != st::kResultOk) {
            for (float* out : io.outputs) {
                std::fill_n(out, io.frames, 0.0F);
            }
            io.status |= bridge::kProcessFailed;
        }
        for (st::int32 i = 0; i < m_outChanges.getParameterCount(); ++i) {
            vst::IParamValueQueue* queue = m_outChanges.getParameterData(i);
            if (queue == nullptr || queue->getPointCount() == 0 ||
                io.outCount >= io.outParams.size()) {
                continue;
            }
            st::int32 offset = 0;
            vst::ParamValue value = 0.0;
            queue->getPoint(queue->getPointCount() - 1, offset, value);
            io.outParams[io.outCount++] =
                bridge::WireParam{.offset = static_cast<std::uint32_t>(offset),
                                  .id = queue->getParameterId(),
                                  .value = value,
                                  .flags = 0,
                                  .reserved = 0};
        }
        if (m_latencyChanged.exchange(false)) {
            io.status |= bridge::kLatencyChanged;
        }
        if (m_restart.load()) {
            io.status |= bridge::kRestartRequested;
        }
    }

    PluginDescriptor describe() override {
        PluginDescriptor d;
        d.id.format = PluginFormat::Vst3;
        d.id.uid = m_info.ID().toString(false);
        d.id.path = m_path;
        d.id.name = m_info.name();
        d.id.version = m_info.version();
        d.vendor = m_info.vendor();
        const auto& subs = m_info.subCategories();
        d.instrument = std::ranges::any_of(subs, [](const std::string& s) {
            return s == "Instrument" || s.starts_with("Instrument");
        });
        d.audioInputs = m_inputs;
        d.audioOutputs = 2;
        d.hasEditor = m_controller != nullptr;
        d.latency = m_active ? m_processor->getLatencySamples() : 0;
        if (m_controller != nullptr) {
            const st::int32 count = m_controller->getParameterCount();
            for (st::int32 i = 0; i < count; ++i) {
                vst::ParameterInfo info{};
                if (m_controller->getParameterInfo(i, info) != st::kResultOk) {
                    continue;
                }
                ParamInfo p;
                p.id = info.id;
                p.name = fromTChar(info.title);
                p.minimum = 0.0;
                p.maximum = 1.0;
                p.defaultValue = info.defaultNormalizedValue;
                p.steps = static_cast<std::uint32_t>(std::max<st::int32>(info.stepCount, 0));
                p.automatable = (info.flags & vst::ParameterInfo::kCanAutomate) != 0 &&
                                (info.flags & vst::ParameterInfo::kIsReadOnly) == 0;
                p.hidden = (info.flags & vst::ParameterInfo::kIsHidden) != 0;
                d.params.push_back(std::move(p));
            }
        }
        assignParamKeys(d.params);
        return d;
    }

    bool paramValue(std::uint32_t id, double& out) override {
        if (m_controller == nullptr) {
            return false;
        }
        out = m_controller->getParamNormalized(id);
        return true;
    }

    bool paramText(std::uint32_t id, double value, std::string& out) override {
        vst::String128 text{};
        if ((m_controller == nullptr) ||
            m_controller->getParamStringByValue(id, value, text) != st::kResultOk) {
            return false;
        }
        out = fromTChar(text);
        return true;
    }

    bool saveState(StateBlob& out) override {
        st::MemoryStream component;
        if (m_component->getState(&component) != st::kResultOk) {
            return false;
        }
        st::MemoryStream controller;
        if ((m_controller != nullptr) && !m_singleComponent) {
            static_cast<void>(m_controller->getState(&controller));
        }
        out.assign(kStateMagic.begin(), kStateMagic.end());
        appendBlob(out, component);
        appendBlob(out, controller);
        return true;
    }

    bool loadState(std::span<const std::uint8_t> bytes) override {
        if (bytes.size() < kStateMagic.size() + 8 ||
            !std::equal(kStateMagic.begin(), kStateMagic.end(), bytes.begin())) {
            return false;
        }
        std::size_t at = kStateMagic.size();
        std::span<const std::uint8_t> component;
        std::span<const std::uint8_t> controller;
        if (!readBlob(bytes, at, component) || !readBlob(bytes, at, controller)) {
            return false;
        }
        st::MemoryStream componentStream(const_cast<std::uint8_t*>(component.data()),
                                         static_cast<st::TSize>(component.size()));
        if (m_component->setState(&componentStream) != st::kResultOk) {
            return false;
        }
        if (m_controller != nullptr) {
            componentStream.seek(0, st::IBStream::kIBSeekSet, nullptr);
            m_controller->setComponentState(&componentStream);
            if (!m_singleComponent && !controller.empty()) {
                st::MemoryStream controllerStream(const_cast<std::uint8_t*>(controller.data()),
                                                  static_cast<st::TSize>(controller.size()));
                m_controller->setState(&controllerStream);
            }
        }
        return true;
    }

    std::uint32_t latency() override {
        return m_active ? m_processor->getLatencySamples() : 0;
    }

    bool openEditor(std::uint64_t& window, std::uint32_t& width, std::uint32_t& height) override {
        if (m_controller == nullptr) {
            return false;
        }
        if (m_view == nullptr) {
            m_view = st::owned(m_controller->createView(vst::ViewType::kEditor));
            if ((m_view == nullptr) ||
                m_view->isPlatformTypeSupported(st::kPlatformTypeHWND) != st::kResultTrue) {
                m_view = nullptr;
                return false;
            }
            st::ViewRect rect{};
            m_view->getSize(&rect);
            const auto w = static_cast<std::uint32_t>(std::max<st::int32>(rect.getWidth(), 100));
            const auto h = static_cast<std::uint32_t>(std::max<st::int32>(rect.getHeight(), 100));
            if (!m_window.create(m_info.name().c_str(), w, h)) {
                m_view = nullptr;
                return false;
            }
            m_frame = st::owned(new PlugFrame(m_window));
            m_view->setFrame(m_frame);
            if (m_view->attached(m_window.handle(), st::kPlatformTypeHWND) != st::kResultOk) {
                m_view->setFrame(nullptr);
                m_view = nullptr;
                m_window.destroy();
                return false;
            }
            m_window.show();
            m_width = w;
            m_height = h;
        }
        window = reinterpret_cast<std::uintptr_t>(m_window.handle());
        width = m_width;
        height = m_height;
        return true;
    }

    void closeEditor() override {
        if (m_view != nullptr) {
            m_view->removed();
            m_view->setFrame(nullptr);
            m_view = nullptr;
            m_window.destroy();
        }
    }

    void onEdit(vst::ParamID id, double value) {
        const std::scoped_lock lock(m_editMutex);
        m_edits.emplace_back(id, value);
    }
    void onRestart(st::int32 flags) {
        if ((flags & vst::kLatencyChanged) != 0) {
            m_latencyChanged.store(true);
            m_restart.store(true);
        }
        if ((flags & vst::kReloadComponent) != 0) {
            m_restart.store(true);
        }
    }

private:
    static void appendBlob(StateBlob& out, st::MemoryStream& stream) {
        const auto size = static_cast<std::uint32_t>(stream.getSize());
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(
                static_cast<std::uint8_t>((size >> static_cast<unsigned>(shift)) & 0xFFU));
        }
        const auto* data = reinterpret_cast<const std::uint8_t*>(stream.getData());
        out.insert(out.end(), data, data + size);
    }

    static bool readBlob(std::span<const std::uint8_t> bytes, std::size_t& at,
                         std::span<const std::uint8_t>& out) {
        if (bytes.size() - at < 4) {
            return false;
        }
        std::uint32_t size = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            size |= static_cast<std::uint32_t>(bytes[at + i]) << (8U * i);
        }
        at += 4;
        if (bytes.size() - at < size) {
            return false;
        }
        out = bytes.subspan(at, size);
        at += size;
        return true;
    }

    void readBuses() {
        m_inputs = 0;
        if (m_component->getBusCount(vst::kAudio, vst::kInput) > 0) {
            vst::BusInfo info{};
            if (m_component->getBusInfo(vst::kAudio, vst::kInput, 0, info) == st::kResultOk) {
                m_inputs = static_cast<std::uint32_t>(
                    std::clamp<st::int32>(info.channelCount, 0, bridge::kMaxChannels));
            }
        }
    }

    VST3::Hosting::Module::Ptr m_module;
    VST3::Hosting::ClassInfo m_info;
    std::string m_path;
    st::IPtr<vst::IComponent> m_component;
    st::IPtr<vst::IEditController> m_controller;
    st::IPtr<vst::IAudioProcessor> m_processor;
    st::IPtr<vst::IConnectionPoint> m_componentPoint;
    st::IPtr<vst::IConnectionPoint> m_controllerPoint;
    st::IPtr<ComponentHandler> m_handler;
    bool m_singleComponent{false};
    std::uint32_t m_inputs{2};
    double m_sampleRate{48000.0};
    std::uint32_t m_maxFrames{4096};
    bool m_active{false};
    vst::HostProcessData m_data;
    vst::ParameterChanges m_inChanges;
    vst::ParameterChanges m_outChanges;
    vst::EventList m_events;
    std::array<std::vector<float>, bridge::kMaxChannels> m_silence;
    std::array<std::vector<float>, bridge::kMaxChannels> m_scratch;
    std::array<bool, 128> m_held{};
    std::mutex m_editMutex;
    std::vector<std::pair<vst::ParamID, double>> m_edits;
    std::atomic<bool> m_latencyChanged{false};
    std::atomic<bool> m_restart{false};
    bridge::EditorWindow m_window;
    st::IPtr<st::IPlugView> m_view;
    st::IPtr<PlugFrame> m_frame;
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
};

st::tresult ComponentHandler::performEdit(vst::ParamID id, vst::ParamValue value) {
    m_owner.onEdit(id, value);
    return st::kResultOk;
}

st::tresult ComponentHandler::restartComponent(st::int32 flags) {
    m_owner.onRestart(flags);
    return st::kResultOk;
}

class Vst3PluginModule final : public Module {
public:
    Vst3PluginModule(VST3::Hosting::Module::Ptr module, std::string path)
        : m_module(std::move(module)), m_path(std::move(path)) {}

    std::vector<PluginDescriptor> scan(std::string& error) override {
        std::vector<PluginDescriptor> out;
        for (const auto& info : m_module->getFactory().classInfos()) {
            if (info.category() != kVstAudioEffectClass) {
                continue;
            }
            std::string why;
            auto instance = create(info.ID().toString(false), why);
            if (!instance) {
                error += (error.empty() ? "" : "; ") + why;
                continue;
            }
            std::string activateError;
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
        for (const auto& info : m_module->getFactory().classInfos()) {
            if (info.category() == kVstAudioEffectClass && info.ID().toString(false) == uid) {
                auto instance = std::make_unique<Vst3Instance>(m_module, info, m_path);
                if (!instance->create(error)) {
                    return nullptr;
                }
                return instance;
            }
        }
        error = "no audio module class " + uid + " in " + m_path;
        return nullptr;
    }

private:
    VST3::Hosting::Module::Ptr m_module;
    std::string m_path;
};

} // namespace

std::unique_ptr<Module> loadVst3(const std::string& path, std::string& error) {
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module) {
        if (error.empty()) {
            error = "not a VST3 module";
        }
        return nullptr;
    }
    return std::make_unique<Vst3PluginModule>(std::move(module), path);
}

} // namespace adx::plugin::vst3

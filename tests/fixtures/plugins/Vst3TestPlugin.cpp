// adx_test_plugin.vst3: the fixture's VST3 build (phase_9.md §4.8). One object per
// instance is component, processor and controller at once - the single-component
// shape - written against pluginterfaces alone. TestCore.h has the behaviour.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#if defined(_MSC_VER)
#    pragma warning(push, 0)
#endif
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/vsttypes.h"
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

#include "TestCore.h"

namespace {

namespace St = Steinberg;
namespace Vst = Steinberg::Vst;

const St::TUID kEffectUid = INLINE_UID(0x41445854, 0x45464658, 0x00000001, 0x00000001);
const St::TUID kSynthUid = INLINE_UID(0x41445854, 0x53594E54, 0x00000001, 0x00000002);

struct ParamDef {
    Vst::ParamID id;
    const char* name;
    double plainMax;
    St::int32 steps;
    double defaultPlain;
};

constexpr std::array<ParamDef, 4> kParams{{
    {adxtest::kGain, "Gain", 2.0, 0, 1.0},
    {adxtest::kMode, "Mode", adxtest::kModeCount - 1, adxtest::kModeCount - 1, 0.0},
    {adxtest::kLatency, "Latency", adxtest::kMaxLatency, adxtest::kMaxLatency, 0.0},
    {adxtest::kSeed, "Seed", 1.0, 0, 0.0},
}};

const ParamDef* findParam(Vst::ParamID id) {
    for (const ParamDef& def : kParams) {
        if (def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

void toTChar(const char* text, Vst::String128 out) {
    std::size_t i = 0;
    for (; text[i] != '\0' && i < 127; ++i) {
        out[i] = static_cast<Vst::TChar>(text[i]);
    }
    out[i] = 0;
}

class TestPlugin final
    : public St::U::Implements<St::U::Directly<Vst::IComponent, Vst::IAudioProcessor,
                                               Vst::IEditController>> {
public:
    explicit TestPlugin(bool instrument) : m_core(instrument), m_instrument(instrument) {}

    // IPluginBase (shared by IComponent and IEditController)
    St::tresult PLUGIN_API initialize(St::FUnknown* /*context*/) override {
        return St::kResultOk;
    }
    St::tresult PLUGIN_API terminate() override {
        return St::kResultOk;
    }

    // IComponent
    St::tresult PLUGIN_API getControllerClassId(St::TUID /*classId*/) override {
        return St::kNotImplemented; // single component: this object is the controller
    }
    St::tresult PLUGIN_API setIoMode(Vst::IoMode /*mode*/) override {
        return St::kResultOk;
    }
    St::int32 PLUGIN_API getBusCount(Vst::MediaType type, Vst::BusDirection dir) override {
        if (type == Vst::kAudio) {
            return dir == Vst::kOutput || !m_instrument ? 1 : 0;
        }
        return type == Vst::kEvent && dir == Vst::kInput && m_instrument ? 1 : 0;
    }
    St::tresult PLUGIN_API getBusInfo(Vst::MediaType type, Vst::BusDirection dir, St::int32 index,
                                      Vst::BusInfo& bus) override {
        if (index != 0 || getBusCount(type, dir) == 0) {
            return St::kInvalidArgument;
        }
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = type == Vst::kAudio ? 2 : 16;
        toTChar(type == Vst::kAudio ? (dir == Vst::kInput ? "In" : "Out") : "Notes", bus.name);
        bus.busType = Vst::kMain;
        bus.flags = Vst::BusInfo::kDefaultActive;
        return St::kResultOk;
    }
    St::tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo& /*in*/, Vst::RoutingInfo& /*out*/) override {
        return St::kNotImplemented;
    }
    St::tresult PLUGIN_API activateBus(Vst::MediaType /*type*/, Vst::BusDirection /*dir*/,
                                       St::int32 /*index*/, St::TBool /*state*/) override {
        return St::kResultOk;
    }
    St::tresult PLUGIN_API setActive(St::TBool state) override {
        if (state != 0) {
            m_core.activate(m_sampleRate, m_maxFrames);
            m_restartSent = false;
        }
        return St::kResultOk;
    }
    St::tresult PLUGIN_API setState(St::IBStream* stream) override {
        std::vector<std::uint8_t> bytes;
        std::array<std::uint8_t, 256> chunk{};
        while (true) {
            St::int32 read = 0;
            if (stream->read(chunk.data(), static_cast<St::int32>(chunk.size()), &read) !=
                    St::kResultOk ||
                read <= 0) {
                break;
            }
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + read);
        }
        return m_core.load(bytes.data(), bytes.size()) ? St::kResultOk : St::kResultFalse;
    }
    St::tresult PLUGIN_API getState(St::IBStream* stream) override {
        const std::vector<std::uint8_t> bytes = m_core.save();
        St::int32 wrote = 0;
        return stream->write(const_cast<std::uint8_t*>(bytes.data()),
                             static_cast<St::int32>(bytes.size()), &wrote);
    }

    // IAudioProcessor
    St::tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* /*inputs*/, St::int32 /*numIns*/,
                                              Vst::SpeakerArrangement* /*outputs*/,
                                              St::int32 /*numOuts*/) override {
        return St::kResultOk;
    }
    St::tresult PLUGIN_API getBusArrangement(Vst::BusDirection /*dir*/, St::int32 /*index*/,
                                             Vst::SpeakerArrangement& arr) override {
        arr = Vst::SpeakerArr::kStereo;
        return St::kResultOk;
    }
    St::tresult PLUGIN_API canProcessSampleSize(St::int32 size) override {
        return size == Vst::kSample32 ? St::kResultTrue : St::kResultFalse;
    }
    St::uint32 PLUGIN_API getLatencySamples() override {
        return m_core.reportedLatency();
    }
    St::tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) override {
        m_sampleRate = setup.sampleRate;
        m_maxFrames = static_cast<std::uint32_t>(setup.maxSamplesPerBlock);
        return St::kResultOk;
    }
    St::tresult PLUGIN_API setProcessing(St::TBool /*state*/) override {
        return St::kResultOk;
    }
    St::tresult PLUGIN_API process(Vst::ProcessData& data) override {
        if (data.inputParameterChanges != nullptr) {
            for (St::int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
                Vst::IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
                St::int32 offset = 0;
                Vst::ParamValue value = 0.0;
                if (queue != nullptr && queue->getPointCount() > 0 &&
                    queue->getPoint(queue->getPointCount() - 1, offset, value) == St::kResultOk) {
                    setNormalized(queue->getParameterId(), value);
                }
            }
        }
        if (data.numSamples <= 0 || data.numOutputs == 0) {
            return St::kResultOk;
        }
        const auto frames = static_cast<std::uint32_t>(data.numSamples);
        std::array<float, 1> none{};
        std::array<const float*, 2> silence{none.data(), none.data()};
        const float* const* in = data.numInputs > 0 && !m_instrument
                                     ? data.inputs[0].channelBuffers32
                                     : silence.data();
        float* const* out = data.outputs[0].channelBuffers32;
        m_core.beginBlock(frames);
        std::uint32_t position = 0;
        const St::int32 count = data.inputEvents != nullptr ? data.inputEvents->getEventCount() : 0;
        for (St::int32 i = 0; i < count; ++i) {
            Vst::Event event{};
            if (data.inputEvents->getEvent(i, event) != St::kResultOk) {
                continue;
            }
            const std::uint32_t at =
                std::min(static_cast<std::uint32_t>(event.sampleOffset), frames);
            if (at > position) {
                m_core.render(in, out, position, at - position);
                position = at;
            }
            if (event.type == Vst::Event::kNoteOnEvent) {
                m_core.noteOn(event.noteOn.pitch, event.noteOn.velocity);
            } else if (event.type == Vst::Event::kNoteOffEvent) {
                m_core.noteOff(event.noteOff.pitch);
            }
        }
        if (position < frames) {
            m_core.render(in, out, position, frames - position);
        }
        const bool restart = m_core.endBlock(out, frames);
        if (restart && !m_restartSent && m_handler != nullptr) {
            m_restartSent = true;
            m_handler->restartComponent(Vst::kLatencyChanged);
        }
        return St::kResultOk;
    }
    St::uint32 PLUGIN_API getTailSamples() override {
        return 0;
    }

    // IEditController
    St::tresult PLUGIN_API setComponentState(St::IBStream* /*state*/) override {
        return St::kResultOk; // the same object: it already has its state
    }
    // setState and getState are IComponent's and IEditController's at once: one object,
    // one state (above).
    St::int32 PLUGIN_API getParameterCount() override {
        return static_cast<St::int32>(kParams.size());
    }
    St::tresult PLUGIN_API getParameterInfo(St::int32 index, Vst::ParameterInfo& info) override {
        if (index < 0 || static_cast<std::size_t>(index) >= kParams.size()) {
            return St::kInvalidArgument;
        }
        const ParamDef& def = kParams.at(static_cast<std::size_t>(index));
        info = Vst::ParameterInfo{};
        info.id = def.id;
        toTChar(def.name, info.title);
        toTChar(def.name, info.shortTitle);
        info.stepCount = def.steps;
        info.defaultNormalizedValue = def.defaultPlain / def.plainMax;
        info.flags = Vst::ParameterInfo::kCanAutomate;
        return St::kResultOk;
    }
    St::tresult PLUGIN_API getParamStringByValue(Vst::ParamID id, Vst::ParamValue value,
                                                 Vst::String128 string) override {
        const ParamDef* def = findParam(id);
        if (def == nullptr) {
            return St::kInvalidArgument;
        }
        std::array<char, 64> text{};
        std::snprintf(text.data(), text.size(), "%g", value * def->plainMax);
        toTChar(text.data(), string);
        return St::kResultOk;
    }
    St::tresult PLUGIN_API getParamValueByString(Vst::ParamID /*id*/, Vst::TChar* /*string*/,
                                                 Vst::ParamValue& /*value*/) override {
        return St::kNotImplemented;
    }
    Vst::ParamValue PLUGIN_API normalizedParamToPlain(Vst::ParamID id, Vst::ParamValue value) override {
        const ParamDef* def = findParam(id);
        return def == nullptr ? value : value * def->plainMax;
    }
    Vst::ParamValue PLUGIN_API plainParamToNormalized(Vst::ParamID id, Vst::ParamValue value) override {
        const ParamDef* def = findParam(id);
        return def == nullptr ? value : value / def->plainMax;
    }
    Vst::ParamValue PLUGIN_API getParamNormalized(Vst::ParamID id) override {
        const ParamDef* def = findParam(id);
        return def == nullptr ? 0.0 : m_core.param(id) / def->plainMax;
    }
    St::tresult PLUGIN_API setParamNormalized(Vst::ParamID id, Vst::ParamValue value) override {
        setNormalized(id, value);
        return St::kResultOk;
    }
    St::tresult PLUGIN_API setComponentHandler(Vst::IComponentHandler* handler) override {
        m_handler = handler;
        return St::kResultOk;
    }
    St::IPlugView* PLUGIN_API createView(St::FIDString /*name*/) override {
        return nullptr;
    }

private:
    void setNormalized(Vst::ParamID id, double value) {
        if (const ParamDef* def = findParam(id)) {
            m_core.setParam(id, value * def->plainMax);
        }
    }

    adxtest::Core m_core;
    bool m_instrument;
    double m_sampleRate{48000.0};
    std::uint32_t m_maxFrames{4096};
    Vst::IComponentHandler* m_handler{nullptr};
    bool m_restartSent{false};
};

class Factory final
    : public St::U::ImplementsNonDestroyable<St::U::Directly<St::IPluginFactory2>,
                                             St::U::Indirectly<St::IPluginFactory>> {
public:
    St::tresult PLUGIN_API getFactoryInfo(St::PFactoryInfo* info) override {
        *info = St::PFactoryInfo("adX", "", "", St::PFactoryInfo::kUnicode);
        return St::kResultOk;
    }
    St::int32 PLUGIN_API countClasses() override {
        return 2;
    }
    St::tresult PLUGIN_API getClassInfo(St::int32 index, St::PClassInfo* info) override {
        if (index < 0 || index > 1) {
            return St::kInvalidArgument;
        }
        *info = St::PClassInfo(index == 0 ? kEffectUid : kSynthUid, St::PClassInfo::kManyInstances,
                               kVstAudioEffectClass, index == 0 ? "adX Test Effect" : "adX Test Synth");
        return St::kResultOk;
    }
    St::tresult PLUGIN_API getClassInfo2(St::int32 index, St::PClassInfo2* info) override {
        if (index < 0 || index > 1) {
            return St::kInvalidArgument;
        }
        *info = St::PClassInfo2(index == 0 ? kEffectUid : kSynthUid, St::PClassInfo::kManyInstances,
                                kVstAudioEffectClass,
                                index == 0 ? "adX Test Effect" : "adX Test Synth", 0,
                                index == 0 ? "Fx" : "Instrument|Synth", "adX", "1.0.0",
                                "VST 3.8.0");
        return St::kResultOk;
    }
    St::tresult PLUGIN_API createInstance(St::FIDString cid, St::FIDString iid, void** obj) override {
        const bool effect = St::FUnknownPrivate::iidEqual(cid, kEffectUid);
        const bool synth = St::FUnknownPrivate::iidEqual(cid, kSynthUid);
        if (!effect && !synth) {
            *obj = nullptr;
            return St::kNoInterface;
        }
        auto* plugin = new TestPlugin(synth);
        const St::tresult result = plugin->queryInterface(iid, obj);
        plugin->release();
        return result;
    }
};

Factory& factory() {
    static Factory instance;
    return instance;
}

} // namespace

extern "C" {

SMTG_EXPORT_SYMBOL bool InitDll() {
    adxtest::misbehaveAtLoad();
    return true;
}

SMTG_EXPORT_SYMBOL bool ExitDll() {
    return true;
}

SMTG_EXPORT_SYMBOL St::IPluginFactory* PLUGIN_API GetPluginFactory() {
    return &factory();
}
}

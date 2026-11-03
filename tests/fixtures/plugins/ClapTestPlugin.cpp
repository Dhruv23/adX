// adx_test_plugin.clap: the fixture's CLAP build (phase_9.md §4.8). TestCore.h has
// the behaviour; this is CLAP's plumbing around it.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include <clap/clap.h>

#include "TestCore.h"

namespace {

using adxtest::Core;

struct ParamDef {
    clap_id id;
    const char* name;
    double min;
    double max;
    double def;
    bool stepped;
};

constexpr std::array<ParamDef, 4> kParams{{
    {adxtest::kGain, "Gain", 0.0, 2.0, 1.0, false},
    {adxtest::kMode, "Mode", 0.0, adxtest::kModeCount - 1, 0.0, true},
    {adxtest::kLatency, "Latency", 0.0, adxtest::kMaxLatency, 0.0, true},
    {adxtest::kSeed, "Seed", 0.0, 1.0, 0.0, false},
}};

constexpr std::array<const char*, 3> kEffectFeatures{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                                     CLAP_PLUGIN_FEATURE_UTILITY, nullptr};
constexpr std::array<const char*, 3> kSynthFeatures{CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                                    CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};

const clap_plugin_descriptor kEffectDescriptor{
    CLAP_VERSION, "com.adx.test.effect", "adX Test Effect", "adX", "", "", "", "1.0.0",
    "A gain and a delay with selectable misbehaviour", kEffectFeatures.data()};
const clap_plugin_descriptor kSynthDescriptor{
    CLAP_VERSION, "com.adx.test.synth", "adX Test Synth", "adX", "", "", "", "1.0.0",
    "A sine synth with selectable misbehaviour", kSynthFeatures.data()};

struct Plugin {
    clap_plugin plugin{};
    const clap_host* host{nullptr};
    Core core;
    bool restartPending{false};

    Plugin(const clap_host* h, bool instrument) : host(h), core(instrument) {}
};

Plugin* self(const clap_plugin* p) {
    return static_cast<Plugin*>(p->plugin_data);
}

// --- params ------------------------------------------------------------------------

uint32_t paramsCount(const clap_plugin* /*p*/) {
    return static_cast<uint32_t>(kParams.size());
}

bool paramsInfo(const clap_plugin* /*p*/, uint32_t index, clap_param_info* info) {
    if (index >= kParams.size()) {
        return false;
    }
    const ParamDef& def = kParams.at(index);
    *info = clap_param_info{};
    info->id = def.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | (def.stepped ? CLAP_PARAM_IS_STEPPED : 0U);
    std::snprintf(info->name, sizeof info->name, "%s", def.name);
    info->min_value = def.min;
    info->max_value = def.max;
    info->default_value = def.def;
    return true;
}

bool paramsValue(const clap_plugin* p, clap_id id, double* out) {
    *out = self(p)->core.param(id);
    return true;
}

bool paramsText(const clap_plugin* /*p*/, clap_id id, double value, char* out, uint32_t size) {
    if (id == adxtest::kGain) {
        std::snprintf(out, size, "%.2f x", value);
    } else {
        std::snprintf(out, size, "%g", value);
    }
    return true;
}

bool paramsFromText(const clap_plugin* /*p*/, clap_id /*id*/, const char* text, double* out) {
    *out = std::atof(text);
    return true;
}

void applyEvent(Plugin& plugin, const clap_event_header* header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
        return;
    }
    if (header->type == CLAP_EVENT_PARAM_VALUE) {
        const auto* value = reinterpret_cast<const clap_event_param_value*>(header);
        plugin.core.setParam(value->param_id, value->value);
    } else if (header->type == CLAP_EVENT_NOTE_ON) {
        const auto* note = reinterpret_cast<const clap_event_note*>(header);
        plugin.core.noteOn(note->key, static_cast<float>(note->velocity));
    } else if (header->type == CLAP_EVENT_NOTE_OFF) {
        const auto* note = reinterpret_cast<const clap_event_note*>(header);
        plugin.core.noteOff(note->key);
    }
}

void applyEvents(Plugin& plugin, const clap_input_events* events) {
    const uint32_t count = events->size(events);
    for (uint32_t i = 0; i < count; ++i) {
        applyEvent(plugin, events->get(events, i));
    }
}

void paramsFlush(const clap_plugin* p, const clap_input_events* in, const clap_output_events* /*out*/) {
    applyEvents(*self(p), in);
}

constexpr clap_plugin_params kParamsExt{paramsCount, paramsInfo, paramsValue, paramsText,
                                        paramsFromText, paramsFlush};

// --- state ---------------------------------------------------------------------------

bool stateSave(const clap_plugin* p, const clap_ostream* stream) {
    const std::vector<std::uint8_t> bytes = self(p)->core.save();
    return stream->write(stream, bytes.data(), bytes.size()) == static_cast<int64_t>(bytes.size());
}

bool stateLoad(const clap_plugin* p, const clap_istream* stream) {
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 256> chunk{};
    while (true) {
        const int64_t got = stream->read(stream, chunk.data(), chunk.size());
        if (got < 0) {
            return false;
        }
        if (got == 0) {
            break;
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + got);
    }
    return self(p)->core.load(bytes.data(), bytes.size());
}

constexpr clap_plugin_state kStateExt{stateSave, stateLoad};

// --- latency, ports ---------------------------------------------------------------------

uint32_t latencyGet(const clap_plugin* p) {
    return self(p)->core.reportedLatency();
}

constexpr clap_plugin_latency kLatencyExt{latencyGet};

uint32_t portsCount(const clap_plugin* p, bool isInput) {
    return isInput && self(p)->core.instrument() ? 0 : 1;
}

bool portsGet(const clap_plugin* p, uint32_t index, bool isInput, clap_audio_port_info* info) {
    if (index != 0 || (isInput && self(p)->core.instrument())) {
        return false;
    }
    *info = clap_audio_port_info{};
    info->id = isInput ? 0 : 1;
    std::snprintf(info->name, sizeof info->name, "%s", isInput ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

constexpr clap_plugin_audio_ports kPortsExt{portsCount, portsGet};

uint32_t notePortsCount(const clap_plugin* p, bool isInput) {
    return isInput && self(p)->core.instrument() ? 1 : 0;
}

bool notePortsGet(const clap_plugin* /*p*/, uint32_t index, bool isInput, clap_note_port_info* info) {
    if (index != 0 || !isInput) {
        return false;
    }
    *info = clap_note_port_info{};
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof info->name, "Notes");
    return true;
}

constexpr clap_plugin_note_ports kNotePortsExt{notePortsCount, notePortsGet};

// --- the plugin ----------------------------------------------------------------------

bool init(const clap_plugin* /*p*/) {
    return true;
}

void destroy(const clap_plugin* p) {
    delete self(p);
}

bool activate(const clap_plugin* p, double sampleRate, uint32_t /*min*/, uint32_t max) {
    self(p)->core.activate(sampleRate, max);
    self(p)->restartPending = false;
    return true;
}

void deactivate(const clap_plugin* /*p*/) {}

bool startProcessing(const clap_plugin* /*p*/) {
    return true;
}

void stopProcessing(const clap_plugin* /*p*/) {}

void reset(const clap_plugin* /*p*/) {}

clap_process_status process(const clap_plugin* p, const clap_process* process) {
    Plugin& plugin = *self(p);
    std::array<float, 1> none{};
    std::array<const float*, 2> silence{none.data(), none.data()};
    const float* const* in = process->audio_inputs_count > 0
                                 ? process->audio_inputs[0].data32
                                 : silence.data();
    float* const* out = process->audio_outputs[0].data32;
    // Parameters first, as a whole: the mode decides what this block does. Notes on
    // their own samples.
    const clap_input_events* events = process->in_events;
    const uint32_t count = events->size(events);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_event_header* header = events->get(events, i);
        if (header->type == CLAP_EVENT_PARAM_VALUE) {
            applyEvent(plugin, header);
        }
    }
    plugin.core.beginBlock(process->frames_count);
    uint32_t position = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const clap_event_header* header = events->get(events, i);
        if (header->type == CLAP_EVENT_PARAM_VALUE) {
            continue;
        }
        const uint32_t at = std::min(header->time, process->frames_count);
        if (at > position) {
            plugin.core.render(in, out, position, at - position);
            position = at;
        }
        applyEvent(plugin, header);
    }
    if (position < process->frames_count) {
        plugin.core.render(in, out, position, process->frames_count - position);
    }
    if (plugin.core.endBlock(out, process->frames_count) && !plugin.restartPending) {
        plugin.restartPending = true;
        plugin.host->request_restart(plugin.host);
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* getExtension(const clap_plugin* p, const char* id) {
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &kParamsExt;
    }
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
        return &kStateExt;
    }
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0 && !self(p)->core.instrument()) {
        return &kLatencyExt;
    }
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &kPortsExt;
    }
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) {
        return &kNotePortsExt;
    }
    return nullptr;
}

void onMainThread(const clap_plugin* /*p*/) {}

const clap_plugin* create(const clap_host* host, const clap_plugin_descriptor* descriptor) {
    auto* plugin = new Plugin(host, descriptor == &kSynthDescriptor);
    plugin->plugin.desc = descriptor;
    plugin->plugin.plugin_data = plugin;
    plugin->plugin.init = init;
    plugin->plugin.destroy = destroy;
    plugin->plugin.activate = activate;
    plugin->plugin.deactivate = deactivate;
    plugin->plugin.start_processing = startProcessing;
    plugin->plugin.stop_processing = stopProcessing;
    plugin->plugin.reset = reset;
    plugin->plugin.process = process;
    plugin->plugin.get_extension = getExtension;
    plugin->plugin.on_main_thread = onMainThread;
    return &plugin->plugin;
}

// --- the factory and the entry ---------------------------------------------------------

uint32_t factoryCount(const clap_plugin_factory* /*f*/) {
    return 2;
}

const clap_plugin_descriptor* factoryDescriptor(const clap_plugin_factory* /*f*/, uint32_t index) {
    return index == 0 ? &kEffectDescriptor : index == 1 ? &kSynthDescriptor : nullptr;
}

const clap_plugin* factoryCreate(const clap_plugin_factory* /*f*/, const clap_host* host,
                                 const char* id) {
    if (std::strcmp(id, kEffectDescriptor.id) == 0) {
        return create(host, &kEffectDescriptor);
    }
    if (std::strcmp(id, kSynthDescriptor.id) == 0) {
        return create(host, &kSynthDescriptor);
    }
    return nullptr;
}

constexpr clap_plugin_factory kFactory{factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char* /*path*/) {
    adxtest::misbehaveAtLoad();
    return true;
}

void entryDeinit() {}

const void* entryFactory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" {
CLAP_EXPORT const clap_plugin_entry clap_entry{CLAP_VERSION, entryInit, entryDeinit, entryFactory};
}

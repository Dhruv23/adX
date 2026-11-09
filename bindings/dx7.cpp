// The DX7 codec for the librarian and the FM-1 panel (phase_11.5.md §4.4, §4.6): voices
// travel as 156-int lists (the voice bytes, VCED order, operator on/off last).

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "engine/instruments/dx7/Dx7Params.h"
#include "engine/midi/dx7/Dx7Codec.h"
#include "engine/midi/dx7/Dx7ParamChange.h"

namespace py = pybind11;
using adx::dx7::Dx7Voice;

namespace {

Dx7Voice toVoice(const std::vector<int>& bytes) {
    if (bytes.size() != adx::dx7::kVoiceBytes && bytes.size() != adx::dx7::kVcedBytes) {
        throw std::invalid_argument("a DX7 voice is 155 or 156 bytes");
    }
    Dx7Voice voice = Dx7Voice::init();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        voice.bytes[i] = static_cast<std::uint8_t>(bytes[i] & 0x7F);
    }
    return voice;
}

std::vector<int> fromVoice(const Dx7Voice& voice) {
    return {voice.bytes.begin(), voice.bytes.end()};
}

py::bytes asBytes(const std::vector<std::uint8_t>& data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

std::vector<std::uint8_t> fromBytes(const py::bytes& data) {
    const std::string text = data;
    return {text.begin(), text.end()};
}

} // namespace

namespace adx::bindings {

void registerDx7(py::module_& m) {
    // GIL: trivial - at most a few banks of bytes
    m.def(
        "dx7_read",
        [](const py::bytes& data) {
            const auto result = adx::dx7::readVoices(fromBytes(data));
            py::list voices;
            for (const Dx7Voice& voice : result.voices) {
                voices.append(fromVoice(voice));
            }
            py::dict out;
            out["voices"] = voices;
            out["messages"] = result.messages;
            out["bad_checksums"] = result.badChecksums;
            out["clamped"] = result.clamped;
            out["skipped"] = result.skipped;
            return out;
        },
        py::arg("data"),
        "Every DX7 voice in .syx data or a dump: voices (156-int lists), messages, "
        "bad_checksums, clamped, skipped.");
    // GIL: trivial
    m.def(
        "dx7_single_sysex",
        [](const std::vector<int>& voice, int channel) {
            return asBytes(
                adx::dx7::singleVoiceSysex(toVoice(voice), static_cast<std::uint8_t>(channel)));
        },
        py::arg("voice"), py::arg("channel") = 0);
    // GIL: trivial
    m.def(
        "dx7_bank_sysex",
        [](const std::vector<std::vector<int>>& voices, int channel) {
            std::vector<Dx7Voice> list;
            list.reserve(voices.size());
            for (const auto& v : voices) {
                list.push_back(toVoice(v));
            }
            return asBytes(adx::dx7::bankSysex(list, static_cast<std::uint8_t>(channel)));
        },
        py::arg("voices"), py::arg("channel") = 0);
    // GIL: trivial
    m.def(
        "dx7_dump_request",
        [](bool bank, int channel) {
            return asBytes(adx::dx7::dumpRequest(bank, static_cast<std::uint8_t>(channel)));
        },
        py::arg("bank"), py::arg("channel") = 0);
    // GIL: trivial
    m.def(
        "dx7_param_change",
        [](std::uint32_t param, int value, int channel) -> py::object {
            const auto message =
                adx::dx7::encodeParamChange({.param = param,
                                             .value = static_cast<std::uint8_t>(value & 0x7F),
                                             .channel = static_cast<std::uint8_t>(channel)});
            if (!message) {
                return py::none();
            }
            return asBytes(std::vector<std::uint8_t>(message->begin(), message->end()));
        },
        py::arg("param"), py::arg("value"), py::arg("channel") = 0,
        "The DX7 parameter-change message for dx7 parameter `param` (None: no address).");
    // GIL: trivial
    m.def(
        "dx7_decode_param_change",
        [](const py::bytes& data) -> py::object {
            const auto change = adx::dx7::decodeParamChange(fromBytes(data));
            if (!change) {
                return py::none();
            }
            return py::make_tuple(change->param, change->value, change->channel);
        },
        py::arg("message"));
    // GIL: trivial
    m.def(
        "dx7_voice_to_params",
        [](const std::vector<int>& voice) {
            std::array<float, adx::dx7::kVoiceBytes> params{};
            adx::dx7::paramsFromVoice(toVoice(voice), params);
            return std::vector<float>(params.begin(), params.end());
        },
        py::arg("voice"), "Voice bytes -> the first 156 dx7 parameter values.");
    // GIL: trivial
    m.def(
        "dx7_params_to_voice",
        [](const std::vector<float>& params) {
            Dx7Voice voice = Dx7Voice::init();
            adx::dx7::voiceFromParams(params, voice);
            return fromVoice(voice);
        },
        py::arg("params"), "dx7 parameter values (the first 156) -> voice bytes, clamped.");
    // GIL: trivial
    m.def(
        "dx7_byte_offset", [](std::size_t byte) { return adx::dx7::byteOffset(byte); },
        py::arg("byte"));
    // GIL: trivial
    m.def("dx7_byte_max", [](std::size_t byte) { return adx::dx7::maxOf(byte); }, py::arg("byte"));
}

} // namespace adx::bindings

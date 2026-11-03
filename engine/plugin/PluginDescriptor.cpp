// adx-thread: main
#include "engine/plugin/PluginDescriptor.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

#include "engine/plugin/Json.h"

namespace adx::plugin {

const char* toString(PluginFormat format) noexcept {
    return format == PluginFormat::Vst3 ? "vst3" : "clap";
}

bool formatFromString(std::string_view text, PluginFormat& out) noexcept {
    if (text == "clap") {
        out = PluginFormat::Clap;
        return true;
    }
    if (text == "vst3") {
        out = PluginFormat::Vst3;
        return true;
    }
    return false;
}

const ParamInfo* PluginDescriptor::findParam(std::string_view key) const noexcept {
    const auto match = std::ranges::find(params, key, &ParamInfo::key);
    return match == params.end() ? nullptr : &*match;
}

std::string paramKey(std::string_view name, std::uint32_t id, const std::vector<ParamInfo>& taken) {
    std::string key;
    key.reserve(name.size());
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            key.push_back(c);
        } else if (c >= 'A' && c <= 'Z') {
            key.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (!key.empty() && key.back() != '_') {
            key.push_back('_');
        }
    }
    while (!key.empty() && key.back() == '_') {
        key.pop_back();
    }
    // A key must not start with a digit or be empty: it is an ident in the file.
    if (key.empty() || (key.front() >= '0' && key.front() <= '9')) {
        key = "p" + std::to_string(id) + (key.empty() ? "" : "_" + key);
    }
    // The slot's own keys are not available to a plugin parameter.
    if (key == "mix" || key == "bypass" || key == "sidechain" || key == "ir" || key == "plugin" ||
        key == "name" || key == "path" || key == "format") {
        key += "_";
    }
    const auto used = [&taken](const std::string& candidate) {
        return std::ranges::any_of(taken,
                                   [&candidate](const ParamInfo& p) { return p.key == candidate; });
    };
    if (!used(key)) {
        return key;
    }
    for (std::uint32_t suffix = 2;; ++suffix) {
        std::string candidate = key + "_" + std::to_string(suffix);
        if (!used(candidate)) {
            return candidate;
        }
    }
}

namespace {

void number(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "0";
        return;
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

void field(std::string& out, std::string_view key, std::string_view text, bool comma = true) {
    json::quote(out, key);
    out.push_back(':');
    json::quote(out, text);
    if (comma) {
        out.push_back(',');
    }
}

void numberField(std::string& out, std::string_view key, double value, bool comma = true) {
    json::quote(out, key);
    out.push_back(':');
    number(out, value);
    if (comma) {
        out.push_back(',');
    }
}

void boolField(std::string& out, std::string_view key, bool value, bool comma = true) {
    json::quote(out, key);
    out += value ? ":true" : ":false";
    if (comma) {
        out.push_back(',');
    }
}

void writeDescriptor(std::string& out, const PluginDescriptor& plugin) {
    out.push_back('{');
    field(out, "format", toString(plugin.id.format));
    field(out, "uid", plugin.id.uid);
    field(out, "path", plugin.id.path);
    field(out, "name", plugin.id.name);
    field(out, "version", plugin.id.version);
    field(out, "vendor", plugin.vendor);
    boolField(out, "instrument", plugin.instrument);
    numberField(out, "inputs", plugin.audioInputs);
    numberField(out, "outputs", plugin.audioOutputs);
    boolField(out, "editor", plugin.hasEditor);
    numberField(out, "latency", plugin.latency);
    out += "\"params\":[";
    for (std::size_t i = 0; i < plugin.params.size(); ++i) {
        const ParamInfo& param = plugin.params[i];
        if (i != 0) {
            out.push_back(',');
        }
        out.push_back('{');
        numberField(out, "id", param.id);
        field(out, "key", param.key);
        field(out, "name", param.name);
        numberField(out, "min", param.minimum);
        numberField(out, "max", param.maximum);
        numberField(out, "default", param.defaultValue);
        numberField(out, "steps", param.steps);
        boolField(out, "automatable", param.automatable);
        boolField(out, "hidden", param.hidden, false);
        out.push_back('}');
    }
    out += "]}";
}

bool readDescriptor(const json::Value& value, PluginDescriptor& out) {
    if (value.kind != json::Value::Kind::Object) {
        return false;
    }
    if (!formatFromString(value.str("format"), out.id.format)) {
        return false;
    }
    out.id.uid = value.str("uid");
    out.id.path = value.str("path");
    out.id.name = value.str("name");
    out.id.version = value.str("version");
    out.vendor = value.str("vendor");
    out.instrument = value.flag("instrument");
    out.audioInputs = static_cast<std::uint32_t>(value.num("inputs", 2));
    out.audioOutputs = static_cast<std::uint32_t>(value.num("outputs", 2));
    out.hasEditor = value.flag("editor");
    out.latency = static_cast<std::uint32_t>(value.num("latency"));
    if (const json::Value* params = value.get("params");
        params != nullptr && params->kind == json::Value::Kind::Array) {
        for (const json::Value& item : params->items) {
            ParamInfo param;
            param.id = static_cast<std::uint32_t>(item.num("id"));
            param.key = item.str("key");
            param.name = item.str("name");
            param.minimum = item.num("min");
            param.maximum = item.num("max", 1.0);
            param.defaultValue = item.num("default");
            param.steps = static_cast<std::uint32_t>(item.num("steps"));
            param.automatable = item.flag("automatable", true);
            param.hidden = item.flag("hidden");
            out.params.push_back(std::move(param));
        }
    }
    return !out.id.uid.empty();
}

} // namespace

std::string toJson(const ScanResult& result) {
    std::string out;
    out.push_back('{');
    field(out, "path", result.path);
    field(out, "format", toString(result.format));
    // As text: a file time is past 2^53 and would not survive a double.
    field(out, "mtime", std::to_string(result.mtime));
    field(out, "size", std::to_string(result.size));
    boolField(out, "ok", result.ok);
    field(out, "error", result.error);
    out += "\"plugins\":[";
    for (std::size_t i = 0; i < result.plugins.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        writeDescriptor(out, result.plugins[i]);
    }
    out += "]}";
    return out;
}

bool fromJson(std::string_view text, ScanResult& out, std::string& error) {
    json::Value root;
    if (!json::parse(text, root, error)) {
        return false;
    }
    return fromJson(root, out, error);
}

bool fromJson(const json::Value& root, ScanResult& out, std::string& error) {
    if (root.kind != json::Value::Kind::Object) {
        error = "a scan result is a JSON object";
        return false;
    }
    out = ScanResult{};
    out.path = root.str("path");
    static_cast<void>(formatFromString(root.str("format"), out.format));
    const std::string mtime = root.str("mtime");
    const std::string size = root.str("size");
    std::from_chars(mtime.data(), mtime.data() + mtime.size(), out.mtime);
    std::from_chars(size.data(), size.data() + size.size(), out.size);
    out.ok = root.flag("ok");
    out.error = root.str("error");
    if (const json::Value* plugins = root.get("plugins");
        plugins != nullptr && plugins->kind == json::Value::Kind::Array) {
        for (const json::Value& item : plugins->items) {
            PluginDescriptor plugin;
            if (!readDescriptor(item, plugin)) {
                error = "a plugin entry has no format or uid";
                return false;
            }
            out.plugins.push_back(std::move(plugin));
        }
    }
    return true;
}

} // namespace adx::plugin

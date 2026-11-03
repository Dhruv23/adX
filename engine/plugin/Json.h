// adx-thread: main
//
// Just enough JSON for scan results and the plugin cache: objects, arrays, strings,
// numbers and booleans. Not a general library - nothing else in the engine reads JSON,
// and a dependency for forty lines of descriptor would be the wrong trade.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace adx::plugin::json {

struct Value {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0.0};
    std::string text;
    std::vector<Value> items;
    std::map<std::string, Value, std::less<>> fields;

    [[nodiscard]] const Value* get(std::string_view key) const noexcept;
    [[nodiscard]] std::string str(std::string_view key) const;
    [[nodiscard]] double num(std::string_view key, double fallback = 0.0) const noexcept;
    [[nodiscard]] bool flag(std::string_view key, bool fallback = false) const noexcept;
};

/// False with `error` set on malformed input. Trailing whitespace is allowed, nothing
/// else.
[[nodiscard]] bool parse(std::string_view text, Value& out, std::string& error);

/// Appends `text` as a JSON string literal, quotes included.
void quote(std::string& out, std::string_view text);

} // namespace adx::plugin::json

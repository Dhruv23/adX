// adx-thread: main
#include "engine/plugin/Json.h"

#include <array>
#include <charconv>
#include <cstdio>

namespace adx::plugin::json {

const Value* Value::get(std::string_view key) const noexcept {
    const auto found = fields.find(key);
    return found == fields.end() ? nullptr : &found->second;
}

std::string Value::str(std::string_view key) const {
    const Value* value = get(key);
    return value != nullptr && value->kind == Kind::String ? value->text : std::string{};
}

double Value::num(std::string_view key, double fallback) const noexcept {
    const Value* value = get(key);
    return value != nullptr && value->kind == Kind::Number ? value->number : fallback;
}

bool Value::flag(std::string_view key, bool fallback) const noexcept {
    const Value* value = get(key);
    return value != nullptr && value->kind == Kind::Bool ? value->boolean : fallback;
}

namespace {

class Reader {
public:
    explicit Reader(std::string_view text) : m_text(text) {}

    bool document(Value& out, std::string& error) {
        if (!value(out, 0)) {
            error = m_error.empty() ? "malformed JSON" : m_error;
            return false;
        }
        skip();
        if (m_at != m_text.size()) {
            error = "trailing characters after the JSON value at offset " + std::to_string(m_at);
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    void skip() noexcept {
        while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\t' ||
                                        m_text[m_at] == '\n' || m_text[m_at] == '\r')) {
            ++m_at;
        }
    }

    bool fail(std::string message) {
        if (m_error.empty()) {
            m_error = std::move(message) + " at offset " + std::to_string(m_at);
        }
        return false;
    }

    bool literal(std::string_view word) noexcept {
        if (m_text.substr(m_at, word.size()) == word) {
            m_at += word.size();
            return true;
        }
        return false;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nested too deeply");
        }
        skip();
        if (m_at >= m_text.size()) {
            return fail("unexpected end");
        }
        const char c = m_text[m_at];
        if (c == '{') {
            return object(out, depth);
        }
        if (c == '[') {
            return array(out, depth);
        }
        if (c == '"') {
            out.kind = Value::Kind::String;
            return string(out.text);
        }
        if (literal("true")) {
            out.kind = Value::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.kind = Value::Kind::Bool;
            out.boolean = false;
            return true;
        }
        if (literal("null")) {
            out.kind = Value::Kind::Null;
            return true;
        }
        return number(out);
    }

    bool number(Value& out) {
        const char* begin = m_text.data() + m_at;
        const char* end = m_text.data() + m_text.size();
        double parsed = 0.0;
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec != std::errc{} || result.ptr == begin) {
            return fail("expected a value");
        }
        m_at += static_cast<std::size_t>(result.ptr - begin);
        out.kind = Value::Kind::Number;
        out.number = parsed;
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool hex4(std::uint32_t& out) {
        if (m_at + 4 > m_text.size()) {
            return fail("short \\u escape");
        }
        std::uint32_t code = 0;
        const auto result =
            std::from_chars(m_text.data() + m_at, m_text.data() + m_at + 4, code, 16);
        if (result.ec != std::errc{} || result.ptr != m_text.data() + m_at + 4) {
            return fail("bad \\u escape");
        }
        m_at += 4;
        out = code;
        return true;
    }

    bool string(std::string& out) {
        ++m_at; // the opening quote
        out.clear();
        while (m_at < m_text.size()) {
            const char c = m_text[m_at++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_at >= m_text.size()) {
                break;
            }
            const char escape = m_text[m_at++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out.push_back(escape);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code < 0xDC00 && m_text.substr(m_at, 2) == "\\u") {
                    m_at += 2;
                    std::uint32_t low = 0;
                    if (!hex4(low)) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Value& out, int depth) {
        ++m_at;
        out.kind = Value::Kind::Array;
        skip();
        if (m_at < m_text.size() && m_text[m_at] == ']') {
            ++m_at;
            return true;
        }
        while (true) {
            Value item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(item));
            skip();
            if (m_at < m_text.size() && m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == ']') {
                ++m_at;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool object(Value& out, int depth) {
        ++m_at;
        out.kind = Value::Kind::Object;
        skip();
        if (m_at < m_text.size() && m_text[m_at] == '}') {
            ++m_at;
            return true;
        }
        while (true) {
            skip();
            if (m_at >= m_text.size() || m_text[m_at] != '"') {
                return fail("expected a key");
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            skip();
            if (m_at >= m_text.size() || m_text[m_at] != ':') {
                return fail("expected ':'");
            }
            ++m_at;
            Value item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.fields.insert_or_assign(std::move(key), std::move(item));
            skip();
            if (m_at < m_text.size() && m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == '}') {
                ++m_at;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    std::string_view m_text;
    std::size_t m_at{0};
    std::string m_error;
};

} // namespace

bool parse(std::string_view text, Value& out, std::string& error) {
    out = Value{};
    return Reader{text}.document(out, error);
}

void quote(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::array<char, 8> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer.data();
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

} // namespace adx::plugin::json

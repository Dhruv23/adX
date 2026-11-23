// adx-thread: main
#include "engine/project/Formula.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

#include "engine/dsp/Math.h"

namespace adx::project {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Every function goes through dsp/Math.h, so a formula's lane has the same bits in
// every build, like every other part of a render (phase_3.md §4.10).

double sinRadians(double x) noexcept {
    return dsp::sinTurns(x / dsp::kTwoPi);
}
double cosRadians(double x) noexcept {
    return dsp::cosTurns(x / dsp::kTwoPi);
}
double atan2Radians(double y, double x) noexcept {
    return dsp::atan2Turns(y, x) * dsp::kTwoPi;
}
double power(double base, double exponent) noexcept {
    // A whole exponent by squaring: exact where it can be (2^9 is 512, not 512.0000000007),
    // and only multiplies, so the same bits in every build.
    if (dsp::floorExact(exponent) == exponent && exponent >= -64.0 && exponent <= 64.0) {
        auto n = static_cast<int>(exponent < 0.0 ? -exponent : exponent);
        double result = 1.0;
        double square = base;
        while (n > 0) {
            if ((n & 1) != 0) {
                result *= square;
            }
            square *= square;
            n >>= 1;
        }
        return exponent < 0.0 ? 1.0 / result : result;
    }
    if (base > 0.0) {
        return dsp::pow(base, exponent);
    }
    if (base == 0.0) {
        return exponent > 0.0 ? 0.0 : (exponent == 0.0 ? 1.0 : kInfinity);
    }
    // A negative base has a real power only for a whole exponent.
    if (dsp::floorExact(exponent) != exponent) {
        return kNaN;
    }
    const double magnitude = dsp::pow(-base, exponent);
    const bool odd = std::fmod(exponent, 2.0) != 0.0;
    return odd ? -magnitude : magnitude;
}
double fraction(double x) noexcept {
    return x - dsp::floorExact(x);
}
double triangle(double x) noexcept {
    const double f = fraction(x);
    if (f < 0.25) {
        return 4.0 * f;
    }
    return f < 0.75 ? 2.0 - (4.0 * f) : (4.0 * f) - 4.0;
}
/// splitmix64 of the whole part: a fixed 0..1 for each integer.
double random01(double x) noexcept {
    auto z = static_cast<std::uint64_t>(static_cast<std::int64_t>(dsp::floorExact(x)));
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<double>(z >> 11U) * 0x1.0p-53;
}
double clamp3(double v, double lo, double hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

using Fn = double (*)(const double* args) noexcept;

struct Function {
    std::string_view name;
    std::uint8_t arity;
    Fn fn;
};

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic) - args points at `arity`
// values on the evaluation stack.
constexpr std::array kFunctions = std::to_array<Function>({
    {"sin", 1, [](const double* v) noexcept { return sinRadians(v[0]); }},
    {"cos", 1, [](const double* v) noexcept { return cosRadians(v[0]); }},
    {"tan", 1, [](const double* v) noexcept { return sinRadians(v[0]) / cosRadians(v[0]); }},
    {"asin", 1,
     [](const double* v) noexcept { return atan2Radians(v[0], std::sqrt(1.0 - (v[0] * v[0]))); }},
    {"acos", 1,
     [](const double* v) noexcept { return atan2Radians(std::sqrt(1.0 - (v[0] * v[0])), v[0]); }},
    {"atan", 1, [](const double* v) noexcept { return atan2Radians(v[0], 1.0); }},
    {"atan2", 2, [](const double* v) noexcept { return atan2Radians(v[0], v[1]); }},
    {"sinh", 1, [](const double* v) noexcept { return (dsp::exp(v[0]) - dsp::exp(-v[0])) / 2.0; }},
    {"cosh", 1, [](const double* v) noexcept { return (dsp::exp(v[0]) + dsp::exp(-v[0])) / 2.0; }},
    {"tanh", 1, [](const double* v) noexcept { return dsp::tanh(v[0]); }},
    {"abs", 1, [](const double* v) noexcept { return v[0] < 0.0 ? -v[0] : v[0]; }},
    {"sqrt", 1, [](const double* v) noexcept { return std::sqrt(v[0]); }},
    {"cbrt", 1,
     [](const double* v) noexcept {
         const double m = dsp::pow(v[0] < 0.0 ? -v[0] : v[0], 1.0 / 3.0);
         return v[0] < 0.0 ? -m : m;
     }},
    {"exp", 1, [](const double* v) noexcept { return dsp::exp(v[0]); }},
    {"log", 1, [](const double* v) noexcept { return v[0] > 0.0 ? dsp::log(v[0]) : kNaN; }},
    {"log2", 1, [](const double* v) noexcept { return v[0] > 0.0 ? dsp::log2(v[0]) : kNaN; }},
    {"log10", 1, [](const double* v) noexcept { return v[0] > 0.0 ? dsp::log10(v[0]) : kNaN; }},
    {"pow", 2, [](const double* v) noexcept { return power(v[0], v[1]); }},
    {"min", 2, [](const double* v) noexcept { return v[0] < v[1] ? v[0] : v[1]; }},
    {"max", 2, [](const double* v) noexcept { return v[0] > v[1] ? v[0] : v[1]; }},
    {"clamp", 3, [](const double* v) noexcept { return clamp3(v[0], v[1], v[2]); }},
    {"floor", 1, [](const double* v) noexcept { return dsp::floorExact(v[0]); }},
    {"ceil", 1, [](const double* v) noexcept { return -dsp::floorExact(-v[0]); }},
    {"round", 1, [](const double* v) noexcept { return dsp::floorExact(v[0] + 0.5); }},
    {"frac", 1, [](const double* v) noexcept { return fraction(v[0]); }},
    {"sign", 1,
     [](const double* v) noexcept { return v[0] > 0.0 ? 1.0 : (v[0] < 0.0 ? -1.0 : 0.0); }},
    {"lerp", 3, [](const double* v) noexcept { return v[0] + ((v[1] - v[0]) * v[2]); }},
    {"step", 2, [](const double* v) noexcept { return v[1] < v[0] ? 0.0 : 1.0; }},
    {"smoothstep", 3,
     [](const double* v) noexcept {
         const double span = v[1] - v[0];
         if (span == 0.0) {
             return v[2] < v[0] ? 0.0 : 1.0;
         }
         const double x = clamp3((v[2] - v[0]) / span, 0.0, 1.0);
         return x * x * (3.0 - (2.0 * x));
     }},
    {"hypot", 2, [](const double* v) noexcept { return std::sqrt((v[0] * v[0]) + (v[1] * v[1])); }},
    {"tri", 1, [](const double* v) noexcept { return triangle(v[0]); }},
    {"saw", 1, [](const double* v) noexcept { return (2.0 * fraction(v[0])) - 1.0; }},
    {"sq", 1, [](const double* v) noexcept { return fraction(v[0]) < 0.5 ? 1.0 : -1.0; }},
    {"rand", 1, [](const double* v) noexcept { return random01(v[0]); }},
});
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

/// How deep parentheses and unary operators may nest before compile gives up, so a
/// hostile formula cannot overflow the compiler's own stack.
constexpr int kMaxNesting = 200;

bool isIdentStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool isIdentChar(char c) noexcept {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}
bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

} // namespace

// NOLINTBEGIN(misc-no-recursion) - a recursive-descent parser recurses by design, and
// kMaxNesting bounds it.
class FormulaCompiler {
public:
    FormulaCompiler(std::string_view text, Formula& out) : m_text(text), m_out(out) {}

    std::optional<FormulaError> run() {
        skipSpace();
        if (m_pos >= m_text.size()) {
            return fail(0, m_text.size(), "the formula is empty");
        }
        if (!expression()) {
            return m_error;
        }
        skipSpace();
        if (m_pos < m_text.size()) {
            return fail(m_pos, 1, "unexpected '" + std::string(1, m_text[m_pos]) + "'");
        }
        return std::nullopt;
    }

private:
    using Op = Formula::Op;

    std::string_view m_text;
    Formula& m_out;
    std::size_t m_pos{0};
    std::size_t m_depth{0};
    int m_nesting{0};
    std::optional<FormulaError> m_error;

    std::optional<FormulaError> fail(std::size_t offset, std::size_t length, std::string message) {
        if (!m_error) {
            m_error = FormulaError{.offset = offset, .length = length, .message = std::move(message)};
        }
        return m_error;
    }
    bool failed(std::size_t offset, std::size_t length, std::string message) {
        static_cast<void>(fail(offset, length, std::move(message)));
        return false;
    }

    void skipSpace() noexcept {
        while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t')) {
            ++m_pos;
        }
    }
    /// Consumes `token` if it is next.
    bool accept(std::string_view token) noexcept {
        skipSpace();
        if (m_text.substr(m_pos).starts_with(token)) {
            m_pos += token.size();
            return true;
        }
        return false;
    }
    /// The next character, without consuming it, past spaces; 0 at the end.
    char peek() noexcept {
        skipSpace();
        return m_pos < m_text.size() ? m_text[m_pos] : '\0';
    }

    /// Appends one instruction and tracks how deep the stack gets: `pops` values
    /// consumed, one pushed.
    bool emit(Formula::Instruction instruction, std::size_t pops) {
        m_depth = m_depth - pops + 1;
        if (m_depth > Formula::kMaxStack) {
            return failed(0, m_text.size(), "the formula is too deeply nested");
        }
        m_out.m_program.push_back(instruction);
        return true;
    }
    bool emitOp(Op op, std::size_t pops) {
        return emit(Formula::Instruction{.op = op, .function = 0, .number = 0.0}, pops);
    }

    bool expression() {
        if (++m_nesting > kMaxNesting) {
            return failed(m_pos, 1, "the formula is too deeply nested");
        }
        const bool ok = ternary();
        --m_nesting;
        return ok;
    }

    bool ternary() {
        if (!logicalOr()) {
            return false;
        }
        if (!accept("?")) {
            return true;
        }
        if (!expression()) {
            return false;
        }
        if (!accept(":")) {
            return failed(m_pos, 1, "expected ':' to finish the '?'");
        }
        if (!expression()) {
            return false;
        }
        return emitOp(Op::Select, 3);
    }

    template<class Next>
    bool binaryLevel(Next next, std::initializer_list<std::pair<std::string_view, Op>> ops) {
        if (!(this->*next)()) {
            return false;
        }
        for (;;) {
            bool matched = false;
            for (const auto& [token, op] : ops) {
                // "<" must not swallow the start of "<=": longer tokens are listed first.
                if (accept(token)) {
                    if (!(this->*next)() || !emitOp(op, 2)) {
                        return false;
                    }
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return true;
            }
        }
    }

    bool logicalOr() {
        return binaryLevel(&FormulaCompiler::logicalAnd, {{"||", Op::Or}});
    }
    bool logicalAnd() {
        return binaryLevel(&FormulaCompiler::equality, {{"&&", Op::And}});
    }
    bool equality() {
        return binaryLevel(&FormulaCompiler::relational, {{"==", Op::Equal}, {"!=", Op::NotEqual}});
    }
    bool relational() {
        return binaryLevel(&FormulaCompiler::additive, {{"<=", Op::LessEqual},
                                                        {">=", Op::GreaterEqual},
                                                        {"<", Op::Less},
                                                        {">", Op::Greater}});
    }
    bool additive() {
        return binaryLevel(&FormulaCompiler::multiplicative,
                           {{"+", Op::Add}, {"-", Op::Subtract}});
    }
    bool multiplicative() {
        return binaryLevel(&FormulaCompiler::unary,
                           {{"*", Op::Multiply}, {"/", Op::Divide}, {"%", Op::Modulo}});
    }

    bool unary() {
        if (++m_nesting > kMaxNesting) {
            return failed(m_pos, 1, "the formula is too deeply nested");
        }
        bool ok = false;
        if (accept("-")) {
            ok = unary() && emitOp(Op::Negate, 1);
        } else if (accept("+")) {
            ok = unary();
        } else if (peek() == '!' && !m_text.substr(m_pos).starts_with("!=")) {
            ++m_pos;
            ok = unary() && emitOp(Op::Not, 1);
        } else {
            ok = powerLevel();
        }
        --m_nesting;
        return ok;
    }

    bool powerLevel() {
        if (!primary()) {
            return false;
        }
        if (accept("^")) {
            // Right-associative, and binds tighter than a unary minus on its left:
            // -2^2 is -4, 2^-1 is 0.5.
            return unary() && emitOp(Op::Power, 2);
        }
        return true;
    }

    bool primary() {
        const char c = peek();
        if (c == '\0') {
            return failed(m_text.size(), 0, "the formula ends where a value was expected");
        }
        if (c == '(') {
            ++m_pos;
            if (!expression()) {
                return false;
            }
            if (!accept(")")) {
                return failed(m_pos, 1, "expected ')'");
            }
            return true;
        }
        if (isDigit(c) || c == '.') {
            return number();
        }
        if (isIdentStart(c)) {
            return identifier();
        }
        return failed(m_pos, 1, "unexpected '" + std::string(1, c) + "'");
    }

    bool number() {
        const std::size_t start = m_pos;
        while (m_pos < m_text.size() && (isDigit(m_text[m_pos]) || m_text[m_pos] == '.')) {
            ++m_pos;
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            std::size_t look = m_pos + 1;
            if (look < m_text.size() && (m_text[look] == '+' || m_text[look] == '-')) {
                ++look;
            }
            if (look < m_text.size() && isDigit(m_text[look])) {
                m_pos = look;
                while (m_pos < m_text.size() && isDigit(m_text[m_pos])) {
                    ++m_pos;
                }
            }
        }
        double value = 0.0;
        const char* first = m_text.data() + start;
        const char* last = m_text.data() + m_pos;
        const auto [end, error] = std::from_chars(first, last, value);
        if (error != std::errc{} || end != last) {
            return failed(start, m_pos - start, "'" + std::string(m_text.substr(start, m_pos - start)) +
                                                    "' is not a number");
        }
        return emit(Formula::Instruction{.op = Op::Number, .function = 0, .number = value}, 0);
    }

    bool identifier() {
        const std::size_t start = m_pos;
        while (m_pos < m_text.size() && isIdentChar(m_text[m_pos])) {
            ++m_pos;
        }
        const std::string_view name = m_text.substr(start, m_pos - start);
        if (peek() == '(') {
            return call(name, start);
        }
        static constexpr std::array<std::pair<std::string_view, Op>, 6> kVariables{{
            {"t", Op::VarT},
            {"s", Op::VarS},
            {"p", Op::VarP},
            {"a", Op::VarA},
            {"b", Op::VarB},
            {"c", Op::VarC},
        }};
        for (const auto& [variable, op] : kVariables) {
            if (name == variable) {
                if (op == Op::VarT || op == Op::VarS || op == Op::VarP) {
                    m_out.m_varies = true;
                }
                return emitOp(op, 0);
            }
        }
        if (name == "pi") {
            return emit(Formula::Instruction{.op = Op::Number, .function = 0, .number = dsp::kPi},
                        0);
        }
        if (name == "e") {
            return emit(Formula::Instruction{.op = Op::Number,
                                             .function = 0,
                                             .number = 2.71828182845904523536},
                        0);
        }
        return failed(start, name.size(),
                      "unknown name '" + std::string(name) + "' (variables are t s p a b c)");
    }

    bool call(std::string_view name, std::size_t start) {
        const auto match = std::ranges::find(kFunctions, name, &Function::name);
        if (match == kFunctions.end()) {
            return failed(start, name.size(), "unknown function '" + std::string(name) + "'");
        }
        ++m_pos; // '('
        std::size_t count = 0;
        if (!accept(")")) {
            for (;;) {
                if (!expression()) {
                    return false;
                }
                ++count;
                if (accept(")")) {
                    break;
                }
                if (!accept(",")) {
                    return failed(m_pos, 1, "expected ',' or ')' in " + std::string(name) + "()");
                }
            }
        }
        if (count != match->arity) {
            return failed(start, m_pos - start,
                          std::string(name) + "() takes " + std::to_string(match->arity) +
                              (match->arity == 1 ? " argument" : " arguments") + ", not " +
                              std::to_string(count));
        }
        const auto index = static_cast<std::uint8_t>(std::distance(kFunctions.begin(), match));
        return emit(Formula::Instruction{.op = Op::Call, .function = index, .number = 0.0}, count);
    }
};
// NOLINTEND(misc-no-recursion)

Formula Formula::compile(std::string_view text, std::optional<FormulaError>& error) {
    Formula formula;
    FormulaCompiler compiler(text, formula);
    error = compiler.run();
    if (error) {
        return Formula{};
    }
    return formula;
}

double Formula::evaluate(const FormulaInputs& inputs) const noexcept {
    if (m_program.empty()) {
        return 0.0;
    }
    std::array<double, kMaxStack> stack{};
    std::size_t top = 0; // one past the last value
    const auto pop = [&stack, &top]() noexcept { return stack.at(--top); };
    const auto push = [&stack, &top](double v) noexcept { stack.at(top++) = v; };
    for (const Instruction& in : m_program) {
        switch (in.op) {
        case Op::Number:
            push(in.number);
            break;
        case Op::VarT:
            push(inputs.t);
            break;
        case Op::VarS:
            push(inputs.s);
            break;
        case Op::VarP:
            push(inputs.p);
            break;
        case Op::VarA:
            push(inputs.a);
            break;
        case Op::VarB:
            push(inputs.b);
            break;
        case Op::VarC:
            push(inputs.c);
            break;
        case Op::Negate:
            push(-pop());
            break;
        case Op::Not:
            push(pop() == 0.0 ? 1.0 : 0.0);
            break;
        case Op::Select: {
            const double otherwise = pop();
            const double then = pop();
            push(pop() != 0.0 ? then : otherwise);
            break;
        }
        case Op::Call: {
            const Function& function = kFunctions.at(in.function);
            top -= function.arity;
            push(function.fn(&stack.at(top)));
            break;
        }
        default: {
            const double right = pop();
            const double left = pop();
            double result = 0.0;
            switch (in.op) {
            case Op::Add:
                result = left + right;
                break;
            case Op::Subtract:
                result = left - right;
                break;
            case Op::Multiply:
                result = left * right;
                break;
            case Op::Divide:
                result = left / right;
                break;
            case Op::Modulo:
                // Floored, so a negative t still counts up through 0..right.
                result = right == 0.0 ? kNaN : left - (right * dsp::floorExact(left / right));
                break;
            case Op::Power:
                result = power(left, right);
                break;
            case Op::Less:
                result = left < right ? 1.0 : 0.0;
                break;
            case Op::LessEqual:
                result = left <= right ? 1.0 : 0.0;
                break;
            case Op::Greater:
                result = left > right ? 1.0 : 0.0;
                break;
            case Op::GreaterEqual:
                result = left >= right ? 1.0 : 0.0;
                break;
            case Op::Equal:
                result = left == right ? 1.0 : 0.0;
                break;
            case Op::NotEqual:
                result = left != right ? 1.0 : 0.0;
                break;
            case Op::And:
                result = (left != 0.0 && right != 0.0) ? 1.0 : 0.0;
                break;
            case Op::Or:
                result = (left != 0.0 || right != 0.0) ? 1.0 : 0.0;
                break;
            default:
                break;
            }
            push(result);
            break;
        }
        }
    }
    const double value = top == 1 ? stack.front() : 0.0;
    return std::isfinite(value) ? value : 0.0;
}

} // namespace adx::project

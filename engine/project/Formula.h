// adx-thread: main
//
// The formula controller's expression language: one line of arithmetic over position.
//
//   0.5 + 0.5 * sin(2 * pi * t / 4)      a sine, one cycle a bar
//   p < a ? 1 : -1                        a pulse whose width is `a`
//   lerp(-1, 1, smoothstep(0, 8, t))      a rise over the first two bars
//
// Variables: `t` beats (quarter notes) since the start, `s` seconds, `p` 0..1 through the
// controller's PERIOD, `a` `b` `c` the controller's three inputs, `pi` and `e`.
// Operators, loosest first: `?:`, `||`, `&&`, `== !=`, `< <= > >=`, `+ -`, `* / %`,
// unary `- + !`, and `^` (power, right-associative). A comparison or a logical operator
// is 1 or 0. Functions: sin cos tan asin acos atan atan2 sinh cosh tanh abs sqrt cbrt
// exp log log2 log10 pow min max clamp floor ceil round frac sign lerp step smoothstep
// hypot, the LFO shapes `tri saw sq` (one cycle per unit, -1..1), and `rand(x)`, a fixed
// pseudo-random 0..1 for each whole value of x. Trigonometry is in radians.
//
// A formula compiles once into a postfix program; evaluating it never allocates and is
// total - a division by zero or a log of a negative is whatever IEEE says, and the
// caller clamps a NaN to 0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adx::project {

struct FormulaInputs {
    double t{0.0};
    double s{0.0};
    double p{0.0};
    double a{0.0};
    double b{0.0};
    double c{0.0};
};

struct FormulaError {
    /// Where in the text, in bytes, and how much of it.
    std::size_t offset{0};
    std::size_t length{0};
    std::string message;
};

class Formula {
public:
    /// Compiles `text`; an error says where and why.
    [[nodiscard]] static Formula compile(std::string_view text, std::optional<FormulaError>& error);

    /// The expression's value for `inputs`; 0 when it did not compile, or for a NaN.
    [[nodiscard]] double evaluate(const FormulaInputs& inputs) const noexcept;

    [[nodiscard]] bool empty() const noexcept {
        return m_program.empty();
    }
    /// Whether the value can change with position (reads t, s or p) - a constant
    /// compiles to a single knot.
    [[nodiscard]] bool varies() const noexcept {
        return m_varies;
    }

    enum class Op : std::uint8_t {
        Number,
        VarT,
        VarS,
        VarP,
        VarA,
        VarB,
        VarC,
        Negate,
        Not,
        Add,
        Subtract,
        Multiply,
        Divide,
        Modulo,
        Power,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        Equal,
        NotEqual,
        And,
        Or,
        Select,
        Call,
    };

    struct Instruction {
        Op op{Op::Number};
        /// For Call: which function.
        std::uint8_t function{0};
        double number{0.0};
    };

    /// The deepest the evaluation stack may grow; a deeper formula does not compile.
    static constexpr std::size_t kMaxStack = 64;

private:
    std::vector<Instruction> m_program;
    bool m_varies{false};

    friend class FormulaCompiler;
};

} // namespace adx::project

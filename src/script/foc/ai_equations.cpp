// Perceptual equations of the FoC tactical AI (#449, docs/behaviour/foc-tactical-ai.md
// "Perceptual equations"). Rule IDs PE-xx cite that note.

#include "ai_equations.hpp"

#include "eawr/script/numeric/binary64.hpp"
#include "eawr/script/numeric/decimal.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace eawr::script::foc::ai {
namespace {

namespace b64 = numeric::binary64;

core::Diagnostic parse_error(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = "EAWR-AI-0101";
    diagnostic.message = std::move(message);
    return diagnostic;
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& character : out) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - ('a' - 'A'));
    }
    return out;
}

bool identifier_start(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_';
}

bool identifier_part(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

// Recursive descent over the PE-01 grammar; emits postfix nodes.
class Parser {
public:
    Parser(std::string_view text, Equation& out, const ConverterFunction& converters)
        : text_(text), out_(out), converters_(converters) {}

    core::Result<void> parse() {
        if (auto parsed = comparison(); !parsed) return parsed;
        skip();
        if (position_ != text_.size()) return fail("unexpected text");
        return core::Result<void>::success();
    }

private:
    core::Result<void> fail(std::string what) const {
        return core::Result<void>::failure(parse_error(
            out_.name + ": " + what + " at offset " + std::to_string(position_)));
    }

    void skip() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_])) != 0) ++position_;
    }

    bool accept(std::string_view token) {
        skip();
        if (text_.substr(position_, token.size()) != token) return false;
        position_ += token.size();
        return true;
    }

    void emit(Node::Kind kind) {
        Node node;
        node.kind = kind;
        out_.program.push_back(std::move(node));
    }

    // comparison := additive (op additive)*  (lowest precedence, left-associative)
    core::Result<void> comparison() {
        if (auto parsed = additive(); !parsed) return parsed;
        for (;;) {
            Node::Kind kind;
            if (accept("==")) kind = Node::Kind::equal;
            else if (accept("!=")) kind = Node::Kind::not_equal;
            else if (accept("<=")) kind = Node::Kind::less_equal;
            else if (accept(">=")) kind = Node::Kind::greater_equal;
            else if (accept("<")) kind = Node::Kind::less;
            else if (accept(">")) kind = Node::Kind::greater;
            else return core::Result<void>::success();
            if (auto parsed = additive(); !parsed) return parsed;
            emit(kind);
        }
    }

    core::Result<void> additive() {
        if (auto parsed = multiplicative(); !parsed) return parsed;
        for (;;) {
            Node::Kind kind;
            if (accept("+")) kind = Node::Kind::add;
            else if (accept("-")) kind = Node::Kind::subtract;
            else return core::Result<void>::success();
            if (auto parsed = multiplicative(); !parsed) return parsed;
            emit(kind);
        }
    }

    // * / and the random operator # share one level (PE-01).
    core::Result<void> multiplicative() {
        if (auto parsed = unary(); !parsed) return parsed;
        for (;;) {
            Node::Kind kind;
            if (accept("*")) kind = Node::Kind::multiply;
            else if (accept("/")) kind = Node::Kind::divide;
            else if (accept("#")) kind = Node::Kind::random;
            else return core::Result<void>::success();
            if (auto parsed = unary(); !parsed) return parsed;
            emit(kind);
        }
    }

    core::Result<void> unary() {
        if (accept("-")) {
            if (auto parsed = unary(); !parsed) return parsed;
            emit(Node::Kind::negate);
            return core::Result<void>::success();
        }
        return primary();
    }

    core::Result<void> primary() {
        skip();
        if (position_ >= text_.size()) return fail("expression expected");
        const char next = text_[position_];
        if (next == '(') {
            ++position_;
            if (auto parsed = comparison(); !parsed) return parsed;
            if (!accept(")")) return fail("')' expected");
            return core::Result<void>::success();
        }
        if (next == '"') {
            const std::size_t end = text_.find('"', position_ + 1);
            if (end == std::string_view::npos) return fail("unterminated string");
            Node node;
            node.kind = Node::Kind::text;
            node.text = upper(text_.substr(position_ + 1, end - position_ - 1));
            out_.program.push_back(std::move(node));
            position_ = end + 1;
            return core::Result<void>::success();
        }
        if (std::isdigit(static_cast<unsigned char>(next)) != 0 || next == '.') {
            const auto parsed = numeric::decimal::parse_prefix(text_.substr(position_));
            if (parsed.consumed == 0) return fail("number expected");
            Node node;
            node.kind = Node::Kind::constant;
            node.value = Real::from_repr(parsed.value);
            out_.program.push_back(std::move(node));
            position_ += parsed.consumed;
            return core::Result<void>::success();
        }
        if (!identifier_start(next)) return fail("unexpected character");
        std::vector<std::string> chain;
        chain.push_back(identifier());
        while (accept(".")) {
            skip();
            if (position_ >= text_.size() || !identifier_start(text_[position_])) return fail("token expected after '.'");
            chain.push_back(identifier());
        }
        if (chain.size() == 1) {
            skip();
            // clamp(value, low, high)
            if (position_ < text_.size() && text_[position_] == '(') {
                if (upper(chain.front()) != "CLAMP") return fail("unknown function " + chain.front());
                ++position_;
                for (int argument = 0; argument < 3; ++argument) {
                    if (argument > 0 && !accept(",")) return fail("',' expected");
                    if (auto parsed = comparison(); !parsed) return parsed;
                }
                if (!accept(")")) return fail("')' expected");
                emit(Node::Kind::clamp);
                return core::Result<void>::success();
            }
            // Converter[Value | Value]
            if (position_ < text_.size() && text_[position_] == '[') {
                const std::size_t end = text_.find(']', position_);
                if (end == std::string_view::npos) return fail("']' expected");
                const std::string_view value = text_.substr(position_ + 1, end - position_ - 1);
                const auto converted = converters_ ? converters_(chain.front(), value) : std::nullopt;
                if (!converted) return fail("converter " + chain.front() + " does not know [" + std::string(value) + "]");
                Node node;
                node.kind = Node::Kind::constant;
                node.value = *converted;
                out_.program.push_back(std::move(node));
                position_ = end + 1;
                return core::Result<void>::success();
            }
        }
        Lookup lookup;
        for (const auto& token : chain) lookup.tokens.push_back(upper(token));
        // {Parameter_X = expression, ...}: the values are computed first and bound in order.
        if (accept("{")) {
            for (bool first = true;; first = false) {
                if (accept("}")) break;
                if (!first && !accept(",")) return fail("',' or '}' expected");
                skip();
                if (position_ >= text_.size() || !identifier_start(text_[position_])) return fail("parameter expected");
                lookup.parameters.push_back(upper(identifier()));
                if (!accept("=")) return fail("'=' expected");
                if (auto parsed = comparison(); !parsed) return parsed;
            }
        }
        Node node;
        node.kind = Node::Kind::lookup;
        node.lookup = out_.lookups.size();
        out_.lookups.push_back(std::move(lookup));
        out_.program.push_back(std::move(node));
        return core::Result<void>::success();
    }

    std::string identifier() {
        const std::size_t begin = position_;
        while (position_ < text_.size() && identifier_part(text_[position_])) ++position_;
        return std::string(text_.substr(begin, position_ - begin));
    }

    std::string_view text_;
    Equation& out_;
    const ConverterFunction& converters_;
    std::size_t position_{};
};

bool pop(std::vector<Real>& stack, Real& out) {
    if (stack.empty()) return false;
    out = stack.back();
    stack.pop_back();
    return true;
}

} // namespace

Real to_single(Real value) noexcept {
    const std::uint64_t bits = value.repr;
    if (b64::is_nan(bits) || b64::is_infinite(bits) || b64::is_zero(bits)) return value;
    const bool negative = b64::sign_of(bits);
    const Real zero = Real::from_repr(negative ? b64::negative_zero : b64::positive_zero);
    const std::int32_t field = b64::exponent_of(bits);
    // binary64 subnormals are far below the binary32 range.
    if (field == 0) return zero;
    std::int32_t exponent = field - 1023; // value = 1.fraction * 2^exponent
    std::uint64_t significand = b64::fraction_of(bits) | b64::hidden_bit; // 53 bits
    // Bits of precision the binary32 result keeps: 24 when normal, fewer when subnormal.
    std::int32_t keep = 24;
    if (exponent < -126) keep = 24 - (-126 - exponent);
    if (keep <= 0) {
        // [2^-150, 2^-149): above half the smallest subnormal rounds up to it; the exact half
        // ties to even (zero); anything smaller is zero.
        if (keep == 0 && significand > b64::hidden_bit) return Real::from_repr(b64::pack(negative, -149 + 1022, b64::hidden_bit));
        return zero;
    }
    const auto drop = static_cast<std::uint32_t>(53 - keep);
    const std::uint64_t half = std::uint64_t{1} << (drop - 1U);
    const std::uint64_t mask = (std::uint64_t{1} << drop) - 1U;
    const std::uint64_t remainder = significand & mask;
    significand >>= drop;
    if (remainder > half || (remainder == half && (significand & 1U) != 0)) ++significand;
    if ((significand >> static_cast<std::uint32_t>(keep)) != 0) {
        significand >>= 1U;
        ++exponent;
    }
    significand <<= drop;
    if (exponent > 127) return Real::from_repr(negative ? b64::negative_infinity : b64::positive_infinity);
    // The significand keeps its hidden bit, which pack() adds into the exponent field.
    return Real::from_repr(b64::pack(negative, exponent + 1022, significand));
}

Real real(std::int64_t value) noexcept { return Real(value); }

std::optional<Real> parse_real(std::string_view text) noexcept {
    const auto parsed = numeric::decimal::parse_lua_number(text);
    if (!parsed) return std::nullopt;
    return Real::from_repr(*parsed);
}

Real square_root(Real value) noexcept { return Real::from_repr(b64::square_root(value.repr)); }

std::int64_t truncate(Real value) noexcept { return b64::to_int64_truncate(value.repr); }

std::uint32_t crc32(std::string_view text) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char byte : text) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? 0xedb88320U : 0U);
    }
    return ~crc;
}

std::uint32_t AiRandom::next() noexcept {
    seed_ = seed_ * 0x41c64e6dU + 0xbdfU;
    return (seed_ >> 10U) & 0x7fffU;
}

std::int32_t AiRandom::range(std::int32_t low, std::int32_t high) noexcept {
    if (high < low) std::swap(low, high);
    if (low == high) return low;
    std::uint32_t span = static_cast<std::uint32_t>(high - low);
    if (span == 0) span = 0x7ffe;
    std::uint32_t bit = 0x4000;
    while ((span & bit) == 0 && bit != 0) bit >>= 1U;
    std::uint32_t value = 0;
    do {
        value = next() & (bit * 2U - 1U);
    } while (static_cast<std::int32_t>(span) < static_cast<std::int32_t>(value));
    return static_cast<std::int32_t>(value) + low;
}

Real AiRandom::uniform(Real low, Real high) noexcept {
    if (high < low) std::swap(low, high);
    const Real fraction = to_single(real(next()) / real(32767));
    return to_single(low + to_single(fraction * to_single(high - low)));
}

core::Result<Equation> parse_equation(std::string name, std::string_view body, const ConverterFunction& converters) {
    Equation equation;
    equation.name = std::move(name);
    Parser parser(body, equation, converters);
    if (auto parsed = parser.parse(); !parsed) return core::Result<Equation>::failure(parsed.error());
    return core::Result<Equation>::success(std::move(equation));
}

core::Result<EquationSet> EquationSet::parse(
    const std::vector<std::pair<std::string, std::string>>& files, const ConverterFunction& converters) {
    EquationSet set;
    for (const auto& [path, raw] : files) {
        // Comments and the declaration go; each child element of the root is one equation.
        std::string text;
        text.reserve(raw.size());
        for (std::size_t index = 0; index < raw.size();) {
            if (raw.compare(index, 4, "<!--") == 0) {
                const std::size_t end = raw.find("-->", index + 4);
                index = end == std::string::npos ? raw.size() : end + 3;
                continue;
            }
            if (raw.compare(index, 2, "<?") == 0) {
                const std::size_t end = raw.find("?>", index + 2);
                index = end == std::string::npos ? raw.size() : end + 2;
                continue;
            }
            text.push_back(raw[index++]);
        }
        std::size_t cursor = text.find('<');
        if (cursor == std::string::npos) continue;
        const std::size_t root_end = text.find('>', cursor);
        if (root_end == std::string::npos) return core::Result<EquationSet>::failure(parse_error(path + ": no root element"));
        cursor = root_end + 1;
        for (;;) {
            const std::size_t open = text.find('<', cursor);
            if (open == std::string::npos || (open + 1 < text.size() && text[open + 1] == '/')) break;
            const std::size_t close = text.find('>', open);
            if (close == std::string::npos) break;
            const std::string key = text.substr(open + 1, close - open - 1);
            const std::string end_tag = "</" + key + ">";
            const std::size_t end = text.find(end_tag, close + 1);
            if (end == std::string::npos) return core::Result<EquationSet>::failure(parse_error(path + ": " + key + " is not closed"));
            const std::string upper_key = upper(key);
            if (!set.equations_.contains(upper_key)) {
                auto equation = parse_equation(key, std::string_view(text).substr(close + 1, end - close - 1), converters);
                if (!equation) return core::Result<EquationSet>::failure(equation.error());
                set.equations_.emplace(upper_key, std::move(equation).value());
            }
            cursor = end + end_tag.size();
        }
    }
    return core::Result<EquationSet>::success(std::move(set));
}

const Equation* EquationSet::find(std::string_view name) const {
    const auto found = equations_.find(upper(name));
    return found == equations_.end() ? nullptr : &found->second;
}

std::optional<Real> run(const Equation& equation, LookupResolver& resolver, AiRandom& random) {
    std::vector<Real> stack;
    std::vector<std::string> texts; // string values bound as parameters
    // Parameter values may be strings; they ride on the stack as indices into `texts`.
    std::vector<bool> is_text;
    const auto push = [&](Real value, bool text) {
        stack.push_back(value);
        is_text.push_back(text);
    };
    const auto pop_value = [&](Real& out) {
        if (!pop(stack, out)) return false;
        is_text.pop_back();
        return true;
    };
    for (const Node& node : equation.program) {
        using Kind = Node::Kind;
        switch (node.kind) {
        case Kind::constant: push(node.value, false); break;
        case Kind::text:
            texts.push_back(node.text);
            push(real(static_cast<std::int64_t>(texts.size() - 1)), true);
            break;
        case Kind::lookup: {
            const Lookup& lookup = equation.lookups[node.lookup];
            if (stack.size() < lookup.parameters.size()) return std::nullopt;
            std::vector<Binding> bindings(lookup.parameters.size());
            for (std::size_t index = lookup.parameters.size(); index-- > 0;) {
                const bool text = is_text.back();
                Real value{};
                pop_value(value);
                bindings[index].parameter = lookup.parameters[index];
                if (text) bindings[index].value = texts[static_cast<std::size_t>(truncate(value))];
                else bindings[index].value = value;
            }
            const auto resolved = resolver.resolve(lookup, bindings);
            if (!resolved) return std::nullopt;
            push(*resolved, false);
            break;
        }
        case Kind::negate: {
            Real value{};
            if (!pop_value(value)) return std::nullopt;
            push(-value, false);
            break;
        }
        case Kind::clamp: {
            // PE-04: clamp(v, lo, hi) fails when hi < lo.
            Real high{}, low{}, value{};
            if (stack.size() < 3) return std::nullopt;
            pop_value(high);
            pop_value(low);
            if (high < low) return std::nullopt;
            pop_value(value);
            push(value < low ? low : (high < value ? high : value), false);
            break;
        }
        default: {
            Real right{}, left{};
            if (stack.size() < 2) return std::nullopt;
            pop_value(right);
            pop_value(left);
            Real result{};
            switch (node.kind) {
            case Kind::add: result = left + right; break;
            case Kind::subtract: result = left - right; break;
            case Kind::multiply: result = left * right; break;
            case Kind::divide:
                // PE-03: 0 / x is 0; x / 0 is 0 and fails the equation.
                if (left == real(0)) {
                    result = real(0);
                } else if (right == real(0)) {
                    return std::nullopt;
                } else {
                    result = left / right;
                }
                break;
            case Kind::random: result = random.uniform(to_single(left), to_single(right)); break;
            case Kind::equal: result = real(left == right ? 1 : 0); break;
            case Kind::not_equal: result = real(left == right ? 0 : 1); break;
            case Kind::less: result = real(left < right ? 1 : 0); break;
            case Kind::less_equal: result = real(left <= right ? 1 : 0); break;
            case Kind::greater: result = real(right < left ? 1 : 0); break;
            case Kind::greater_equal: result = real(right <= left ? 1 : 0); break;
            default: return std::nullopt;
            }
            push(result, false);
            break;
        }
        }
    }
    if (stack.size() != 1) return std::nullopt;
    return stack.front();
}

} // namespace eawr::script::foc::ai

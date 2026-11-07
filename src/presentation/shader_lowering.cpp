#include "eawr/presentation/shader_lowering.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <system_error>
#include <utility>

namespace eawr::presentation::shader_lowering {
namespace {
using namespace diagnostic_codes;
struct Failure final { std::string_view code; std::size_t offset; const char* message; };
[[noreturn]] void fail(std::string_view code, std::size_t offset, const char* message) {
    throw Failure{code, offset, message};
}
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
std::uint32_t mask(Type value_type) { return (1U << value_type.width) - 1U; }
bool numeric(Type value_type) { return value_type.scalar != Scalar::boolean && value_type.scalar != Scalar::sampler_2d; }
bool floating(Type value_type) { return value_type.scalar == Scalar::floating; }
std::string type_name(Type value_type) {
    switch (value_type.scalar) {
    case Scalar::floating: return value_type.width == 1U ? "float" : "vec" + std::to_string(value_type.width);
    case Scalar::signed_integer: return value_type.width == 1U ? "int" : "ivec" + std::to_string(value_type.width);
    case Scalar::unsigned_integer: return value_type.width == 1U ? "uint" : "uvec" + std::to_string(value_type.width);
    case Scalar::boolean: return value_type.width == 1U ? "bool" : "bvec" + std::to_string(value_type.width);
    case Scalar::sampler_2d: return "sampler2D";
    }
    return {};
}
std::optional<Type> named_type(std::string_view name) {
    if (name == "float") return Type{Scalar::floating, 1U};
    if (name == "int") return Type{Scalar::signed_integer, 1U};
    if (name == "uint") return Type{Scalar::unsigned_integer, 1U};
    if (name == "bool") return Type{Scalar::boolean, 1U};
    if (name == "sampler2D") return Type{Scalar::sampler_2d, 1U};
    for (const auto scalar : {Scalar::floating, Scalar::signed_integer, Scalar::unsigned_integer, Scalar::boolean})
        for (std::uint32_t width = 2U; width <= 4U; ++width)
            if (name == type_name({scalar, width})) return Type{scalar, width};
    return std::nullopt;
}
bool reserved_identifier(std::string_view name) {
    // S-18, GLSL 3.30 sections 3.6/3.7: keywords, future keywords,
    // consecutive underscores anywhere and the reserved builtin prefix.
    constexpr std::string_view words =
        " attribute const uniform varying layout centroid flat smooth noperspective break continue do for while switch case default "
        "if else in out inout float int void bool true false invariant discard return mat2 mat3 mat4 "
        "mat2x2 mat2x3 mat2x4 mat3x2 mat3x3 mat3x4 mat4x2 mat4x3 mat4x4 "
        "vec2 vec3 vec4 ivec2 ivec3 ivec4 bvec2 bvec3 bvec4 uint uvec2 uvec3 uvec4 lowp mediump highp precision "
        "sampler1D sampler2D sampler3D samplerCube sampler1DShadow sampler2DShadow samplerCubeShadow "
        "sampler1DArray sampler2DArray sampler1DArrayShadow sampler2DArrayShadow "
        "isampler1D isampler2D isampler3D isamplerCube isampler1DArray isampler2DArray "
        "usampler1D usampler2D usampler3D usamplerCube usampler1DArray usampler2DArray "
        "sampler2DRect sampler2DRectShadow isampler2DRect usampler2DRect samplerBuffer isamplerBuffer usamplerBuffer "
        "sampler2DMS isampler2DMS usampler2DMS sampler2DMSArray isampler2DMSArray usampler2DMSArray struct "
        "common partition active asm class union enum typedef template this packed goto "
        "inline noinline volatile public static extern external interface long short double half fixed unsigned superp input output "
        "hvec2 hvec3 hvec4 dvec2 dvec3 dvec4 fvec2 fvec3 fvec4 sampler3DRect filter "
        "image1D image2D image3D imageCube iimage1D iimage2D iimage3D iimageCube uimage1D uimage2D uimage3D uimageCube "
        "image1DArray image2DArray iimage1DArray iimage2DArray uimage1DArray uimage2DArray "
        "image1DShadow image2DShadow image1DArrayShadow image2DArrayShadow imageBuffer iimageBuffer uimageBuffer "
        "sizeof cast namespace using row_major ";
    return name.starts_with("gl_") || name.find("__") != std::string_view::npos || name == "main"
        || words.find(" " + std::string(name) + " ") != std::string_view::npos;
}
struct Budget final {
    const Limits& limits;
    Statistics statistics;
    void step(std::size_t amount, std::size_t offset) {
        if (amount > limits.work - statistics.work) fail(limit, offset, "lowering work budget exceeded");
        statistics.work += amount;
    }
    void depth(std::size_t value, std::size_t offset) {
        if (value > limits.depth) fail(limit, offset, "lowering nesting budget exceeded");
        statistics.maximum_depth = std::max(statistics.maximum_depth, value);
        step(1U, offset);
    }
};
enum class TokenKind { identifier, number, punctuation, end };
struct Token final { TokenKind kind; std::string_view text; std::size_t offset; };
class Lexer final {
public:
    Lexer(std::string_view input, Budget& budget) : input_(input), budget_(budget) {}
    std::vector<Token> scan() {
        if (input_.size() > budget_.limits.source_bytes) fail(limit, 0U, "lowering source byte budget exceeded");
        while (at_ < input_.size()) {
            budget_.step(1U, at_);
            const char c = input_[at_];
            if (space(c)) { if (c == '\n') line_start_ = true; ++at_; continue; }
            if (input_.substr(at_, 2U) == "//") {
                while (at_ < input_.size() && input_[at_] != '\n') advance();
                continue;
            }
            if (input_.substr(at_, 2U) == "/*") {
                const auto begin = at_; advance(); advance();
                while (at_ < input_.size() && input_.substr(at_, 2U) != "*/") {
                    if (input_[at_] == '\n') line_start_ = true;
                    advance();
                }
                if (at_ == input_.size()) fail(syntax, begin, "unterminated block comment");
                advance(); advance(); continue;
            }
            if (c == '#') { directive(); continue; }
            if (input_.substr(at_, 2U) == "++" || input_.substr(at_, 2U) == "--")
                fail(unsupported, at_, "increment and decrement are unsupported");
            line_start_ = false;
            const auto begin = at_;
            if (alpha(c)) {
                do { advance(); } while (at_ < input_.size() && (alpha(input_[at_]) || digit(input_[at_])));
                if (at_ - begin > 256U) fail(limit, begin, "identifier byte budget exceeded");
                add(TokenKind::identifier, begin); continue;
            }
            if (digit(c) || (c == '.' && at_ + 1U < input_.size() && digit(input_[at_ + 1U]))) {
                while (at_ < input_.size() && digit(input_[at_])) advance();
                bool real = false;
                if (at_ < input_.size() && input_[at_] == '.') {
                    real = true; advance();
                    while (at_ < input_.size() && digit(input_[at_])) advance();
                }
                if (at_ < input_.size() && (input_[at_] == 'e' || input_[at_] == 'E')) {
                    real = true; advance();
                    if (at_ < input_.size() && (input_[at_] == '+' || input_[at_] == '-')) advance();
                    const auto exponent = at_;
                    while (at_ < input_.size() && digit(input_[at_])) advance();
                    if (exponent == at_) fail(syntax, begin, "missing numeric exponent");
                }
                if (!real && at_ < input_.size() && input_[at_] == 'u') advance();
                if (at_ < input_.size() && (alpha(input_[at_]) || input_[at_] == '.'))
                    fail(unsupported, begin, "unsupported numeric literal");
                add(TokenKind::number, begin); continue;
            }
            constexpr std::array pairs{"==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/="};
            bool pair = false;
            for (const auto* op : pairs) if (input_.substr(at_, 2U) == op) {
                advance(); advance(); add(TokenKind::punctuation, begin); pair = true; break;
            }
            if (pair) continue;
            if (std::string_view("{}()[];,.?:+-*/%<>=!").find(c) == std::string_view::npos)
                fail(unsupported, begin, "unsupported source token");
            advance(); add(TokenKind::punctuation, begin);
        }
        if (tokens_.size() >= budget_.limits.tokens) fail(limit, at_, "lowering token budget exceeded");
        tokens_.push_back({TokenKind::end, {}, at_});
        budget_.statistics.tokens = tokens_.size();
        return std::move(tokens_);
    }
private:
    std::string_view input_;
    Budget& budget_;
    std::size_t at_{};
    bool line_start_{true}, version_{};
    std::vector<Token> tokens_;
    void advance() { budget_.step(1U, at_); ++at_; }
    void add(TokenKind kind, std::size_t begin) {
        if (tokens_.size() >= budget_.limits.tokens) fail(limit, begin, "lowering token budget exceeded");
        tokens_.push_back({kind, input_.substr(begin, at_ - begin), begin});
    }
    void directive() {
        const auto begin = at_;
        if (!line_start_ || version_ || !tokens_.empty())
            fail(unsupported, begin, "only one leading version directive is supported");
        while (at_ < input_.size() && input_[at_] != '\n') advance();
        auto line = input_.substr(begin + 1U, at_ - begin - 1U);
        const auto word = [&line]() {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) line.remove_prefix(1U);
            const auto end = line.find_first_of(" \t\r");
            const auto result = line.substr(0U, end);
            line.remove_prefix(end == std::string_view::npos ? line.size() : end);
            return result;
        };
        if (word() != "version" || word() != "330") fail(unsupported, begin, "unsupported preprocessor directive");
        const auto profile = word();
        if ((!profile.empty() && profile != "core") || !word().empty())
            fail(unsupported, begin, "unsupported version directive arguments");
        version_ = true;
    }
};

enum class Storage { input, output, uniform, local };
struct Symbol final {
    std::string name, emitted;
    Type type;
    Storage storage{};
    bool readonly{}, array{}, constant{};
    std::vector<std::size_t> uniforms;
    std::optional<std::size_t> port;
    std::uint32_t assigned{};
};
enum class NodeKind { literal, reference, unary, binary, select, call, swizzle };
struct Node final {
    NodeKind kind{};
    Type type;
    std::string_view text;
    std::size_t offset{}, symbol{}, element{};
    std::vector<std::size_t> children;
    bool constant{};
};
struct Statement final {
    bool declaration{};
    std::size_t symbol{}, left{}, right{};
    bool initialized{};
    std::string_view operation;
    std::size_t offset{};
};
int precedence(std::string_view op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == ">" || op == "<=" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    return 0;
}

// The parser owns flat node/symbol arenas. Source views never leave this call.
class Parser final {
public:
    Parser(std::vector<Token> tokens, Budget& budget, Stage stage)
        : tokens_(std::move(tokens)), budget_(budget) { result_.stage = stage; }
    LoweredStage run();
private:
    std::vector<Token> tokens_;
    Budget& budget_;
    std::size_t at_{};
    bool in_entry_{};
    std::size_t entry_offset_{}, entry_end_offset_{}, emission_offset_{};
    LoweredStage result_;
    std::vector<Symbol> symbols_;
    std::map<std::string, std::size_t, std::less<>> names_;
    std::map<std::string, bool, std::less<>> blocks_;
    std::vector<Node> nodes_;
    std::vector<Statement> statements_;
    const Token& peek() const { return tokens_[at_]; }
    Token take() {
        const auto token = peek(); budget_.step(1U, token.offset);
        if (token.kind == TokenKind::end) fail(syntax, token.offset, "unexpected end of source");
        ++at_; return token;
    }
    bool accept(std::string_view text) { if (peek().text != text) return false; static_cast<void>(take()); return true; }
    Token expect(std::string_view text) {
        if (peek().text != text) fail(syntax, peek().offset, "unexpected token");
        return take();
    }
    Token identifier() {
        const auto token = take();
        if (token.kind != TokenKind::identifier || reserved_identifier(token.text))
            fail(symbol, token.offset, "invalid or reserved declaration name");
        return token;
    }
    Type parse_type();
    std::uint32_t integer();
    Layout layout();
    std::size_t add_symbol(Token name, Type value_type, Storage storage, bool readonly = false);
    void uniform(Token name, Type value_type, Layout value_layout, std::string_view block = {}, std::string_view member = {});
    void global();
    void entry();
    std::size_t builtin(Token name);
    std::size_t add_node(Node node);
    std::size_t expression(std::size_t depth = 1U, int minimum = 1);
    std::size_t primary(std::size_t depth);
    std::size_t call(Token name, std::size_t depth);
    Type call_type(Token name, const std::vector<std::size_t>& arguments);
    Type binary_type(std::string_view op, Type left, Type right, std::size_t offset);
    void read(std::size_t node, std::uint32_t required, std::size_t depth = 1U);
    void write(std::size_t node, bool compound);
    bool one(std::size_t node, std::uint32_t component, const std::vector<std::uint32_t>& known, std::size_t depth = 1U);
    void prove_outputs();
    void emit();
    void append(std::string& out, std::string_view text);
    void emit_node(std::string& out, std::size_t node, std::size_t depth = 1U);
};

Type Parser::parse_type() {
    const auto token = take();
    const auto result = named_type(token.text);
    if (!result) fail(unsupported, token.offset, "unsupported declaration type");
    return *result;
}
std::uint32_t Parser::integer() {
    const auto token = take();
    auto text = token.text;
    if (text.ends_with('u')) text.remove_suffix(1U);
    if (text.size() > 1U && text.front() == '0') fail(unsupported, token.offset, "non-decimal integer spelling is unsupported");
    std::uint32_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (token.kind != TokenKind::number || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(unsupported, token.offset, "expected a bounded unsigned integer literal");
    return result;
}
Layout Parser::layout() {
    Layout value;
    if (!accept("layout")) return value;
    expect("(");
    do {
        const auto key = take();
        budget_.step(1U, key.offset);
        if (key.text == "std140") {
            if (value.std140) fail(symbol, key.offset, "duplicate layout property");
            value.std140 = true;
        } else {
            std::optional<std::uint32_t>* destination = nullptr;
            if (key.text == "location") destination = &value.location;
            else if (key.text == "component") destination = &value.component;
            else if (key.text == "binding") destination = &value.binding;
            else if (key.text == "set") destination = &value.descriptor_set;
            else fail(unsupported, key.offset, "unsupported layout property");
            if (*destination) fail(symbol, key.offset, "duplicate layout property");
            expect("="); *destination = integer();
        }
    } while (accept(","));
    expect(")");
    if (value.component && *value.component > 3U) fail(type, peek().offset, "invalid layout component");
    return value;
}
std::size_t Parser::add_symbol(Token name, Type value_type, Storage storage, bool readonly) {
    budget_.step(1U, name.offset);
    if (symbols_.size() >= budget_.limits.symbols) fail(limit, name.offset, "lowering symbol budget exceeded");
    if (names_.contains(name.text) || blocks_.contains(name.text)) fail(symbol, name.offset, "duplicate declaration");
    const auto index = symbols_.size();
    Symbol value;
    value.name = name.text; value.type = value_type; value.storage = storage; value.readonly = readonly;
    value.emitted = "eawr_l" + std::to_string(index);
    value.assigned = storage == Storage::input || storage == Storage::uniform ? mask(value_type) : 0U;
    names_.emplace(value.name, index); symbols_.push_back(std::move(value));
    return index;
}
void Parser::uniform(Token name, Type value_type, Layout value_layout, std::string_view block, std::string_view member) {
    bool array = accept("[");
    const auto count = array ? integer() : 1U;
    if (array) expect("]");
    if (count == 0U || count > budget_.limits.uniforms - result_.uniforms.size())
        fail(limit, name.offset, "lowering uniform element budget exceeded");
    if (array && value_type.scalar == Scalar::sampler_2d) fail(unsupported, name.offset, "sampler arrays require a later resource policy");
    const auto index = add_symbol(name, value_type, Storage::uniform, true);
    symbols_[index].array = array;
    for (std::uint32_t element = 0U; element < count; ++element) {
        budget_.step(1U, name.offset);
        Uniform value;
        value.source_name = name.text; value.block_name = block; value.member_name = member;
        value.type = value_type; value.layout = value_layout;
        if (array) value.element = element;
        value.emitted_name = (result_.stage == Stage::vertex ? "eawr_vu" : "eawr_fu") + std::to_string(result_.uniforms.size());
        symbols_[index].uniforms.push_back(result_.uniforms.size());
        result_.uniforms.push_back(std::move(value));
    }
    expect(";");
}
void Parser::global() {
    const auto value_layout = layout();
    Interpolation interpolation = Interpolation::smooth;
    bool centroid = accept("centroid");
    if (accept("flat")) interpolation = Interpolation::flat;
    else if (accept("noperspective")) interpolation = Interpolation::no_perspective;
    else static_cast<void>(accept("smooth"));
    Storage storage = Storage::local;
    if (accept("in")) storage = Storage::input;
    else if (accept("out")) storage = Storage::output;
    else if (accept("uniform")) storage = Storage::uniform;
    const bool constant = accept("const");
    if (constant && storage != Storage::local) fail(unsupported, peek().offset, "unsupported declaration qualifier combination");
    if (storage == Storage::uniform && peek().kind == TokenKind::identifier
        && at_ + 1U < tokens_.size() && tokens_[at_ + 1U].text == "{") {
        if (centroid || interpolation != Interpolation::smooth || value_layout.location || value_layout.component)
            fail(type, peek().offset, "invalid uniform block qualifiers");
        const auto block_type = identifier(); expect("{");
        struct Member final { Token name; Type value_type; bool array; std::uint32_t count; };
        std::vector<Member> members;
        while (peek().text != "}") {
            budget_.step(1U, peek().offset);
            if (members.size() >= budget_.limits.symbols) fail(limit, peek().offset, "block member budget exceeded");
            const auto value_type = parse_type();
            if (value_type.scalar == Scalar::sampler_2d) fail(unsupported, peek().offset, "opaque uniform block member");
            const auto name = identifier();
            budget_.step(members.size(), name.offset);
            if (std::any_of(members.begin(), members.end(), [&](const Member& member) { return member.name.text == name.text; }))
                fail(symbol, name.offset, "duplicate block member");
            const bool array = accept("["); const auto count = array ? integer() : 1U;
            if (array) expect("]");
            if (count == 0U || count > budget_.limits.uniforms) fail(limit, name.offset, "block array extent budget exceeded");
            expect(";"); members.push_back({name, value_type, array, count});
        }
        expect("}"); const auto instance = identifier(); expect(";");
        if (members.empty()) fail(type, block_type.offset, "empty uniform block");
        if (blocks_.contains(instance.text) || names_.contains(instance.text)) fail(symbol, instance.offset, "duplicate block instance");
        blocks_.emplace(std::string(instance.text), true);
        for (const auto& member : members) {
            budget_.step(1U, member.name.offset);
            if (member.count > budget_.limits.uniforms - result_.uniforms.size())
                fail(limit, member.name.offset, "lowering uniform element budget exceeded");
            const auto key = std::string(instance.text) + "." + std::string(member.name.text);
            const Token key_token{TokenKind::identifier, key, member.name.offset};
            const auto index = add_symbol(key_token, member.value_type, Storage::uniform, true);
            symbols_[index].array = member.array;
            for (std::uint32_t element = 0U; element < member.count; ++element) {
                budget_.step(1U, member.name.offset);
                Uniform value;
                value.source_name = instance.text; value.block_name = block_type.text; value.member_name = member.name.text;
                value.type = member.value_type; value.layout = value_layout;
                if (member.array) value.element = element;
                value.emitted_name = (result_.stage == Stage::vertex ? "eawr_vu" : "eawr_fu") + std::to_string(result_.uniforms.size());
                symbols_[index].uniforms.push_back(result_.uniforms.size());
                result_.uniforms.push_back(std::move(value));
            }
        }
        return;
    }
    const auto value_type = parse_type(); const auto name = identifier();
    if (storage == Storage::uniform) {
        if (centroid || interpolation != Interpolation::smooth || value_layout.std140 || value_layout.location || value_layout.component)
            fail(type, name.offset, "invalid uniform qualifiers");
        uniform(name, value_type, value_layout); return;
    }
    if (value_type.scalar == Scalar::sampler_2d) fail(type, name.offset, "opaque type requires uniform storage");
    if (storage == Storage::local) {
        if (centroid || interpolation != Interpolation::smooth || value_layout.location || value_layout.component
            || value_layout.binding || value_layout.descriptor_set || value_layout.std140)
            fail(type, name.offset, "invalid local qualifiers");
        const auto index = add_symbol(name, value_type, storage, constant);
        const bool initialized = accept("="); std::size_t right{};
        if (initialized) {
            right = expression();
            if (nodes_[right].type != value_type) fail(type, name.offset, "initializer type mismatch");
            if ((constant || !in_entry_) && !nodes_[right].constant)
                fail(type, name.offset, "initializer must be constant in this context");
            read(right, mask(value_type)); symbols_[index].assigned = mask(value_type);
            symbols_[index].constant = constant;
        } else if (constant) fail(syntax, name.offset, "constant requires an initializer");
        expect(";"); statements_.push_back({true, index, 0U, right, initialized, {}, name.offset}); return;
    }
    if (peek().text == "[") fail(unsupported, name.offset, "stage interface arrays are unsupported");
    if (value_layout.binding || value_layout.descriptor_set || value_layout.std140)
        fail(type, name.offset, "invalid stage interface layout");
    if (value_layout.component && (!value_layout.location || *value_layout.component + value_type.width > 4U))
        fail(type, name.offset, "stage interface component range is invalid");
    const auto index = add_symbol(name, value_type, storage);
    Port port;
    port.source_name = name.text; port.emitted_name = "eawr_p" + std::to_string(result_.ports.size());
    port.type = value_type; port.direction = storage == Storage::input ? Direction::input : Direction::output;
    port.layout = value_layout; port.interpolation = interpolation; port.centroid = centroid;
    for (const auto& previous : result_.ports) {
        budget_.step(1U, name.offset);
        if (port.layout.location && previous.layout.location == port.layout.location && previous.direction == port.direction) {
            const auto a = mask(port.type) << port.layout.component.value_or(0U);
            const auto b = mask(previous.type) << previous.layout.component.value_or(0U);
            if ((a & b) != 0U) fail(symbol, name.offset, "overlapping stage interface locations");
        }
    }
    symbols_[index].emitted = port.emitted_name; symbols_[index].port = result_.ports.size();
    result_.ports.push_back(std::move(port)); expect(";");
}
std::size_t Parser::builtin(Token name) {
    Type value_type; Builtin kind{}; Storage storage{}; Stage stage{};
    if (name.text == "gl_Position") { value_type = {Scalar::floating, 4U}; kind = Builtin::position; storage = Storage::output; stage = Stage::vertex; }
    else if (name.text == "gl_PointSize") { kind = Builtin::point_size; storage = Storage::output; stage = Stage::vertex; }
    else if (name.text == "gl_VertexID" || name.text == "gl_InstanceID") {
        value_type.scalar = Scalar::signed_integer; storage = Storage::input; stage = Stage::vertex;
        kind = name.text == "gl_VertexID" ? Builtin::vertex_index : Builtin::instance_index;
    } else if (name.text == "gl_FragCoord") {
        value_type.width = 4U; kind = Builtin::fragment_coordinate; storage = Storage::input; stage = Stage::fragment;
    } else if (name.text == "gl_FrontFacing") {
        value_type.scalar = Scalar::boolean; kind = Builtin::front_facing; storage = Storage::input; stage = Stage::fragment;
    } else if (name.text == "gl_FragDepth") { kind = Builtin::fragment_depth; storage = Storage::output; stage = Stage::fragment; }
    else fail(symbol, name.offset, "undeclared source identifier");
    if (result_.stage != stage) fail(unsupported, name.offset, "builtin is unavailable in this stage");
    const auto index = add_symbol(name, value_type, storage);
    Port port;
    port.source_name = name.text; port.emitted_name = "eawr_p" + std::to_string(result_.ports.size());
    port.type = value_type; port.builtin = kind;
    port.direction = storage == Storage::input ? Direction::input : Direction::output;
    symbols_[index].emitted = port.emitted_name; symbols_[index].port = result_.ports.size();
    result_.ports.push_back(std::move(port)); return index;
}
std::size_t Parser::add_node(Node node) {
    budget_.step(1U, node.offset);
    if (nodes_.size() >= budget_.limits.nodes) fail(limit, node.offset, "lowering node budget exceeded");
    if (node.kind == NodeKind::literal) node.constant = true;
    else if (node.kind == NodeKind::reference) node.constant = symbols_[node.symbol].constant;
    else {
        budget_.step(node.children.size(), node.offset);
        node.constant = !(node.kind == NodeKind::call && node.text == "texture")
            && std::all_of(node.children.begin(), node.children.end(), [&](std::size_t child) { return nodes_[child].constant; });
    }
    const auto index = nodes_.size(); nodes_.push_back(std::move(node)); return index;
}

Type Parser::binary_type(std::string_view op, Type left, Type right, std::size_t offset) {
    if (op == "&&" || op == "||") {
        if (left != Type{Scalar::boolean, 1U} || right != left) fail(type, offset, "logical operator requires scalar booleans");
        return left;
    }
    if (op == "==" || op == "!=") {
        if (left != right || left.scalar == Scalar::sampler_2d) fail(type, offset, "equality operand type mismatch");
        return {Scalar::boolean, 1U};
    }
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        if (!numeric(left) || left.width != 1U || left != right) fail(type, offset, "comparison requires matching numeric scalars");
        return {Scalar::boolean, 1U};
    }
    if (!numeric(left) || !numeric(right) || left.scalar != right.scalar
        || (left.width != right.width && left.width != 1U && right.width != 1U))
        fail(type, offset, "arithmetic operand type mismatch");
    if (op == "%" && floating(left)) fail(type, offset, "remainder requires integers");
    return {left.scalar, std::max(left.width, right.width)};
}
Type Parser::call_type(Token name, const std::vector<std::size_t>& arguments) {
    const auto at = [&](std::size_t index) { return nodes_[arguments[index]].type; };
    const auto arity = [&](std::size_t count) {
        if (arguments.size() != count) fail(type, name.offset, "intrinsic argument count mismatch");
    };
    if (const auto target = named_type(name.text)) {
        if (target->scalar == Scalar::sampler_2d || arguments.empty()) fail(type, name.offset, "invalid constructor");
        std::uint32_t components{};
        for (const auto argument : arguments) {
            const auto argument_type = nodes_[argument].type;
            if (argument_type.scalar == Scalar::sampler_2d) fail(type, name.offset, "opaque constructor operand");
            components += argument_type.width;
        }
        if (!(arguments.size() == 1U && at(0U).width == 1U) && components != target->width)
            fail(type, name.offset, "constructor component count mismatch");
        return *target;
    }
    if (name.text == "texture") {
        arity(2U);
        if (at(0U) != Type{Scalar::sampler_2d, 1U} || at(1U) != Type{Scalar::floating, 2U})
            fail(type, name.offset, "texture requires sampler2D and vec2");
        return {Scalar::floating, 4U};
    }
    if (name.text == "dot") {
        arity(2U);
        if (!floating(at(0U)) || at(0U).width < 2U || at(0U) != at(1U)) fail(type, name.offset, "dot operand type mismatch");
        return {Scalar::floating, 1U};
    }
    if (name.text == "all" || name.text == "any" || name.text == "not") {
        arity(1U);
        if (at(0U).scalar != Scalar::boolean || at(0U).width < 2U) fail(type, name.offset, "boolean vector intrinsic type mismatch");
        return name.text == "not" ? at(0U) : Type{Scalar::boolean, 1U};
    }
    if (name.text == "equal" || name.text == "notEqual" || name.text == "lessThan" || name.text == "lessThanEqual"
        || name.text == "greaterThan" || name.text == "greaterThanEqual") {
        arity(2U);
        const bool equality = name.text == "equal" || name.text == "notEqual";
        if (at(0U) != at(1U) || at(0U).width < 2U || at(0U).scalar == Scalar::sampler_2d || (!equality && !numeric(at(0U))))
            fail(type, name.offset, "vector comparison operand type mismatch");
        return {Scalar::boolean, at(0U).width};
    }
    if (name.text == "min" || name.text == "max" || name.text == "clamp") {
        arity(name.text == "clamp" ? 3U : 2U);
        if (!numeric(at(0U))) fail(type, name.offset, "numeric intrinsic requires numeric arguments");
        for (std::size_t i = 1U; i < arguments.size(); ++i)
            if (at(i).scalar != at(0U).scalar || (at(i).width != 1U && at(i).width != at(0U).width))
                fail(type, name.offset, "numeric intrinsic operand type mismatch");
        if (arguments.size() == 3U && at(1U) != at(2U)) fail(type, name.offset, "clamp limit type mismatch");
        return at(0U);
    }
    if (name.text == "pow" || name.text == "mix") {
        arity(name.text == "mix" ? 3U : 2U);
        if (!floating(at(0U)) || at(0U) != at(1U)) fail(type, name.offset, "floating intrinsic operand type mismatch");
        if (arguments.size() == 3U && (!floating(at(2U)) || (at(2U).width != 1U && at(2U).width != at(0U).width)))
            fail(type, name.offset, "mix factor type mismatch");
        return at(0U);
    }
    constexpr std::array unary{"normalize", "length", "inversesqrt", "sqrt", "abs", "floor", "fract", "exp2", "log2", "isnan"};
    if (std::find(unary.begin(), unary.end(), name.text) != unary.end()) {
        arity(1U);
        if (!(floating(at(0U)) || (name.text == "abs" && at(0U).scalar == Scalar::signed_integer)))
            fail(type, name.offset, "unary intrinsic operand type mismatch");
        if (name.text == "isnan") return {Scalar::boolean, at(0U).width};
        return name.text == "length" ? Type{Scalar::floating, 1U} : at(0U);
    }
    fail(unsupported, name.offset, "unsupported function call");
}
std::size_t Parser::call(Token name, std::size_t depth) {
    if (names_.contains(name.text) || blocks_.contains(name.text)) fail(symbol, name.offset, "call target is a variable");
    expect("("); std::vector<std::size_t> arguments;
    if (peek().text != ")") do {
        if (arguments.size() >= 16U) fail(limit, peek().offset, "call argument budget exceeded");
        arguments.push_back(expression(depth + 1U));
    } while (accept(","));
    expect(")");
    const auto value_type = call_type(name, arguments);
    return add_node({NodeKind::call, value_type, name.text, name.offset, 0U, 0U, std::move(arguments)});
}
std::size_t Parser::primary(std::size_t depth) {
    budget_.depth(depth, peek().offset);
    const auto token = take();
    std::size_t node{};
    if (token.text == "(") { node = expression(depth + 1U); expect(")"); }
    else if (token.text == "+" || token.text == "-" || token.text == "!") {
        const auto child = primary(depth + 1U); const auto value_type = nodes_[child].type;
        if (token.text == "!" ? value_type != Type{Scalar::boolean, 1U} : !numeric(value_type))
            fail(type, token.offset, "unary operator type mismatch");
        node = add_node({NodeKind::unary, value_type, token.text, token.offset, 0U, 0U, {child}});
    } else if (token.kind == TokenKind::number || token.text == "true" || token.text == "false") {
        Type value_type;
        if (token.kind != TokenKind::number) value_type.scalar = Scalar::boolean;
        else if (token.text.find_first_of(".eE") != std::string_view::npos) {
            float value{};
            const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != token.text.data() + token.text.size() || !std::isfinite(value))
                fail(unsupported, token.offset, "floating literal is outside the supported range");
        } else {
            auto text = token.text;
            const bool unsigned_value = text.ends_with('u'); if (unsigned_value) text.remove_suffix(1U);
            if (text.size() > 1U && text.front() == '0') fail(unsupported, token.offset, "non-decimal integer spelling is unsupported");
            std::uint32_t value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                || (!unsigned_value && value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())))
                fail(unsupported, token.offset, "integer literal is outside the supported range");
            value_type.scalar = unsigned_value ? Scalar::unsigned_integer : Scalar::signed_integer;
        }
        node = add_node({NodeKind::literal, value_type, token.text, token.offset, 0U, 0U, {}});
    } else if (token.kind == TokenKind::identifier) {
        if (peek().text == "(") node = call(token, depth);
        else {
            std::string key(token.text);
            if (blocks_.contains(key)) { expect("."); key += "."; key += identifier().text; }
            const auto found = names_.find(key);
            const auto index = found != names_.end() ? found->second : builtin(token);
            std::size_t element{};
            if (symbols_[index].array) {
                if (!accept("[")) fail(unsupported, token.offset, "whole uniform array expressions are unsupported");
                element = integer(); expect("]");
                if (element >= symbols_[index].uniforms.size()) fail(type, token.offset, "uniform array index is out of range");
            }
            node = add_node({NodeKind::reference, symbols_[index].type, {}, token.offset, index, element, {}});
        }
    } else fail(syntax, token.offset, "expected an expression");
    while (accept(".")) {
        budget_.depth(++depth, peek().offset);
        const auto field = take(); const auto parent_type = nodes_[node].type;
        if (field.kind != TokenKind::identifier || field.text.empty() || field.text.size() > 4U
            || parent_type.width == 1U || parent_type.scalar == Scalar::sampler_2d)
            fail(type, field.offset, "invalid vector swizzle");
        std::string_view alphabet;
        for (const auto group : {std::string_view("xyzw"), std::string_view("rgba"), std::string_view("stpq")})
            if (group.find(field.text.front()) != std::string_view::npos) alphabet = group;
        for (const char character : field.text)
            if (alphabet.find(character) >= parent_type.width) fail(type, field.offset, "invalid swizzle component");
        node = add_node({NodeKind::swizzle, {parent_type.scalar, static_cast<std::uint32_t>(field.text.size())},
                         field.text, field.offset, 0U, 0U, {node}});
    }
    if (peek().text == "[") fail(unsupported, peek().offset, "dynamic or non-uniform indexing is unsupported");
    return node;
}
std::size_t Parser::expression(std::size_t depth, int minimum) {
    budget_.depth(depth, peek().offset);
    auto left = primary(depth + 1U);
    while (precedence(peek().text) >= minimum && precedence(peek().text) != 0) {
        const auto op = take(); const auto right = expression(depth + 1U, precedence(op.text) + 1);
        const auto value_type = binary_type(op.text, nodes_[left].type, nodes_[right].type, op.offset);
        left = add_node({NodeKind::binary, value_type, op.text, op.offset, 0U, 0U, {left, right}});
    }
    if (minimum == 1 && accept("?")) {
        const auto yes = expression(depth + 1U); expect(":"); const auto no = expression(depth + 1U);
        if (nodes_[left].type != Type{Scalar::boolean, 1U} || nodes_[yes].type != nodes_[no].type
            || nodes_[yes].type.scalar == Scalar::sampler_2d) fail(type, nodes_[left].offset, "conditional operand type mismatch");
        left = add_node({NodeKind::select, nodes_[yes].type, {}, nodes_[left].offset, 0U, 0U, {left, yes, no}});
    }
    return left;
}

void Parser::read(std::size_t index, std::uint32_t required, std::size_t depth) {
    const auto& node = nodes_[index]; budget_.depth(depth, node.offset);
    if (node.kind == NodeKind::reference) {
        auto& source = symbols_[node.symbol];
        if ((source.assigned & required) != required) fail(symbol, node.offset, "read of an undefined component");
        if (source.port) result_.ports[*source.port].read_mask |= required;
        if (source.storage == Storage::uniform)
            result_.uniforms[source.uniforms[node.element]].read_mask |= required;
    } else if (node.kind == NodeKind::swizzle) {
        std::string_view alphabet;
        for (const auto group : {std::string_view("xyzw"), std::string_view("rgba"), std::string_view("stpq")})
            if (group.find(node.text.front()) != std::string_view::npos) alphabet = group;
        std::uint32_t selected{};
        for (std::size_t i = 0U; i < node.text.size(); ++i)
            if ((required & (1U << i)) != 0U) selected |= 1U << alphabet.find(node.text[i]);
        read(node.children[0U], selected, depth + 1U);
    } else {
        for (const auto child : node.children) read(child, mask(nodes_[child].type), depth + 1U);
    }
}
void Parser::write(std::size_t index, bool compound) {
    std::uint32_t selected = mask(nodes_[index].type);
    auto current = index;
    std::size_t depth = 1U;
    while (nodes_[current].kind == NodeKind::swizzle) {
        const auto& node = nodes_[current]; budget_.depth(depth++, node.offset);
        std::string_view alphabet;
        for (const auto group : {std::string_view("xyzw"), std::string_view("rgba"), std::string_view("stpq")})
            if (group.find(node.text.front()) != std::string_view::npos) alphabet = group;
        std::uint32_t all{}, mapped{};
        for (std::size_t i = 0U; i < node.text.size(); ++i) {
            const auto bit = 1U << alphabet.find(node.text[i]);
            if ((all & bit) != 0U) fail(type, node.offset, "duplicate writable swizzle component");
            all |= bit;
            if ((selected & (1U << i)) != 0U) mapped |= bit;
        }
        selected = mapped; current = node.children[0U];
    }
    const auto& node = nodes_[current];
    if (node.kind != NodeKind::reference) fail(symbol, node.offset, "assignment requires a writable variable");
    auto& target = symbols_[node.symbol];
    if (target.readonly || target.storage == Storage::input) fail(symbol, node.offset, "assignment to readonly storage");
    if (compound) read(index, mask(nodes_[index].type));
    target.assigned |= selected;
    if (target.port) result_.ports[*target.port].written_mask |= selected;
}
void Parser::entry() {
    entry_offset_ = expect("void").offset;
    expect("main"); expect("("); static_cast<void>(accept("void")); expect(")"); expect("{");
    in_entry_ = true;
    while (peek().text != "}") {
        budget_.step(1U, peek().offset);
        if (peek().kind == TokenKind::end) fail(syntax, peek().offset, "unterminated entry body");
        if (accept("return")) { expect(";"); if (peek().text != "}") fail(unsupported, peek().offset, "early return is unsupported"); break; }
        constexpr std::array control{"if", "else", "for", "while", "do", "switch", "discard", "break", "continue"};
        if (std::find(control.begin(), control.end(), peek().text) != control.end())
            fail(unsupported, peek().offset, "control flow or discard is unsupported");
        if (named_type(peek().text) || peek().text == "const") { global(); continue; }
        const auto left = expression(); const auto operation = take();
        if (operation.text != "=" && operation.text != "+=" && operation.text != "-=" && operation.text != "*=" && operation.text != "/=")
            fail(unsupported, operation.offset, "unsupported entry statement");
        const auto right = expression();
        const auto target_type = nodes_[left].type;
        if (operation.text == "=" ? target_type != nodes_[right].type
            : binary_type(operation.text.substr(0U, 1U), target_type, nodes_[right].type, operation.offset) != target_type)
            fail(type, operation.offset, "assignment type mismatch");
        read(right, mask(nodes_[right].type)); write(left, operation.text != "="); expect(";");
        statements_.push_back({false, 0U, left, right, true, operation.text, operation.offset});
    }
    entry_end_offset_ = expect("}").offset;
    if (peek().kind != TokenKind::end) fail(unsupported, peek().offset, "trailing declarations or additional entries are unsupported");
}
bool Parser::one(std::size_t index, std::uint32_t component, const std::vector<std::uint32_t>& known, std::size_t depth) {
    const auto& node = nodes_[index]; budget_.depth(depth, node.offset);
    if (node.kind == NodeKind::literal) {
        auto text = node.text;
        if (text.ends_with('u')) text.remove_suffix(1U);
        double value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value == 1.0;
    }
    if (node.kind == NodeKind::reference) return (known[node.symbol] & (1U << component)) != 0U;
    if (node.kind == NodeKind::unary && node.text == "+") return one(node.children[0U], component, known, depth + 1U);
    if (node.kind == NodeKind::swizzle) {
        for (const auto alphabet : {std::string_view("xyzw"), std::string_view("rgba"), std::string_view("stpq")})
            if (alphabet.find(node.text.front()) != std::string_view::npos)
                return one(node.children[0U], static_cast<std::uint32_t>(alphabet.find(node.text[component])), known, depth + 1U);
    }
    if (node.kind == NodeKind::call && named_type(node.text)) {
        if (node.children.size() == 1U && nodes_[node.children[0U]].type.width == 1U)
            return one(node.children[0U], 0U, known, depth + 1U);
        for (const auto child : node.children) {
            const auto width = nodes_[child].type.width;
            if (component < width) return one(child, component, known, depth + 1U);
            component -= width;
        }
    }
    // No approximate arithmetic, parameter defaults, or sampled values form
    // part of this proof. A select is safe only when both alternatives agree.
    if (node.kind == NodeKind::select)
        return one(node.children[1U], component, known, depth + 1U)
            && one(node.children[2U], component, known, depth + 1U);
    return false;
}
void Parser::prove_outputs() {
    std::vector<std::uint32_t> known(symbols_.size());
    for (const auto& statement : statements_) {
        const auto right_type = statement.initialized ? nodes_[statement.right].type : Type{};
        std::uint32_t value{};
        if (statement.initialized) for (std::uint32_t c = 0U; c < right_type.width; ++c)
            if (one(statement.right, c, known)) value |= 1U << c;
        if (statement.declaration) { known[statement.symbol] = value; continue; }
        for (std::uint32_t c = 0U; c < nodes_[statement.left].type.width; ++c) {
            auto index = statement.left; auto component = c;
            while (nodes_[index].kind == NodeKind::swizzle) {
                const auto& swizzle = nodes_[index]; budget_.step(1U, swizzle.offset);
                for (const auto alphabet : {std::string_view("xyzw"), std::string_view("rgba"), std::string_view("stpq")})
                    if (alphabet.find(swizzle.text.front()) != std::string_view::npos) {
                        component = static_cast<std::uint32_t>(alphabet.find(swizzle.text[component])); break;
                    }
                index = swizzle.children[0U];
            }
            auto& target = known[nodes_[index].symbol];
            target &= ~(1U << component);
            if (statement.operation == "=" && (value & (1U << c))) target |= 1U << component;
        }
    }
    for (std::size_t i = 0U; i < symbols_.size(); ++i)
        if (symbols_[i].port) result_.ports[*symbols_[i].port].proven_one_mask = known[i];
}
void Parser::append(std::string& out, std::string_view text) {
    if (text.size() > budget_.limits.emitted_bytes - out.size()) fail(limit, emission_offset_, "lowering output byte budget exceeded");
    budget_.step(text.size(), emission_offset_); out.append(text);
}
void Parser::emit_node(std::string& out, std::size_t index, std::size_t depth) {
    const auto& node = nodes_[index]; budget_.depth(depth, node.offset);
    struct Position final {
        std::size_t& current;
        std::size_t previous;
        Position(std::size_t& value, std::size_t next) : current(value), previous(value) { current = next; }
        ~Position() { current = previous; }
    } position(emission_offset_, node.offset);
    switch (node.kind) {
    case NodeKind::literal: append(out, node.text); return;
    case NodeKind::reference: {
        const auto& source = symbols_[node.symbol];
        append(out, source.storage == Storage::uniform
            ? result_.uniforms[source.uniforms[node.element]].emitted_name : source.emitted);
        return;
    }
    case NodeKind::unary:
        append(out, "("); append(out, node.text); emit_node(out, node.children[0U], depth + 1U); append(out, ")"); return;
    case NodeKind::binary:
        append(out, "("); emit_node(out, node.children[0U], depth + 1U); append(out, " "); append(out, node.text);
        append(out, " "); emit_node(out, node.children[1U], depth + 1U); append(out, ")"); return;
    case NodeKind::select:
        append(out, "("); emit_node(out, node.children[0U], depth + 1U); append(out, " ? ");
        emit_node(out, node.children[1U], depth + 1U); append(out, " : ");
        emit_node(out, node.children[2U], depth + 1U); append(out, ")"); return;
    case NodeKind::call:
        append(out, node.text); append(out, "(");
        for (std::size_t i = 0U; i < node.children.size(); ++i) {
            if (i != 0U) append(out, ", ");
            emit_node(out, node.children[i], depth + 1U);
        }
        append(out, ")"); return;
    case NodeKind::swizzle:
        append(out, "("); emit_node(out, node.children[0U], depth + 1U); append(out, ").");
        // Normalize swizzle alphabets without introducing source identifiers.
        for (const auto group : {std::string_view("xyzw"), std::string_view("rgba"), std::string_view("stpq")}) {
            if (group.find(node.text.front()) == std::string_view::npos) continue;
            for (const auto character : node.text) append(out, std::string_view("xyzw").substr(group.find(character), 1U));
            break;
        }
        return;
    }
}
void Parser::emit() {
    result_.function_name = result_.stage == Stage::vertex ? "eawr_lower_vertex" : "eawr_lower_fragment";
    auto& out = result_.function_source;
    emission_offset_ = entry_offset_;
    append(out, "void "); append(out, result_.function_name); append(out, "(");
    for (std::size_t i = 0U; i < result_.ports.size(); ++i) {
        const auto& port = result_.ports[i];
        if (i != 0U) append(out, ", ");
        append(out, port.direction == Direction::input ? "in " : "out ");
        append(out, type_name(port.type)); append(out, " "); append(out, port.emitted_name);
    }
    append(out, ") {\n");
    for (const auto& statement : statements_) {
        emission_offset_ = statement.offset;
        budget_.step(1U, emission_offset_); append(out, "    ");
        if (statement.declaration) {
            const auto& declared = symbols_[statement.symbol];
            append(out, type_name(declared.type)); append(out, " "); append(out, declared.emitted);
            if (statement.initialized) { append(out, " = "); emit_node(out, statement.right); }
        } else {
            emit_node(out, statement.left); append(out, " "); append(out, statement.operation); append(out, " "); emit_node(out, statement.right);
        }
        append(out, ";\n");
    }
    emission_offset_ = entry_end_offset_; append(out, "}\n");
    budget_.statistics.symbols = symbols_.size(); budget_.statistics.nodes = nodes_.size();
    budget_.statistics.emitted_bytes = out.size(); result_.statistics = budget_.statistics;
}
LoweredStage Parser::run() {
    if (result_.stage != Stage::vertex && result_.stage != Stage::fragment) fail(unsupported, 0U, "unsupported shader stage");
    while (peek().kind != TokenKind::end && peek().text != "void") { budget_.step(1U, peek().offset); global(); }
    if (peek().kind == TokenKind::end) fail(syntax, peek().offset, "missing shader entry");
    entry(); prove_outputs(); emit(); return std::move(result_);
}

} // namespace

core::Result<LoweredStage> lower_stage(std::string_view source, Stage stage, std::string_view logical_source, Limits limits) {
    try {
        if (logical_source.size() > 512U) fail(diagnostic_codes::limit, 0U, "logical identity byte budget exceeded");
        Budget budget{limits, {}};
        auto tokens = Lexer(source, budget).scan();
        return core::Result<LoweredStage>::success(Parser(std::move(tokens), budget, stage).run());
    } catch (const Failure& failure) {
        core::Diagnostic diagnostic;
        diagnostic.code = failure.code; diagnostic.message = failure.message;
        if (!logical_source.empty() && logical_source.size() <= 512U) diagnostic.logical_path = logical_source;
        std::uint64_t line = 1U, column = 1U;
        // Even the refusal path is bounded independently of an over-budget input.
        const auto end = std::min({failure.offset, source.size(), limits.source_bytes});
        for (std::size_t i = 0U; i < end; ++i) {
            if (source[i] == '\n') { ++line; column = 1U; } else ++column;
        }
        diagnostic.line = line; diagnostic.column = column;
        return core::Result<LoweredStage>::failure(std::move(diagnostic));
    }
}
} // namespace eawr::presentation::shader_lowering

#include "eawr/presentation/shader_effect_source.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>

namespace eawr::presentation::shader_source {
namespace {
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
struct Refusal { Diagnostic diagnostic; };
[[noreturn]] void refuse(Failure reason, const Location& location) {
    throw Refusal{Diagnostic{reason, location, {}}};
}
struct Token {
    std::string text;
    std::size_t begin{}, end{}, mate{std::numeric_limits<std::size_t>::max()};
    Location location;
};
class Parser {
public:
    Parser(std::span<const std::byte> bytes, assets::Source root,
        std::span<const assets::Source> inputs, Limits limits) : limits_(limits) {
        graph_.source = std::move(root);
        if (bytes.size() > limits.bytes) refuse(Failure::budget, {graph_.source.logical_path, {}, {}});
        text_.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        stage_ = text_;
        known_.insert(graph_.source.logical_path);
        for (const auto& input : inputs) known_.insert(input.logical_path);
    }
    Graph run() {
        lex();
        while (cursor_ < tokens_.size()) {
            if (at(";")) { ++cursor_; continue; }
            if (at("technique")) { technique(); continue; }
            if (at("struct")) {
                const auto start = cursor_;
                ++cursor_; name(); expect("{"); cursor_ = tokens_[cursor_ - 1].mate + 1;
                expect(";");
                if (cursor_ <= start) fail();
                continue;
            }
            declaration();
        }
        if (graph_.techniques.empty()) fail();
        collect_stages();
        graph_.stage_text.assign(reinterpret_cast<const std::byte*>(stage_.data()),
            reinterpret_cast<const std::byte*>(stage_.data() + stage_.size()));
        return std::move(graph_);
    }
private:
    Limits limits_;
    Graph graph_;
    std::string text_, stage_;
    std::set<std::string> known_;
    std::vector<Token> tokens_;
    std::size_t cursor_{}, nodes_{};
    Location location() const {
        return cursor_ < tokens_.size() ? tokens_[cursor_].location : Location{graph_.source.logical_path, {}, {}};
    }
    [[noreturn]] void fail(Failure reason = Failure::syntax) const { refuse(reason, location()); }
    void node() { if (++nodes_ > limits_.nodes) fail(Failure::budget); }
    bool at(std::string_view text) const { return cursor_ < tokens_.size() && lower(tokens_[cursor_].text) == text; }
    void expect(std::string_view text) { if (!at(text)) fail(); ++cursor_; }
    std::string name() {
        if (cursor_ >= tokens_.size() || tokens_[cursor_].text.empty() || !alpha(tokens_[cursor_].text.front())) fail();
        return tokens_[cursor_++].text;
    }
    void blank(std::size_t first, std::size_t last) {
        for (auto i = first; i < last; ++i) if (stage_[i] != '\n' && stage_[i] != '\r') stage_[i] = ' ';
    }
    std::string slice(std::size_t first, std::size_t last) const {
        if (first == last) return {};
        return text_.substr(tokens_[first].begin, tokens_[last - 1].end - tokens_[first].begin);
    }
    void lex() {
        std::string logical = graph_.source.logical_path;
        std::optional<std::uint32_t> line{1};
        std::uint32_t column = 1;
        std::size_t i{};
        bool code{};
        std::vector<std::size_t> stack;
        const auto advance = [&](char c) {
            if (c == '\n') { if (line) ++*line; column = 1; code = false; }
            else ++column;
        };
        while (i < text_.size()) {
            const auto c = text_[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(c); ++i; continue; }
            const Location loc{logical, line, line ? std::optional(column) : std::nullopt};
            if (c == '/' && i + 1 < text_.size() && text_[i + 1] == '/') {
                while (i < text_.size() && text_[i] != '\n') { advance(text_[i]); ++i; }
                continue;
            }
            if (c == '/' && i + 1 < text_.size() && text_[i + 1] == '*') {
                advance(text_[i++]); advance(text_[i++]);
                while (i + 1 < text_.size() && !(text_[i] == '*' && text_[i + 1] == '/')) advance(text_[i++]);
                if (i + 1 >= text_.size()) refuse(Failure::syntax, loc);
                advance(text_[i++]); advance(text_[i++]); continue;
            }
            if (c == '#' && !code) {
                const auto start = i;
                while (i < text_.size() && text_[i] != '\n') { advance(text_[i]); ++i; }
                std::string_view directive(text_.data() + start + 1, i - start - 1);
                const auto trim = [&]() { while (!directive.empty() && (directive.front() == ' ' || directive.front() == '\t')) directive.remove_prefix(1); };
                trim();
                if (directive.starts_with("line")) { directive.remove_prefix(4); trim(); }
                std::uint32_t number{};
                const auto result = std::from_chars(directive.data(), directive.data() + directive.size(), number);
                if (result.ec != std::errc{} || number == 0) refuse(Failure::unsupported, loc);
                directive.remove_prefix(static_cast<std::size_t>(result.ptr - directive.data())); trim();
                if (!directive.empty()) {
                    if (directive.front() != '"') refuse(Failure::unsupported, loc);
                    directive.remove_prefix(1);
                    const auto quote = directive.find('"');
                    if (quote == std::string_view::npos) refuse(Failure::syntax, loc);
                    const auto canonical = vfs::canonicalize(directive.substr(0, quote));
                    logical = canonical && known_.contains(canonical.value()) ? canonical.value() : graph_.source.logical_path;
                    line = canonical && known_.contains(canonical.value()) ? std::optional(number - 1) : std::nullopt;
                    directive.remove_prefix(quote + 1); trim();
                    if (!directive.empty()) refuse(Failure::unsupported, loc);
                } else line = line ? std::optional(number - 1) : std::nullopt;
                continue;
            }
            const auto start = i;
            if (c == '"') {
                advance(text_[i++]); bool closed{};
                while (i < text_.size()) {
                    const auto current = text_[i];
                    if (current == '\n' || current == '\r') refuse(Failure::syntax, loc);
                    advance(text_[i++]);
                    if (current == '\\') {
                        if (i == text_.size()) refuse(Failure::syntax, loc);
                        advance(text_[i++]);
                    } else if (current == '"') { closed = true; break; }
                }
                if (!closed) refuse(Failure::syntax, loc);
            } else if (alpha(c)) {
                do { advance(text_[i++]); } while (i < text_.size() && (alpha(text_[i]) || digit(text_[i])));
            } else if (digit(c) || (c == '.' && i + 1 < text_.size() && digit(text_[i + 1]))) {
                do { advance(text_[i++]); }
                while (i < text_.size() && (alpha(text_[i]) || digit(text_[i]) || text_[i] == '.' ||
                    ((text_[i] == '+' || text_[i] == '-') && (text_[i - 1] == 'e' || text_[i - 1] == 'E'))));
            } else { advance(text_[i++]); }
            code = true;
            if (tokens_.size() >= limits_.tokens) refuse(Failure::budget, loc);
            tokens_.push_back({text_.substr(start, i - start), start, i, std::numeric_limits<std::size_t>::max(), loc});
            const auto index = tokens_.size() - 1;
            const auto& value = tokens_.back().text;
            if (value == "{" || value == "(" || value == "[") {
                if (stack.size() >= limits_.nesting) refuse(Failure::budget, loc);
                stack.push_back(index);
            } else if (value == "}" || value == ")" || value == "]") {
                if (stack.empty()) refuse(Failure::syntax, loc);
                const auto open = stack.back(); stack.pop_back();
                const auto& opening = tokens_[open].text;
                if ((value == "}" && opening != "{") || (value == ")" && opening != "(") || (value == "]" && opening != "[")) refuse(Failure::syntax, loc);
                tokens_[open].mate = index; tokens_[index].mate = open;
            }
        }
        if (!stack.empty()) refuse(Failure::syntax, tokens_[stack.back()].location);
    }
    std::size_t until(std::string_view delimiter) {
        const auto first = cursor_;
        while (cursor_ < tokens_.size() && !at(delimiter)) {
            if (at("{") || at("(") || at("[")) cursor_ = tokens_[cursor_].mate + 1;
            else if (at("}") || at(")") || at("]")) fail();
            else ++cursor_;
        }
        if (cursor_ == tokens_.size()) fail();
        return first;
    }
    std::vector<Assignment> assignments(std::string_view end, bool typed) {
        std::vector<Assignment> out;
        while (!at(end)) {
            node(); Assignment item; item.location = location();
            if (typed) item.type = name();
            item.name = name();
            if (at("[")) { // retained selector, consumed by descriptor conversion
                const auto start = cursor_; cursor_ = tokens_[cursor_].mate + 1;
                item.name += slice(start, cursor_);
            }
            expect("="); const auto start = until(";");
            if (start == cursor_) fail();
            item.expression = slice(start, cursor_); expect(";");
            out.push_back(std::move(item));
        }
        expect(end); return out;
    }
    std::vector<Assignment> annotations() {
        if (!at("<")) return {};
        const auto start = cursor_; ++cursor_;
        auto result = assignments(">", true);
        blank(tokens_[start].begin, tokens_[cursor_ - 1].end); return result;
    }
    void technique() {
        node(); const auto start = cursor_++; Technique item;
        item.name = name(); item.annotations = annotations(); expect("{");
        while (!at("}")) {
            node(); Pass pass; pass.location = location(); expect("pass");
            pass.name = at("{") || at("<") ? "p" + std::to_string(item.passes.size()) : name();
            pass.annotations = annotations(); expect("{"); pass.states = assignments("}", false);
            item.passes.push_back(std::move(pass));
        }
        expect("}"); blank(tokens_[start].begin, tokens_[cursor_ - 1].end);
        graph_.techniques.push_back(std::move(item));
    }
    void declaration() {
        const auto start = cursor_; node(); Declaration item; item.location = location();
        while (at("uniform") || at("extern") || at("static") || at("const") || at("volatile") || at("row_major") || at("column_major") || at("shared")) {
            item.qualifiers.push_back(lower(tokens_[cursor_].text));
            item.row_major |= at("row_major"); item.constant |= at("const"); ++cursor_;
        }
        item.type = name(); item.name = name();
        if (at("(")) {
            cursor_ = tokens_[cursor_].mate + 1;
            if (at(":")) { ++cursor_; name(); }
            if (at(";")) { ++cursor_; return; }
            expect("{"); cursor_ = tokens_[cursor_ - 1].mate + 1; return;
        }
        if (at("[")) {
            ++cursor_; const auto first = cursor_; until("]"); item.array = slice(first, cursor_); expect("]");
        }
        if (at(":")) {
            ++cursor_;
            if (at("register")) { ++cursor_; expect("("); cursor_ = tokens_[cursor_ - 1].mate + 1; }
            else item.semantic = name();
        }
        item.annotations = annotations();
        const auto type = lower(item.type);
        const bool sampler = type.starts_with("sampler");
        if (at("=")) {
            const auto equal = cursor_++;
            if (sampler) {
                expect("sampler_state"); expect("{"); item.sampler_states = assignments("}", false);
                blank(tokens_[equal].begin, tokens_[cursor_ - 1].end);
            } else {
                const auto first = until(";"); item.expression = slice(first, cursor_);
                if (!item.constant) blank(tokens_[equal].begin, tokens_[cursor_].begin);
            }
        }
        expect(";");
        if (type.starts_with("texture") || type == "string" || type == "vertexshader" || type == "pixelshader")
            blank(tokens_[start].begin, tokens_[cursor_ - 1].end);
        graph_.declarations.push_back(std::move(item));
    }
    void collect_stages() {
        std::map<std::string, std::string> objects;
        for (const auto& item : graph_.declarations)
            if (lower(item.type) == "vertexshader" || lower(item.type) == "pixelshader") objects.emplace(item.name, item.expression);
        const auto collect = [&](Assignment& assignment) {
            auto value = assignment.expression;
            if (objects.contains(value)) value = objects.at(value);
            if (value.empty() || lower(value) == "null" || value == "0") return;
            // Parse only the bounded compile-profile-entry production.
            const auto skip = [&](std::size_t& pos) { while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' || value[pos] == '\n' || value[pos] == '\r')) ++pos; };
            std::size_t pos{};
            const auto ident = [&]() {
                skip(pos); const auto begin = pos;
                if (pos == value.size() || !alpha(value[pos])) refuse(Failure::unsupported, assignment.location);
                while (pos < value.size() && (alpha(value[pos]) || digit(value[pos]))) ++pos;
                return value.substr(begin, pos - begin);
            };
            if (lower(ident()) != "compile") refuse(Failure::unsupported, assignment.location);
            Stage stage; stage.profile = ident(); stage.entry = ident(); stage.location = assignment.location;
            skip(pos); if (pos == value.size() || value[pos++] != '(') refuse(Failure::unsupported, assignment.location);
            skip(pos); if (pos == value.size() || value[pos++] != ')') refuse(Failure::unsupported, assignment.location);
            skip(pos); if (pos != value.size()) refuse(Failure::unsupported, assignment.location);
            const auto expected = lower(assignment.name) == "vertexshader" ? "vs_" : "ps_";
            if (!stage.profile.starts_with(expected)) refuse(Failure::unsupported, assignment.location);
            if (std::none_of(graph_.stages.begin(), graph_.stages.end(), [&](const Stage& old) { return old.profile == stage.profile && old.entry == stage.entry; })) {
                node(); graph_.stages.push_back(stage);
            }
            for (std::size_t index = 0; index < graph_.stages.size(); ++index)
                if (graph_.stages[index].profile == stage.profile && graph_.stages[index].entry == stage.entry) assignment.program = index;
        };
        for (auto& declaration : graph_.declarations) {
            const auto type = lower(declaration.type);
            if (type == "vertexshader" || type == "pixelshader") {
                Assignment assignment{declaration.type, declaration.expression, declaration.location, {}, {}};
                collect(assignment); declaration.program = assignment.program;
            }
        }
        for (auto& technique : graph_.techniques) for (auto& pass : technique.passes)
            for (auto& state : pass.states) if (lower(state.name) == "vertexshader" || lower(state.name) == "pixelshader") collect(state);
    }
};
} // namespace
Parsed parse(std::span<const std::byte> expanded, assets::Source root,
    std::span<const assets::Source> inputs, Limits limits) {
    try { return {Parser(expanded, std::move(root), inputs, limits).run(), {}}; }
    catch (const Refusal& error) { return {{}, error.diagnostic}; }
    catch (const std::bad_alloc&) { return {{}, Diagnostic{Failure::budget, {}, {}}}; }
}
} // namespace eawr::presentation::shader_source

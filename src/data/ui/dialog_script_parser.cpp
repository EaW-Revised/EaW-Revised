#include "dialog_script_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace eawr::data::ui {
using namespace dialog_script_detail;

namespace {

// One comma-separated field: term ('|' term)*, where a term is an optional
// NOT, an optional unary minus and one identifier, number or string.
struct Term final {
    bool negated{};
    bool minus{};
    const RcToken* token{};
};
struct Field final {
    std::vector<Term> terms;
    const RcToken* first{};
};

struct ControlForm final {
    std::string_view keyword;
    std::string_view class_name;
    bool has_text;
};
constexpr std::array control_forms{
    ControlForm{"LTEXT", "Static", true},          ControlForm{"RTEXT", "Static", true},
    ControlForm{"CTEXT", "Static", true},          ControlForm{"ICON", "Static", true},
    ControlForm{"PUSHBUTTON", "Button", true},     ControlForm{"DEFPUSHBUTTON", "Button", true},
    ControlForm{"PUSHBOX", "Button", true},        ControlForm{"GROUPBOX", "Button", true},
    ControlForm{"CHECKBOX", "Button", true},       ControlForm{"AUTOCHECKBOX", "Button", true},
    ControlForm{"STATE3", "Button", true},         ControlForm{"AUTO3STATE", "Button", true},
    ControlForm{"RADIOBUTTON", "Button", true},    ControlForm{"AUTORADIOBUTTON", "Button", true},
    ControlForm{"EDITTEXT", "Edit", false},        ControlForm{"COMBOBOX", "ComboBox", false},
    ControlForm{"LISTBOX", "ListBox", false},      ControlForm{"SCROLLBAR", "ScrollBar", false},
    ControlForm{"CONTROL", "", true},
};

const ControlForm* control_form(const std::string_view keyword) {
    for (const auto& form : control_forms) {
        if (form.keyword == keyword) return &form;
    }
    return nullptr;
}

// Words that open a statement or a block, so never a value. A value list
// that ends in '|' or ',' (mods ship both) then stops at the next statement
// instead of swallowing it. COMBOBOX, LISTBOX and SCROLLBAR stay values: the
// resource compiler also takes them as a CONTROL class.
bool reserved(const RcToken& token) {
    static constexpr std::array<std::string_view, 13> words{
        "BEGIN", "END", "DIALOG", "DIALOGEX", "STYLE", "EXSTYLE", "CAPTION",
        "FONT", "MENU", "CLASS", "LANGUAGE", "CHARACTERISTICS", "VERSION",
    };
    if (token.kind != RcTokenKind::identifier) return false;
    if (std::find(words.begin(), words.end(), token.text) != words.end()) return true;
    return control_form(token.text) != nullptr && token.text != "COMBOBOX" && token.text != "LISTBOX" &&
        token.text != "SCROLLBAR";
}

// Every failure is recovered (gap G1 in docs/ui/mod-hud-survey.md): a
// malformed dialog is rejected with one EAWR-UI-0201 error and parsing resumes
// after its END, or at the next "<name> DIALOGEX" when the END is missing.
// Anything else that fails between resources is reported and skipped up to
// the next dialog. Retail recovery is not observed; this is EAWR policy.
class Parser final {
public:
    Parser(const RcTokens& tokens, DialogScript& script) : tokens_(tokens.tokens), script_(script) {}

    void run() {
        while (!done()) {
            const auto start = index_;
            if (auto parsed = resource(); !parsed) {
                script_.diagnostics.push_back(parsed.error());
                if (index_ == start) ++index_;
                while (!done() && !at_dialog_start()) ++index_;
            }
        }
    }

private:
    core::Result<void> resource() {
        const RcToken& head = current();
        if (is_word(head, "LANGUAGE")) {
            ++index_;
            auto language = fields();
            if (!language) return core::Result<void>::failure(language.error());
            return core::Result<void>::success();
        }
        if (is_word(head, "STRINGTABLE")) return skip_resource(head, "", "STRINGTABLE");
        if (head.kind == RcTokenKind::punctuation || reserved(head)) {
            return fail("expected a resource name, found '" + head.text + "'", head);
        }
        ++index_;
        if (done() || at_dialog_start()) return fail("resource name '" + head.text + "' without a type", head);
        const RcToken& type = current();
        if (type.kind != RcTokenKind::identifier && type.kind != RcTokenKind::number) {
            return fail("expected a resource type after '" + head.text + "'", type);
        }
        ++index_;
        if (type.text == "DIALOGEX" || type.text == "DIALOG") {
            read_dialog(head, type.text == "DIALOGEX");
            return core::Result<void>::success();
        }
        return skip_resource(head, head.text, type.text);
    }

    void read_dialog(const RcToken& name, const bool extended) {
        Dialog dialog;
        dialog.name = name.text;
        dialog.extended = extended;
        dialog.line = name.line;
        in_body_ = false;
        auto parsed = parse_dialog(dialog, name);
        if (parsed) {
            script_.dialogs.push_back(std::move(dialog));
            return;
        }
        auto diagnostic = parsed.error();
        diagnostic.message = "dialog '" + name.text + "' (line " + std::to_string(name.line) +
            ") is rejected: " + diagnostic.message;
        script_.diagnostics.push_back(std::move(diagnostic));
        script_.rejected.push_back({name.text, name.line});
        skip_rest_of_dialog();
    }

    void skip_rest_of_dialog() {
        if (!in_body_) {
            while (!done() && !at_begin() && !at_dialog_start()) ++index_;
            if (!at_begin()) return;
            ++index_;
        }
        std::size_t depth = 1U;
        while (!done() && !at_dialog_start()) {
            if (at_begin()) {
                ++depth;
            } else if (at_end() && --depth == 0U) {
                ++index_;
                return;
            }
            ++index_;
        }
    }

    bool done() const { return index_ >= tokens_.size(); }
    const RcToken& current() const { return tokens_[index_]; }
    static bool is_word(const RcToken& token, const std::string_view word) {
        return token.kind == RcTokenKind::identifier && token.text == word;
    }
    // "<name> DIALOGEX" or "<name> DIALOG": where the next dialog begins.
    bool at_dialog_start() const {
        if (done() || index_ + 1U >= tokens_.size() || reserved(current()) ||
            (current().kind != RcTokenKind::identifier && current().kind != RcTokenKind::number)) {
            return false;
        }
        const auto& type = tokens_[index_ + 1U];
        return is_word(type, "DIALOGEX") || is_word(type, "DIALOG");
    }
    static bool is_punctuation(const RcToken& token, const char character) {
        return token.kind == RcTokenKind::punctuation && token.text.size() == 1U &&
            token.text[0] == character;
    }
    bool at_begin() const {
        return !done() && (is_word(current(), "BEGIN") || is_punctuation(current(), '{'));
    }
    bool at_end() const {
        return !done() && (is_word(current(), "END") || is_punctuation(current(), '}'));
    }

    core::Result<void> fail(std::string message, const RcToken& at) const {
        return core::Result<void>::failure(make_diagnostic(
            script_.source, diagnostic_codes::rc_syntax, core::Severity::error, std::move(message),
            at.line, at.column));
    }
    core::Result<void> fail_at_end(std::string message) const {
        const RcToken fallback{};
        return fail(std::move(message), tokens_.empty() ? fallback : tokens_.back());
    }

    // Skips options up to a BEGIN ... END block, or a trailing file name.
    core::Result<void> skip_resource(const RcToken& head, std::string name, std::string type) {
        script_.skipped.push_back({std::move(name), std::move(type), head.line});
        if (is_word(head, "STRINGTABLE") && !done() && &current() == &head) ++index_;
        while (!done() && !at_dialog_start()) {
            if (at_begin()) return skip_block();
            if (current().kind == RcTokenKind::string &&
                (index_ + 1U >= tokens_.size() || !is_punctuation(tokens_[index_ + 1U], ','))) {
                ++index_;
                return core::Result<void>::success();
            }
            ++index_;
        }
        if (!done()) return fail("resource '" + script_.skipped.back().type + "' has no body", head);
        return fail_at_end("resource '" + script_.skipped.back().type + "' has no body");
    }

    core::Result<void> skip_block() {
        std::size_t depth = 0U;
        while (!done()) {
            if (at_begin()) ++depth;
            if (at_end()) {
                --depth;
                if (depth == 0U) {
                    ++index_;
                    return core::Result<void>::success();
                }
            }
            ++index_;
        }
        return fail_at_end("BEGIN without a matching END");
    }

    // An empty field, or a trailing '|' or ',' before the next statement.
    core::Result<std::vector<Field>> missing_value() const {
        std::string message = "expected a value";
        if (index_ > 0U && tokens_[index_ - 1U].kind == RcTokenKind::punctuation) {
            message += " after '" + tokens_[index_ - 1U].text + "'";
        }
        const RcToken fallback{};
        const RcToken& at = !done() ? current() : tokens_.empty() ? fallback : tokens_.back();
        if (done()) {
            message += " before the end of the script";
        } else if (at.kind == RcTokenKind::string) {
            message += ", found \"" + at.text + "\"";
        } else {
            message += ", found '" + at.text + "'";
        }
        return core::Result<std::vector<Field>>::failure(make_diagnostic(
            script_.source, diagnostic_codes::rc_syntax, core::Severity::error, std::move(message),
            at.line, at.column));
    }

    core::Result<std::vector<Field>> fields() {
        std::vector<Field> result;
        while (true) {
            Field field;
            while (true) {
                Term term;
                if (!done() && is_word(current(), "NOT")) {
                    term.negated = true;
                    ++index_;
                }
                if (!done() && is_punctuation(current(), '-')) {
                    term.minus = true;
                    ++index_;
                }
                if (done() || current().kind == RcTokenKind::punctuation || reserved(current()) ||
                    at_dialog_start()) {
                    return missing_value();
                }
                if (term.minus && current().kind != RcTokenKind::number) {
                    return core::Result<std::vector<Field>>::failure(make_diagnostic(
                        script_.source, diagnostic_codes::rc_syntax, core::Severity::error,
                        "unary minus needs a number", current().line, current().column));
                }
                term.token = &current();
                if (field.first == nullptr) field.first = term.token;
                ++index_;
                field.terms.push_back(term);
                if (!done() && is_punctuation(current(), '|')) {
                    ++index_;
                    continue;
                }
                break;
            }
            result.push_back(std::move(field));
            if (!done() && is_punctuation(current(), ',')) {
                ++index_;
                continue;
            }
            return core::Result<std::vector<Field>>::success(std::move(result));
        }
    }

    core::Diagnostic field_error(const Field& field, std::string message) const {
        return make_diagnostic(script_.source, diagnostic_codes::rc_syntax, core::Severity::error,
                               std::move(message), field.first->line, field.first->column);
    }

    std::optional<std::int64_t> integer(const Field& field) const {
        if (field.terms.size() != 1U || field.terms[0].negated ||
            field.terms[0].token->kind != RcTokenKind::number) {
            return std::nullopt;
        }
        return field.terms[0].minus ? -field.terms[0].token->value : field.terms[0].token->value;
    }

    std::optional<std::int32_t> coordinate(const Field& field) const {
        const auto value = integer(field);
        if (!value || *value < std::numeric_limits<std::int32_t>::min() ||
            *value > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(*value);
    }

    // A single identifier, number or string, spelled as written.
    std::optional<std::string> atom(const Field& field) const {
        if (field.terms.size() != 1U || field.terms[0].negated) return std::nullopt;
        const auto& term = field.terms[0];
        return (term.minus ? "-" : "") + term.token->text;
    }

    std::vector<std::string> style(const Field& field) const {
        std::vector<std::string> result;
        for (const auto& term : field.terms) {
            std::string text = term.negated ? "NOT " : "";
            if (term.minus) text.push_back('-');
            text += term.token->kind == RcTokenKind::string ? '"' + term.token->text + '"'
                                                            : term.token->text;
            result.push_back(std::move(text));
        }
        return result;
    }

    core::Result<void> rect(const std::vector<Field>& list, const std::size_t first, RcRect& out) {
        const auto x = coordinate(list[first]);
        const auto y = coordinate(list[first + 1U]);
        const auto width = coordinate(list[first + 2U]);
        const auto height = coordinate(list[first + 3U]);
        if (!x || !y || !width || !height) {
            return core::Result<void>::failure(
                field_error(list[first], "x, y, width and height must be integers"));
        }
        out = {*x, *y, *width, *height};
        return core::Result<void>::success();
    }

    core::Result<void> parse_dialog(Dialog& dialog, const RcToken& name) {
        auto header = fields();
        if (!header) return core::Result<void>::failure(header.error());
        const auto& geometry = header.value();
        if (geometry.size() < 4U || geometry.size() > 5U) {
            return fail("dialog header needs x, y, width, height and an optional help id", name);
        }
        if (auto placed = rect(geometry, 0U, dialog.rect); !placed) return placed;
        if (geometry.size() == 5U) {
            dialog.help_id = integer(geometry[4]);
            if (!dialog.help_id) return core::Result<void>::failure(field_error(geometry[4], "help id must be an integer"));
        }
        while (!done() && !at_begin()) {
            const RcToken& keyword = current();
            if (at_dialog_start()) return fail("no BEGIN before the next dialog", keyword);
            if (keyword.kind != RcTokenKind::identifier) return fail("expected a dialog statement", keyword);
            ++index_;
            auto values = fields();
            if (!values) return core::Result<void>::failure(values.error());
            const auto& list = values.value();
            if (keyword.text == "STYLE" && list.size() == 1U) {
                dialog.style = style(list[0]);
            } else if (keyword.text == "EXSTYLE" && list.size() == 1U) {
                dialog.extended_style = style(list[0]);
            } else if (keyword.text == "CAPTION" && list.size() == 1U &&
                       list[0].terms.size() == 1U && list[0].terms[0].token->kind == RcTokenKind::string) {
                dialog.caption = list[0].terms[0].token->text;
            } else if (keyword.text == "FONT" && list.size() >= 2U && list.size() <= 5U) {
                DialogFont font;
                const auto size = integer(list[0]);
                if (!size || list[1].terms.size() != 1U ||
                    list[1].terms[0].token->kind != RcTokenKind::string) {
                    return fail("FONT needs a point size and a face name", keyword);
                }
                font.point_size = static_cast<std::int32_t>(*size);
                font.face = list[1].terms[0].token->text;
                std::array<std::optional<std::int32_t>*, 3> tail{&font.weight, &font.italic, &font.charset};
                for (std::size_t slot = 2U; slot < list.size(); ++slot) {
                    const auto value = integer(list[slot]);
                    if (!value) return fail("FONT weight, italic and charset must be integers", keyword);
                    *tail[slot - 2U] = static_cast<std::int32_t>(*value);
                }
                dialog.font = std::move(font);
            } else if (keyword.text == "MENU" && list.size() == 1U && atom(list[0])) {
                dialog.menu = atom(list[0]);
            } else if (keyword.text == "CLASS" && list.size() == 1U && atom(list[0])) {
                dialog.window_class = atom(list[0]);
            } else if ((keyword.text == "LANGUAGE" && list.size() == 2U) ||
                       ((keyword.text == "CHARACTERISTICS" || keyword.text == "VERSION") &&
                        list.size() == 1U)) {
                // Resource metadata with no effect on layout.
            } else {
                return fail("unsupported or malformed dialog statement '" + keyword.text + "'", keyword);
            }
        }
        if (done()) return fail_at_end("no BEGIN before the end of the script");
        ++index_;
        in_body_ = true;
        while (!done() && !at_end()) {
            if (at_dialog_start()) return fail("no END before the next dialog", current());
            auto control = parse_control();
            if (!control) return core::Result<void>::failure(control.error());
            dialog.controls.push_back(std::move(control.value()));
        }
        if (done()) return fail_at_end("no END before the end of the script");
        ++index_;
        return core::Result<void>::success();
    }

    core::Result<DialogControl> parse_control() {
        const RcToken& keyword = current();
        const auto* form = keyword.kind == RcTokenKind::identifier ? control_form(keyword.text) : nullptr;
        if (form == nullptr) {
            return core::Result<DialogControl>::failure(make_diagnostic(
                script_.source, diagnostic_codes::rc_syntax, core::Severity::error,
                "unsupported control statement '" + keyword.text + "'", keyword.line, keyword.column));
        }
        ++index_;
        auto values = fields();
        if (!values) return core::Result<DialogControl>::failure(values.error());
        const auto& list = values.value();
        const bool generic = form->keyword == "CONTROL";
        const std::size_t minimum = generic ? 8U : (form->has_text ? 6U : 5U);
        if (list.size() < minimum || list.size() > minimum + 2U + (generic ? 0U : 1U)) {
            return core::Result<DialogControl>::failure(make_diagnostic(
                script_.source, diagnostic_codes::rc_syntax, core::Severity::error,
                std::string(form->keyword) + " has " + std::to_string(list.size()) + " fields",
                keyword.line, keyword.column));
        }
        DialogControl control;
        control.statement = std::string(form->keyword);
        control.class_name = std::string(form->class_name);
        control.line = keyword.line;
        std::size_t next = 0U;
        if (form->has_text) {
            const auto text = atom(list[next]);
            if (!text) return core::Result<DialogControl>::failure(field_error(list[next], "control text must be a single value"));
            control.text = *text;
            ++next;
        }
        const auto id = atom(list[next]);
        if (!id || list[next].terms[0].token->kind == RcTokenKind::string) {
            return core::Result<DialogControl>::failure(field_error(list[next], "control id must be a symbol or number"));
        }
        control.id_name = *id;
        if (const auto literal = integer(list[next])) control.id = *literal;
        ++next;
        std::size_t geometry = next;
        if (generic) {
            const auto class_name = atom(list[next]);
            if (!class_name) return core::Result<DialogControl>::failure(field_error(list[next], "CONTROL class must be a single value"));
            control.class_name = *class_name;
            control.style = style(list[next + 1U]);
            geometry = next + 2U;
        }
        if (auto placed = rect(list, geometry, control.rect); !placed) {
            return core::Result<DialogControl>::failure(placed.error());
        }
        std::size_t optional = geometry + 4U;
        if (!generic && optional < list.size()) control.style = style(list[optional++]);
        if (optional < list.size()) control.extended_style = style(list[optional++]);
        if (optional < list.size()) {
            control.help_id = integer(list[optional]);
            if (!control.help_id) return core::Result<DialogControl>::failure(field_error(list[optional], "help id must be an integer"));
        }
        return core::Result<DialogControl>::success(std::move(control));
    }

    const std::vector<RcToken>& tokens_;
    DialogScript& script_;
    std::size_t index_{};
    // Set once the current dialog's BEGIN is read; recovery then looks for its END.
    bool in_body_{};
};
} // namespace

void dialog_script_detail::parse_dialog_tokens(const RcTokens& tokens, DialogScript& script) {
    Parser parser(tokens, script);
    parser.run();
}

} // namespace eawr::data::ui

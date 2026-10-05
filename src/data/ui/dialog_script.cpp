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

const Dialog* DialogScript::find(const std::string_view name) const noexcept {
    const auto found = std::find_if(dialogs.begin(), dialogs.end(),
                                    [name](const Dialog& dialog) { return dialog.name == name; });
    return found == dialogs.end() ? nullptr : &*found;
}

core::Result<DialogScript> parse_dialog_script(const std::string_view text, Source source) {
    DialogScript script;
    script.source = std::move(source);
    auto tokens = tokenize_rc(text, script.source);
    if (!tokens) return core::Result<DialogScript>::failure(tokens.error());
    for (const auto& directive : tokens.value().directives) {
        auto body = trim(directive.text);
        if (!body.starts_with("include")) continue;
        body = trim(body.substr(7U));
        if (body.size() >= 2U && ((body.front() == '"' && body.back() == '"') ||
                                  (body.front() == '<' && body.back() == '>'))) {
            script.includes.emplace_back(body.substr(1U, body.size() - 2U));
        }
    }
    parse_dialog_tokens(tokens.value(), script);
    return core::Result<DialogScript>::success(std::move(script));
}


} // namespace eawr::data::ui

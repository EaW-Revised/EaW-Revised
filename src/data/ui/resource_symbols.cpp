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
constexpr std::int64_t idc_static = -1;
std::optional<std::int64_t> define_value(std::string_view text) {
    if (const auto comment = text.find("//"); comment != std::string_view::npos) {
        text = trim(text.substr(0U, comment));
    }
    while (text.size() >= 2U && text.front() == '(' && text.back() == ')') {
        text = trim(text.substr(1U, text.size() - 2U));
    }
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text = trim(text.substr(1U));
    }
    const auto value = integer_literal(text);
    if (!value) return std::nullopt;
    return negative ? -*value : *value;
}
} // namespace

std::optional<std::int64_t> ResourceSymbols::find(const std::string_view name) const {
    const auto found = values.find(name);
    if (found == values.end()) return std::nullopt;
    return found->second;
}

core::Result<ResourceSymbols> parse_resource_header(const std::string_view text, Source source) {
    ResourceSymbols result;
    result.source = std::move(source);
    std::uint32_t line = 0U;
    std::size_t begin = 0U;
    while (begin <= text.size()) {
        ++line;
        const auto end = std::min(text.find('\n', begin), text.size());
        const auto raw = trim(text.substr(begin, end - begin));
        begin = end + 1U;
        if (!raw.starts_with('#')) continue;
        auto directive = trim(raw.substr(1U));
        if (!directive.starts_with("define") || directive.size() == 6U ||
            (directive[6] != ' ' && directive[6] != '\t')) {
            continue;
        }
        directive = trim(directive.substr(6U));
        std::size_t name_end = 0U;
        while (name_end < directive.size() && identifier_part(directive[name_end])) ++name_end;
        const auto name = directive.substr(0U, name_end);
        const auto rest = trim(directive.substr(name_end));
        if (name.empty() || !identifier_start(name.front())) {
            result.diagnostics.push_back(make_diagnostic(result.source, diagnostic_codes::rc_symbol,
                                                         core::Severity::warning,
                                                         "#define without a symbol name", line));
            continue;
        }
        if (rest.empty()) continue;  // include guards and flags
        const auto value = define_value(rest);
        if (!value) {
            result.diagnostics.push_back(make_diagnostic(
                result.source, diagnostic_codes::rc_symbol, core::Severity::warning,
                "symbol '" + std::string(name) + "' is not an integer: " + std::string(rest), line));
            continue;
        }
        const auto [found, inserted] = result.values.emplace(std::string(name), *value);
        if (!inserted && found->second != *value) {
            std::ostringstream message;
            message << "symbol '" << name << "' is redefined as " << *value << "; the first value "
                    << found->second << " is kept";
            result.diagnostics.push_back(make_diagnostic(result.source, diagnostic_codes::rc_symbol,
                                                         core::Severity::warning, message.str(), line));
        }
    }
    return core::Result<ResourceSymbols>::success(std::move(result));
}

void resolve_ids(DialogScript& script, const ResourceSymbols& symbols) {
    std::set<std::string, std::less<>> reported;
    const auto resolve = [&](const std::string& name, std::optional<std::int64_t>& id,
                             const std::uint32_t line) {
        if (name.empty()) return;
        if (const auto literal = integer_literal(name.front() == '-' ? name.substr(1U) : name)) {
            id = name.front() == '-' ? -*literal : *literal;
            return;
        }
        if (const auto value = symbols.find(name)) {
            id = *value;
            return;
        }
        if (name == "IDC_STATIC") {
            id = idc_static;
            return;
        }
        id.reset();
        if (reported.insert(name).second) {
            script.diagnostics.push_back(make_diagnostic(
                script.source, diagnostic_codes::rc_unresolved_id, core::Severity::warning,
                "resource id '" + name + "' is not defined in " + symbols.source.logical_path, line));
        }
    };
    for (auto& dialog : script.dialogs) {
        resolve(dialog.name, dialog.id, dialog.line);
        for (auto& control : dialog.controls) resolve(control.id_name, control.id, control.line);
    }
}

} // namespace eawr::data::ui

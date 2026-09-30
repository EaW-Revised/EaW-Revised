#include "eawr/data/ui/command_bar.hpp"

#include "ui_internal.hpp"
#include "xml_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <utility>

namespace eawr::data::ui {
namespace {

struct FieldSchema {
    std::string_view tag;
    FieldKind kind;
};

// Indexed by Field. Tag spellings are the canonical FoC ones.
constexpr std::array<FieldSchema, field_count> schema{{
    {"Animate_Back", FieldKind::flag},
    {"Animate_Upper_Effect", FieldKind::flag},
    {"Blink_Fade", FieldKind::flag},
    {"Can_Animate", FieldKind::flag},
    {"Can_Drag_Stack", FieldKind::flag},
    {"Click_Shift", FieldKind::flag},
    {"Cross_Fade", FieldKind::flag},
    {"Dialog_Scene", FieldKind::flag},
    {"Disable_Darken", FieldKind::flag},
    {"Disabled", FieldKind::flag},
    {"Disabled_Darken", FieldKind::flag},
    {"Drag_And_Drop", FieldKind::flag},
    {"Drag_Back", FieldKind::flag},
    {"Drag_Select", FieldKind::flag},
    {"Ghost_Base_Only", FieldKind::flag},
    {"Hidden", FieldKind::flag},
    {"Left_Justified", FieldKind::flag},
    {"Loop_Anim", FieldKind::flag},
    {"Lower_Effect_Additive", FieldKind::flag},
    {"Manual_Offset", FieldKind::flag},
    {"Model_Offset_X", FieldKind::flag},
    {"Model_Offset_Y", FieldKind::flag},
    {"No_Hidden_Collision", FieldKind::flag},
    {"No_Shell", FieldKind::flag},
    {"Offset_Render", FieldKind::flag},
    {"Outlined_Bar", FieldKind::flag},
    {"Pixel_Align", FieldKind::flag},
    {"Right_Justified", FieldKind::flag},
    {"Selected_Alpha", FieldKind::flag},
    {"Should_Ghost", FieldKind::flag},
    {"Should_Render_At_Drag_Pos", FieldKind::flag},
    {"Smooth_Bar", FieldKind::flag},
    {"Snap_Drag", FieldKind::flag},
    {"Snap_Location", FieldKind::flag},
    {"Stackable", FieldKind::flag},
    {"Swap_Texture", FieldKind::flag},
    {"Tab", FieldKind::flag},
    {"Text_Emboss", FieldKind::flag},
    {"Text_Outline", FieldKind::flag},
    {"Toggle", FieldKind::flag},
    {"Tutorial_Scene", FieldKind::flag},
    {"Anim_FPS", FieldKind::integer},
    {"Base_Layer", FieldKind::integer},
    {"Font_Point_Size", FieldKind::integer},
    {"Max_Bar_Level", FieldKind::integer},
    {"Max_Text_Width", FieldKind::integer},
    {"Blink_Duration", FieldKind::number},
    {"Blink_Rate", FieldKind::number},
    {"Scale", FieldKind::number},
    {"Scale_Duration", FieldKind::number},
    {"Build_Dial2_Offset", FieldKind::vec2},
    {"Build_Dial_Offset", FieldKind::vec2},
    {"Default_Offset", FieldKind::vec2},
    {"Default_Offset_Widescreen", FieldKind::vec2},
    {"Disabled_Offset", FieldKind::vec2},
    {"Icon_Offset", FieldKind::vec2},
    {"Lower_Effect_Offset", FieldKind::vec2},
    {"Mouse_Over_Offset", FieldKind::vec2},
    {"Offset", FieldKind::vec2},
    {"Overlay2_Offset", FieldKind::vec2},
    {"Overlay_Offset", FieldKind::vec2},
    {"Size", FieldKind::vec2},
    {"Text_Offset", FieldKind::vec2},
    {"Text_Offset2", FieldKind::vec2},
    {"Upper_Effect_Offset", FieldKind::vec2},
    {"Color", FieldKind::color},
    {"Text_Color", FieldKind::color},
    {"Text_Color2", FieldKind::color},
    {"Click_SFX", FieldKind::text},
    {"Font_Name", FieldKind::text},
    {"Mega_Texture_Name", FieldKind::text},
    {"Model_Name", FieldKind::text},
    {"Mouse_Over_SFX", FieldKind::text},
    {"Bar_Overlay_Name", FieldKind::tokens},
    {"Bar_Texture_Name", FieldKind::tokens},
    {"Blank_Texture_Name", FieldKind::tokens},
    {"Build_Texture_Name", FieldKind::tokens},
    {"Disabled_Texture_Name", FieldKind::tokens},
    {"Flash_Texture_Name", FieldKind::tokens},
    {"Icon_Alternate_Texture_Name", FieldKind::tokens},
    {"Icon_Texture_Name", FieldKind::tokens},
    {"Lower_Effect_Texture_Name", FieldKind::tokens},
    {"Mouse_Over_Texture_Name", FieldKind::tokens},
    {"Overlay2_Texture_Name", FieldKind::tokens},
    {"Overlay_Texture_Name", FieldKind::tokens},
    {"Selected_Texture_Name", FieldKind::tokens},
    {"Tooltip_Text", FieldKind::tokens},
    {"Alternate_Font_Name", FieldKind::names},
}};
static_assert(schema.back().tag == "Alternate_Font_Name", "schema has one entry per Field");

constexpr std::array<std::pair<std::string_view, ComponentType>, 5> type_names{{
    {"Button", ComponentType::button},
    {"TextButton", ComponentType::text_button},
    {"Bar", ComponentType::bar},
    {"Shell", ComponentType::shell},
    {"Icon", ComponentType::icon},
}};

bool is_space(const char ch) noexcept {
    return xml_space(static_cast<unsigned char>(ch));
}

std::vector<std::string> split_whitespace(const std::string_view value) {
    std::vector<std::string> result;
    std::size_t position = 0;
    while (position < value.size()) {
        while (position < value.size() && is_space(value[position])) ++position;
        const std::size_t start = position;
        while (position < value.size() && !is_space(value[position])) ++position;
        if (position > start) result.emplace_back(value.substr(start, position - start));
    }
    return result;
}

std::string collapse_whitespace(const std::string_view value) {
    std::string result;
    for (const std::string& token : split_whitespace(value)) {
        if (!result.empty()) result.push_back(' ');
        result += token;
    }
    return result;
}

// Lines from pugixml byte offsets, by binary search over line starts.
class LineIndex {
public:
    explicit LineIndex(const std::span<const std::byte> bytes) {
        starts_.push_back(0);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (bytes[i] == std::byte{'\n'}) starts_.push_back(i + 1U);
        }
    }

    [[nodiscard]] std::uint64_t line(const std::ptrdiff_t offset) const {
        if (offset < 0) return 1;
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), static_cast<std::size_t>(offset));
        return static_cast<std::uint64_t>(it - starts_.begin());
    }

private:
    std::vector<std::size_t> starts_;
};

struct Context {
    const vfs::AssetRecord& record;
    const LineIndex& lines;
    std::vector<core::Diagnostic>& diagnostics;

    void report(const std::string_view code, std::string message, const std::uint64_t line,
                const core::Severity severity = core::Severity::warning) const {
        SourceLocation source{record.canonical_path, record.source_id, record.layer_id, line, 1};
        diagnostics.push_back(diagnostic(code, std::move(message), &source, severity));
    }
};

// Concatenated character data of an element; comments split pcdata runs.
std::string element_text(const pugi::xml_node node) {
    std::string result;
    for (const pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_pcdata && child.type() != pugi::node_cdata) continue;
        if (!result.empty()) result.push_back(' ');
        result += child.value();
    }
    return result;
}

// Longest numeric prefix of a token; `clean` is false when characters follow.
template <typename T>
std::optional<T> numeric_prefix(std::string_view token, bool& clean) {
    if (!token.empty() && token.front() == '+') token.remove_prefix(1);
    T value{};
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end == token.data()) {
        clean = false;
        return std::nullopt;
    }
    if (end != token.data() + token.size()) clean = false;
    return value;
}

std::optional<bool> parse_flag(const std::string_view value) {
    const std::string lowered = ascii_lower(value);
    if (lowered == "true" || lowered == "yes" || lowered == "1") return true;
    if (lowered == "false" || lowered == "no" || lowered == "0") return false;
    return std::nullopt;
}

std::string in_quotes(const std::string_view value) {
    return '"' + std::string(value) + '"';
}

// Parses `raw` as `field`. Returns nullopt when nothing is stored.
std::optional<FieldValue> parse_value(const Field field, const std::string& raw, const std::uint64_t line,
                                      const std::string_view component, const Context& context) {
    const FieldKind kind = field_kind(field);
    const std::string value = collapse_whitespace(raw);
    const std::string where = std::string(to_string(field)) + " of " + in_quotes(component);
    const auto bad = [&](const std::string_view why) {
        context.report(diagnostic_codes::field_value, where + ' ' + std::string(why) + ": " + in_quotes(value), line);
    };
    if (value.empty()) {
        switch (kind) {
        case FieldKind::text: return FieldValue{std::string{}};
        case FieldKind::tokens:
        case FieldKind::names: return FieldValue{std::vector<std::string>{}};
        default: bad("is empty; the field is left unset"); return std::nullopt;
        }
    }
    switch (kind) {
    case FieldKind::flag: {
        const auto parsed = parse_flag(value);
        if (!parsed) {
            bad("is not True or False and is ignored");
            return std::nullopt;
        }
        return FieldValue{*parsed};
    }
    case FieldKind::integer:
    case FieldKind::number: {
        const std::vector<std::string> tokens = split_whitespace(value);
        bool clean = tokens.size() == 1U;
        if (kind == FieldKind::integer) {
            const auto parsed = numeric_prefix<std::int32_t>(tokens.front(), clean);
            if (!parsed) {
                bad("is not a whole number and is ignored");
                return std::nullopt;
            }
            if (!clean) bad("has extra characters; the leading number is used");
            return FieldValue{*parsed};
        }
        const auto parsed = numeric_prefix<float>(tokens.front(), clean);
        if (!parsed) {
            bad("is not a number and is ignored");
            return std::nullopt;
        }
        if (!clean) bad("has extra characters; the leading number is used");
        return FieldValue{*parsed};
    }
    case FieldKind::vec2: {
        const std::vector<std::string> tokens = split_whitespace(value);
        bool clean = tokens.size() == 2U;
        Vec2 result;
        const auto x = numeric_prefix<float>(tokens[0], clean);
        if (!x) {
            bad("is not a pair of numbers and is ignored");
            return std::nullopt;
        }
        result.x = *x;
        if (tokens.size() >= 2U) {
            const auto y = numeric_prefix<float>(tokens[1], clean);
            if (y) result.y = *y;
        }
        if (!clean) bad("is not exactly two numbers; missing or unreadable components are 0");
        return FieldValue{result};
    }
    case FieldKind::color: {
        const std::vector<std::string> tokens = split_whitespace(value);
        if (tokens.size() < 3U) {
            bad("has fewer than three components and is ignored");
            return std::nullopt;
        }
        bool clean = tokens.size() <= 4U;
        std::array<std::uint8_t, 4> channels{0, 0, 0, 255};
        for (std::size_t i = 0; i < std::min<std::size_t>(tokens.size(), 4U); ++i) {
            const auto channel = numeric_prefix<std::int32_t>(tokens[i], clean);
            if (!channel) {
                bad("has an unreadable component and is ignored");
                return std::nullopt;
            }
            if (*channel < 0 || *channel > 255) clean = false;
            channels[i] = static_cast<std::uint8_t>(std::clamp(*channel, 0, 255));
        }
        if (!clean) bad("is not three or four components in 0..255; extra components are dropped and values clamped");
        return FieldValue{Rgba8{channels[0], channels[1], channels[2], channels[3]}};
    }
    case FieldKind::text: return FieldValue{value};
    case FieldKind::tokens: return FieldValue{split_whitespace(value)};
    case FieldKind::names: {
        std::vector<std::string> names;
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t comma = std::min(value.find(',', start), value.size());
            std::string name = trim(std::string_view(value).substr(start, comma - start));
            if (!name.empty()) names.push_back(std::move(name));
            start = comma + 1U;
        }
        return FieldValue{std::move(names)};
    }
    }
    return std::nullopt;
}

void parse_component(const pugi::xml_node node, const Context& context, CommandBarCatalog& catalog) {
    const std::uint64_t line = context.lines.line(node.offset_debug());
    CommandBarComponent component;
    component.name = trim(node.attribute("Name").value());
    component.logical_path = context.record.canonical_path;
    component.line = line;
    if (component.name.empty()) {
        context.report(diagnostic_codes::component_invalid, "component has no Name and is skipped", line,
                       core::Severity::error);
        return;
    }
    component.alt = split_alt(component.name);

    std::optional<ComponentType> type;
    bool type_seen = false;
    std::array<bool, field_count> seen{};
    for (const pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) continue;
        const std::uint64_t child_line = context.lines.line(child.offset_debug());
        const std::string tag = child.name();
        const std::string raw = element_text(child);
        if (detail::iequals(tag, "Group")) {
            std::string group = collapse_whitespace(raw);
            if (group.empty()) continue;
            const bool present = std::any_of(component.groups.begin(), component.groups.end(),
                [&](const std::string& existing) { return detail::iequals(existing, group); });
            if (!present) component.groups.push_back(std::move(group));
            continue;
        }
        if (detail::iequals(tag, "Type")) {
            if (type_seen) {
                context.report(diagnostic_codes::field_repeated,
                    "Type of " + in_quotes(component.name) + " is repeated; the last value wins", child_line);
            }
            type_seen = true;
            type = component_type_from(trim(raw));
            if (!type) {
                context.report(diagnostic_codes::component_invalid,
                    "Type of " + in_quotes(component.name) + " is unknown: " + in_quotes(trim(raw)), child_line,
                    core::Severity::error);
            }
            continue;
        }
        const std::optional<Field> field = field_from_tag(tag);
        if (!field) {
            context.report(diagnostic_codes::field_unknown,
                "unknown tag " + in_quotes(tag) + " in " + in_quotes(component.name) + " is kept unparsed", child_line);
            component.unknown_fields.push_back({tag, trim(raw), child_line});
            continue;
        }
        const auto index = static_cast<std::size_t>(*field);
        if (seen[index]) {
            context.report(diagnostic_codes::field_repeated,
                std::string(to_string(*field)) + " of " + in_quotes(component.name) +
                    " is repeated; the last value wins",
                child_line);
        }
        seen[index] = true;
        std::optional<FieldValue> value = parse_value(*field, raw, child_line, component.name, context);
        const auto existing = std::find_if(component.fields.begin(), component.fields.end(),
            [&](const FieldEntry& entry) { return entry.field == *field; });
        if (!value) {
            // An empty later tag clears the field (e.g. a Base_Layer the data marks as derived);
            // an unreadable one keeps the earlier value.
            if (existing != component.fields.end() && collapse_whitespace(raw).empty()) component.fields.erase(existing);
            continue;
        }
        if (existing != component.fields.end()) {
            existing->value = std::move(*value);
            existing->line = child_line;
        } else {
            component.fields.push_back({*field, std::move(*value), child_line});
        }
    }
    if (!type) {
        if (!type_seen) {
            context.report(diagnostic_codes::component_invalid,
                "component " + in_quotes(component.name) + " has no Type and is skipped", line, core::Severity::error);
        }
        return;
    }
    component.type = *type;
    const std::string name = component.name;
    if (catalog.add(std::move(component))) {
        context.report(diagnostic_codes::component_duplicate,
            "component " + in_quotes(name) + " is defined again; the later definition replaces it", line);
    }
}

core::Diagnostic failure(const std::string_view code, std::string message, const std::string_view path) {
    SourceLocation source{std::string(path), {}, {}, 1, 1};
    return diagnostic(code, std::move(message), &source);
}

} // namespace

AltName split_alt(const std::string_view name) {
    std::size_t digits = name.size();
    while (digits > 0 && (name[digits - 1] >= '0' && name[digits - 1] <= '9')) --digits;
    constexpr std::string_view suffix = "_ALT";
    if (digits == name.size() || digits < suffix.size() + 1U ||
        !detail::iequals(name.substr(digits - suffix.size(), suffix.size()), suffix)) {
        return {std::string(name), std::nullopt};
    }
    std::uint32_t variant{};
    const auto [end, error] = std::from_chars(name.data() + digits, name.data() + name.size(), variant);
    if (error != std::errc{} || end != name.data() + name.size()) return {std::string(name), std::nullopt};
    return {std::string(name.substr(0, digits - suffix.size())), variant};
}

const FieldValue* CommandBarComponent::find(const Field field) const noexcept {
    for (const FieldEntry& entry : fields) {
        if (entry.field == field) return &entry.value;
    }
    return nullptr;
}

bool CommandBarComponent::flag(const Field field, const bool fallback) const noexcept {
    const FieldValue* value = find(field);
    const bool* result = value ? std::get_if<bool>(value) : nullptr;
    return result ? *result : fallback;
}

std::optional<std::int32_t> CommandBarComponent::integer(const Field field) const noexcept {
    const FieldValue* value = find(field);
    const std::int32_t* result = value ? std::get_if<std::int32_t>(value) : nullptr;
    return result ? std::optional<std::int32_t>(*result) : std::nullopt;
}

std::optional<float> CommandBarComponent::number(const Field field) const noexcept {
    const FieldValue* value = find(field);
    const float* result = value ? std::get_if<float>(value) : nullptr;
    return result ? std::optional<float>(*result) : std::nullopt;
}

std::string_view CommandBarComponent::text(const Field field) const noexcept {
    const FieldValue* value = find(field);
    const std::string* result = value ? std::get_if<std::string>(value) : nullptr;
    return result ? std::string_view(*result) : std::string_view{};
}

std::span<const std::string> CommandBarComponent::list(const Field field) const noexcept {
    const FieldValue* value = find(field);
    const auto* result = value ? std::get_if<std::vector<std::string>>(value) : nullptr;
    return result ? std::span<const std::string>(*result) : std::span<const std::string>{};
}

std::optional<Vec2> CommandBarComponent::vec2(const Field field) const noexcept {
    const FieldValue* value = find(field);
    const Vec2* result = value ? std::get_if<Vec2>(value) : nullptr;
    return result ? std::optional<Vec2>(*result) : std::nullopt;
}

std::optional<Rgba8> CommandBarComponent::color(const Field field) const noexcept {
    const FieldValue* value = find(field);
    const Rgba8* result = value ? std::get_if<Rgba8>(value) : nullptr;
    return result ? std::optional<Rgba8>(*result) : std::nullopt;
}

bool CommandBarComponent::in_group(const std::string_view group) const noexcept {
    return std::any_of(groups.begin(), groups.end(),
        [&](const std::string& existing) { return detail::iequals(existing, group); });
}

const CommandBarComponent* CommandBarCatalog::find(const std::string_view name) const noexcept {
    const auto match = std::find_if(components_.begin(), components_.end(),
        [&](const CommandBarComponent& component) { return detail::iequals(component.name, name); });
    return match == components_.end() ? nullptr : &*match;
}

std::size_t CommandBarCatalog::count(const ComponentType type) const noexcept {
    return static_cast<std::size_t>(std::count_if(components_.begin(), components_.end(),
        [&](const CommandBarComponent& component) { return component.type == type; }));
}

const CommandBarComponent* CommandBarCatalog::shell_for_model(const std::string_view model_name) const noexcept {
    const auto match = std::find_if(components_.begin(), components_.end(), [&](const CommandBarComponent& component) {
        return component.type == ComponentType::shell && detail::iequals(component.text(Field::model_name), model_name);
    });
    return match == components_.end() ? nullptr : &*match;
}

bool CommandBarCatalog::add(CommandBarComponent component) {
    const auto existing = std::find_if(components_.begin(), components_.end(),
        [&](const CommandBarComponent& other) { return detail::iequals(other.name, component.name); });
    if (existing != components_.end()) {
        *existing = std::move(component);
        return true;
    }
    components_.push_back(std::move(component));
    return false;
}

core::Result<void> parse_command_bar_components(const std::span<const std::byte> bytes,
    const vfs::AssetRecord& record, CommandBarCatalog& catalog, std::vector<core::Diagnostic>& diagnostics) {
    auto parsed = parse_document(bytes, record);
    if (!parsed) return core::Result<void>::failure(parsed.error());
    const LineIndex lines(bytes);
    const Context context{record, lines, diagnostics};
    const pugi::xml_node root = parsed.value().root;
    catalog.add_source_file(record.canonical_path);
    for (const pugi::xml_node node : root.children()) {
        if (node.type() != pugi::node_element) continue;
        if (!detail::iequals(node.name(), "CommandBarComponent")) {
            context.report(diagnostic_codes::field_unknown,
                "unexpected element " + in_quotes(node.name()) + " in the component list is ignored",
                lines.line(node.offset_debug()));
            continue;
        }
        parse_component(node, context, catalog);
    }
    return core::Result<void>::success();
}

core::Result<CommandBarLoad> load_command_bar(const vfs::Vfs& filesystem) {
    const auto list_record = filesystem.stat(command_bar_list_path);
    const auto list_bytes = filesystem.open(command_bar_list_path);
    if (!list_record || !list_bytes) {
        return core::Result<CommandBarLoad>::failure(failure(diagnostic_codes::command_bar_list,
            "command-bar file list is missing", command_bar_list_path));
    }
    auto list = parse_document(list_bytes.value(), list_record.value());
    if (!list) {
        core::Diagnostic error = list.error();
        error.code = std::string(diagnostic_codes::command_bar_list);
        return core::Result<CommandBarLoad>::failure(std::move(error));
    }
    CommandBarLoad result;
    for (const pugi::xml_node file : list.value().root.children()) {
        if (file.type() != pugi::node_element || !detail::iequals(file.name(), "File")) continue;
        const std::string name = trim(element_text(file));
        if (name.empty()) continue;
        const std::string path = "data/xml/" + name;
        const auto record = filesystem.stat(path);
        const auto bytes = filesystem.open(path);
        if (!record || !bytes) {
            result.diagnostics.push_back(failure(diagnostic_codes::command_bar_file,
                "listed command-bar file is missing: " + in_quotes(name), path));
            continue;
        }
        auto parsed = parse_command_bar_components(bytes.value(), record.value(), result.catalog, result.diagnostics);
        if (!parsed) {
            core::Diagnostic error = parsed.error();
            error.code = std::string(diagnostic_codes::command_bar_file);
            result.diagnostics.push_back(std::move(error));
        }
    }
    return core::Result<CommandBarLoad>::success(std::move(result));
}

std::string_view to_string(const ComponentType type) noexcept {
    for (const auto& [name, value] : type_names) {
        if (value == type) return name;
    }
    return "Button";
}

std::optional<ComponentType> component_type_from(const std::string_view text) noexcept {
    for (const auto& [name, value] : type_names) {
        if (detail::iequals(name, text)) return value;
    }
    return std::nullopt;
}

std::string_view to_string(const Field field) noexcept {
    return schema[static_cast<std::size_t>(field)].tag;
}

FieldKind field_kind(const Field field) noexcept {
    return schema[static_cast<std::size_t>(field)].kind;
}

std::optional<Field> field_from_tag(const std::string_view tag) noexcept {
    for (std::size_t i = 0; i < schema.size(); ++i) {
        if (detail::iequals(schema[i].tag, tag)) return static_cast<Field>(i);
    }
    return std::nullopt;
}

} // namespace eawr::data::ui

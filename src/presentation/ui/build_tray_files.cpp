#include "eawr/core/from_chars.hpp"
#include "eawr/presentation/ui/build_tray_files.hpp"

#include "eawr/core/text.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <system_error>
#include <utility>

namespace eawr::presentation::ui::build_tray {
namespace {

using data::XmlNode;

const XmlNode* child(const XmlNode& node, const std::string_view tag) noexcept {
    for (const auto& item : node.children) if (core::iequals(item.name, tag)) return &item;
    return nullptr;
}

std::string text_of(const XmlNode& node) {
    return core::trim(node.raw_text, core::xml_whitespace);
}

std::string attribute_of(const XmlNode& node, const std::string_view name) {
    for (const auto& attribute : node.attributes)
        if (core::iequals(attribute.name, name)) return core::trim(attribute.value, core::xml_whitespace);
    return {};
}

bool parse_number(const std::string_view text, double& value) noexcept {
    const auto trimmed = core::trim_view(text, core::xml_whitespace);
    if (trimmed.empty()) return false;
    double parsed{};
    const auto result = eawr::core::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size() || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

bool parse_integer(const std::string_view text, std::int64_t& value) noexcept {
    const auto trimmed = core::trim_view(text, core::xml_whitespace);
    if (trimmed.empty()) return false;
    std::int64_t parsed{};
    const auto result = eawr::core::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) return false;
    value = parsed;
    return true;
}

bool parse_pair(const std::string_view text, double& x, double& y) noexcept {
    const auto trimmed = core::trim_view(text, core::xml_whitespace);
    const auto split = trimmed.find_first_of(core::xml_whitespace);
    if (split == std::string_view::npos) return false;
    return parse_number(trimmed.substr(0, split), x) && parse_number(trimmed.substr(split), y);
}

std::optional<bool> parse_flag(const std::string_view text) noexcept {
    const auto value = core::trim_view(text, core::xml_whitespace);
    if (core::iequals(value, "yes") || core::iequals(value, "true") || value == "1") return true;
    if (core::iequals(value, "no") || core::iequals(value, "false") || value == "0") return false;
    return std::nullopt;
}

bool is_name(const std::string_view name) noexcept {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](const char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')
            || value == '_';
    });
}

// A type name or the stem of a prefix pattern: object type names may carry a hyphen.
bool is_type_stem(const std::string_view name) noexcept {
    return !name.empty() && std::none_of(name.begin(), name.end(), [](const char value) {
        return core::xml_whitespace.find(value) != std::string_view::npos || value == '*' || value == '<' || value == '&';
    });
}

// Comma or whitespace separated names; false when one is not a plain name.
bool parse_name_list(const std::string_view text, std::vector<std::string>& out) {
    std::size_t position{};
    while (position < text.size()) {
        const auto start = text.find_first_not_of(", \t\r\n", position);
        if (start == std::string_view::npos) break;
        const auto end = text.find_first_of(", \t\r\n", start);
        const auto name = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!is_name(name)) return false;
        out.emplace_back(name);
        if (end == std::string_view::npos) break;
        position = end;
    }
    return true;
}

bool known_side(const std::string_view side) noexcept {
    constexpr std::array sides{"above", "below", "left", "right", "inside_top_left", "inside_top_right",
        "inside_bottom_left", "inside_bottom_right"};
    return std::any_of(sides.begin(), sides.end(), [&](const char* name) { return core::iequals(side, name); });
}

// Every `Candidate` child of `parent`, in order; false when one lacks an Anchor, a known Side or a two-number Offset.
bool parse_candidates(const XmlNode& parent, std::vector<HandleCandidate>& out) {
    for (const auto& item : parent.children) {
        if (!core::iequals(item.name, "Candidate")) continue;
        HandleCandidate candidate;
        if (const auto* node = child(item, "Anchor")) candidate.anchor = text_of(*node);
        if (const auto* node = child(item, "Side")) candidate.side = core::to_lower(text_of(*node));
        const auto* offset = child(item, "Offset");
        if (candidate.anchor.empty() || !known_side(candidate.side) || !offset
            || !parse_pair(text_of(*offset), candidate.x, candidate.y))
            return false;
        out.push_back(std::move(candidate));
    }
    return true;
}

} // namespace

Parsed<TrayLayout> parse_tray_layout(const data::XmlDocument& document) {
    Parsed<TrayLayout> result;
    const auto fail = [&](std::string message) {
        result.value.reset();
        result.error = std::move(message);
        return result;
    };
    if (!core::iequals(document.root.name, "Revised_Build_Tray")) return fail("the root element is not Revised_Build_Tray");
    TrayLayout layout;
    const auto& root = document.root;

    if (const auto* node = child(root, "Enabled_By_Default")) {
        const auto flag = parse_flag(text_of(*node));
        if (!flag) return fail("Enabled_By_Default is not Yes or No");
        layout.enabled_by_default = *flag;
    }

    if (const auto* handle = child(root, "Handle")) {
        // GBT-02: the control is replaced whole; nothing is inherited from the built-in handle.
        HandleDefinition definition;
        definition.candidates.clear();
        if (const auto* node = child(*handle, "Enabled")) {
            const auto flag = parse_flag(text_of(*node));
            if (!flag) return fail("Handle/Enabled is not true or false");
            definition.enabled = *flag;
        }
        if (definition.enabled) {
            const auto* size = child(*handle, "Size");
            double width{}, height{};
            if (!size || !parse_pair(text_of(*size), width, height) || width <= 0 || height <= 0)
                return fail("Handle/Size is missing or not two positive numbers");
            definition.width = width;
            definition.height = height;
            if (const auto* node = child(*handle, "Caption")) definition.caption = text_of(*node);
            if (const auto* node = child(*handle, "Tooltip")) definition.tooltip = text_of(*node);
            if (const auto* node = child(*handle, "Normal_Texture")) definition.normal = text_of(*node);
            if (const auto* node = child(*handle, "Hover_Texture")) definition.hover = text_of(*node);
            if (const auto* node = child(*handle, "Pressed_Texture")) definition.pressed = text_of(*node);
            if (const auto* node = child(*handle, "Disabled_Texture")) definition.disabled = text_of(*node);
            definition.stock_art = definition.normal.empty();
            if (!parse_candidates(*handle, definition.candidates))
                return fail("a Handle Candidate needs an Anchor, a known Side and a two-number Offset");
            if (definition.candidates.empty()) return fail("an enabled Handle needs at least one Candidate");
        }
        layout.handle = std::move(definition);
    }

    if (const auto* close = child(root, "Close_Button")) {
        // GBT-07: replaced whole, like the handle.
        CloseDefinition definition;
        definition.candidates.clear();
        definition.art_components.clear();
        if (const auto* node = child(*close, "Enabled")) {
            const auto flag = parse_flag(text_of(*node));
            if (!flag) return fail("Close_Button/Enabled is not true or false");
            definition.enabled = *flag;
        }
        if (definition.enabled) {
            const auto* size = child(*close, "Size");
            double width{}, height{};
            if (!size || !parse_pair(text_of(*size), width, height) || width <= 0 || height <= 0)
                return fail("Close_Button/Size is missing or not two positive numbers");
            definition.width = width;
            definition.height = height;
            if (const auto* node = child(*close, "Caption")) definition.caption = text_of(*node);
            if (const auto* node = child(*close, "Tooltip")) definition.tooltip = text_of(*node);
            for (const auto& item : close->children) {
                if (!core::iequals(item.name, "Art_Component")) continue;
                auto value = text_of(item);
                if (value.empty()) return fail("a Close_Button Art_Component is empty");
                definition.art_components.push_back(std::move(value));
            }
            if (!parse_candidates(*close, definition.candidates))
                return fail("a Close_Button Candidate needs an Anchor, a known Side and a two-number Offset");
            if (definition.candidates.empty()) return fail("an enabled Close_Button needs at least one Candidate");
        }
        layout.close = std::move(definition);
    }

    if (const auto* tray = child(root, "Tray")) {
        const auto number = [&](const std::string_view tag, double& out, const double low, const double high) {
            const auto* node = child(*tray, tag);
            if (!node) return true;
            double value{};
            if (!parse_number(text_of(*node), value) || value < low || value > high) return false;
            out = value;
            return true;
        };
        const auto name = [&](const std::string_view tag, std::string& out) {
            const auto* node = child(*tray, tag);
            if (!node) return true;
            auto value = text_of(*node);
            if (value.empty()) return false;
            out = std::move(value);
            return true;
        };
        constexpr double unbounded = 1.0e9;
        if (!name("Dialog_Name", layout.dialog_name)) return fail("Tray/Dialog_Name is empty");
        if (!name("Entry_Template", layout.entry_template)) return fail("Tray/Entry_Template is empty");
        if (!number("Entry_Scale", layout.entry_scale, 0.5, 2.0)) return fail("Tray/Entry_Scale is not a number from 0.5 to 2");
        layout.compact_entry_scale = std::min(layout.compact_entry_scale, layout.entry_scale);
        if (!number("Compact_Entry_Scale", layout.compact_entry_scale, 0.5, 2.0)
            || layout.compact_entry_scale > layout.entry_scale)
            return fail("Tray/Compact_Entry_Scale is not a number from 0.5 up to Entry_Scale");
        if (!number("Entry_Gap", layout.entry_gap, 0.0, unbounded) || !number("Group_Gap", layout.group_gap, 0.0, unbounded)
            || !number("Row_Gap", layout.row_gap, 0.0, unbounded) || !number("Panel_Padding", layout.panel_padding, 0.0, unbounded))
            return fail("a Tray gap or the padding is not a number of 0 or more");
        if (!number("Header_Scale", layout.header_scale, 0.5, 2.0)) return fail("Tray/Header_Scale is not a number from 0.5 to 2");
        if (!name("Header_Template", layout.header_template)) return fail("Tray/Header_Template is empty");
        if (const auto* node = child(*tray, "Divider_Texture")) layout.divider_texture = text_of(*node);
        if (const auto* node = child(*tray, "Max_Rows")) {
            std::int64_t rows{};
            if (!parse_integer(text_of(*node), rows) || rows < 2 || rows > 10) return fail("Tray/Max_Rows is not an integer from 2 to 10");
            layout.max_rows = static_cast<std::size_t>(rows);
        }
        if (const auto* node = child(*tray, "Fold_Single_Choice_Groups")) {
            const auto flag = parse_flag(text_of(*node));
            if (!flag) return fail("Tray/Fold_Single_Choice_Groups is not Yes or No");
            layout.fold_single_choice_groups = *flag;
        }
        if (const auto* node = child(*tray, "Width_Mode")) {
            const auto mode = text_of(*node);
            if (core::iequals(mode, "Bar")) layout.width_mode = WidthMode::bar;
            else if (core::iequals(mode, "Fixed")) layout.width_mode = WidthMode::fixed;
            else if (core::iequals(mode, "Auto")) layout.width_mode = WidthMode::automatic;
            else return fail("Tray/Width_Mode is not Bar, Fixed or Auto");
        }
        if (!number("Width", layout.width, std::nextafter(0.0, 1.0), unbounded)) return fail("Tray/Width is not a number above 0");
        if (layout.width_mode == WidthMode::fixed && layout.width <= 0) return fail("Width_Mode Fixed needs a Width");
        if (!number("Max_Width_Share", layout.max_width_share, 0.1, 1.0))
            return fail("Tray/Max_Width_Share is not a number from 0.1 to 1");
        if (const auto* node = child(*tray, "Back_Source")) {
            const auto source = text_of(*node);
            if (core::iequals(source, "Bar")) layout.back_source = BackSource::bar;
            else if (core::iequals(source, "Component")) layout.back_source = BackSource::component;
            else return fail("Tray/Back_Source is not Bar or Component");
        }
        if (!name("Back_Component", layout.back_component)) return fail("Tray/Back_Component is empty");
        if (layout.back_source == BackSource::component && layout.back_component.empty())
            return fail("Back_Source Component needs a Back_Component");
        if (!number("Frame_Width", layout.frame_width, 0.0, 64.0)) return fail("Tray/Frame_Width is not a number from 0 to 64");
        if (const auto* limit = child(*tray, "Galaxy_Top_Limit")) {
            const auto* component = child(*limit, "Component");
            const auto* y = child(*limit, "Y");
            if ((component != nullptr) == (y != nullptr)) return fail("Galaxy_Top_Limit needs exactly one of Component and Y");
            if (component) {
                layout.galaxy_top_component = text_of(*component);
                if (layout.galaxy_top_component.empty()) return fail("Galaxy_Top_Limit/Component is empty");
            } else {
                double value{};
                if (!parse_number(text_of(*y), value)) return fail("Galaxy_Top_Limit/Y is not a number");
                layout.galaxy_top_y = value;
            }
        }
        for (const auto& item : tray->children) {
            if (!core::iequals(item.name, "Planet_View_Cap_Component")) continue;
            auto value = text_of(item);
            if (value.empty()) return fail("a Planet_View_Cap_Component is empty");
            layout.planet_view_cap_components.push_back(std::move(value));
        }
    }
    result.value = std::move(layout);
    return result;
}

Parsed<TrayGroups> parse_tray_groups(const data::XmlDocument& document) {
    Parsed<TrayGroups> result;
    const auto fail = [&](std::string message) {
        result.value.reset();
        result.error = std::move(message);
        return result;
    };
    if (!core::iequals(document.root.name, "Revised_Build_Tray_Groups")) return fail("the root element is not Revised_Build_Tray_Groups");
    TrayGroups groups;
    auto& definition = groups.definition;
    const auto& root = document.root;
    const auto reserved = [](const std::string_view name) {
        return core::iequals(name, "Buildings") || core::iequals(name, "Misc");
    };

    if (const auto* node = child(root, "Category_Priority"); node && !parse_name_list(node->raw_text, definition.category_priority))
        return fail("Category_Priority holds something that is not a category name");
    if (const auto* node = child(root, "Building_Categories"); node && !parse_name_list(node->raw_text, definition.building_categories))
        return fail("Building_Categories holds something that is not a category name");

    for (const auto& item : root.children) {
        if (core::iequals(item.name, "Group_Caption")) {
            auto name = attribute_of(item, "Name");
            auto key = text_of(item);
            if (!is_name(name) || key.empty()) return fail("a Group_Caption needs a plain Name and a text key");
            if (std::any_of(definition.captions.begin(), definition.captions.end(),
                    [&](const auto& other) { return core::iequals(other.first, name); }))
                return fail("Group_Caption " + name + " is defined twice");
            definition.captions.emplace_back(std::move(name), std::move(key));
        } else if (core::iequals(item.name, "Extra_Group")) {
            ExtraGroup extra;
            extra.name = attribute_of(item, "Name");
            if (!is_name(extra.name) || reserved(extra.name)) return fail("an Extra_Group needs a plain Name that is not Buildings or Misc");
            if (std::any_of(definition.extra_groups.begin(), definition.extra_groups.end(),
                    [&](const auto& other) { return core::iequals(other.name, extra.name); }))
                return fail("Extra_Group " + extra.name + " is defined twice");
            if (const auto* node = child(item, "Caption")) extra.caption = text_of(*node);
            if (const auto* node = child(item, "Position")) {
                const auto position = text_of(*node);
                if (core::iequals(position, "Start")) extra.where = ExtraGroup::Where::start;
                else if (core::iequals(position, "End")) extra.where = ExtraGroup::Where::end;
                else if (position.size() > 6 && core::iequals(position.substr(0, 5), "After")
                    && (position[5] == ' ' || position[5] == '\t')
                    && is_name(core::trim_view(std::string_view(position).substr(5), core::xml_whitespace))) {
                    extra.where = ExtraGroup::Where::after;
                    extra.after = core::trim(std::string_view(position).substr(5), core::xml_whitespace);
                } else {
                    return fail("Extra_Group " + extra.name + " has a Position that is not Start, End or After <category>");
                }
            }
            definition.extra_groups.push_back(std::move(extra));
        } else if (core::iequals(item.name, "Type_Assignment")) {
            TypeAssignment assignment;
            if (const auto* node = child(item, "Type")) assignment.type = text_of(*node);
            if (const auto* node = child(item, "Group")) assignment.group = text_of(*node);
            const auto star = assignment.type.find('*');
            const bool pattern_ok = star == std::string::npos || (star + 1 == assignment.type.size() && star > 0);
            const auto stem = star == std::string::npos ? std::string_view(assignment.type)
                                                        : std::string_view(assignment.type).substr(0, star);
            if (!pattern_ok || !is_type_stem(stem) || !is_name(assignment.group))
                return fail("a Type_Assignment needs a Type (a name, or a name prefix ending in *) and a Group");
            if (const auto* node = child(item, "Sort_Key")) {
                std::int64_t key{};
                if (!parse_integer(text_of(*node), key)) return fail("a Type_Assignment Sort_Key is not an integer");
                assignment.sort_key = key;
            }
            definition.assignments.push_back(std::move(assignment));
        }
    }
    result.value = std::move(groups);
    return result;
}

namespace {

template <typename T, typename Parse>
Loaded<T> load_layered(const vfs::Vfs& files, const std::string_view path, Parse parse) {
    Loaded<T> result;
    auto candidates = files.candidates(path);
    if (!candidates) return result;
    // The candidates run from the highest layer down; one file per layer, applied lowest first.
    std::vector<vfs::AssetRecord> layers;
    for (const auto& candidate : candidates.value()) {
        if (std::none_of(layers.begin(), layers.end(), [&](const auto& row) { return row.layer_id == candidate.layer_id; }))
            layers.push_back(candidate);
    }
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const auto document = data::load_document(files, *it);
        auto parsed = document ? parse(document.value()) : Parsed<T>{std::nullopt, "the file is not readable XML"};
        if (parsed.value) {
            result.value = std::move(parsed.value);
        } else {
            result.invalid = true;
            if (result.first_error.empty()) result.first_error = it->canonical_path + ": " + parsed.error;
        }
    }
    return result;
}

bool is_category(const CategoryEnum& categories, const std::string_view name) noexcept {
    return categories.index_of(name).has_value();
}

} // namespace

Loaded<TrayLayout> load_tray_layout(const vfs::Vfs& files) {
    return load_layered<TrayLayout>(files, layout_path, [](const data::XmlDocument& document) { return parse_tray_layout(document); });
}

Loaded<TrayGroups> load_tray_groups(const vfs::Vfs& files) {
    return load_layered<TrayGroups>(files, groups_path, [](const data::XmlDocument& document) { return parse_tray_groups(document); });
}

CategoryEnum load_category_enum(const vfs::Vfs& files) {
    CategoryEnum result;
    const auto document = data::load_document(files, "data/xml/enum/gameobjectcategorytype.xml");
    if (!document) return result;
    for (const auto& item : document.value().root.children) result.names.push_back(item.name);
    return result;
}

bool layer_enabled_by_default(const Loaded<TrayLayout>& layout) {
    return layout.value ? layout.value->enabled_by_default : true;
}

bool layer_enabled_by_default(const vfs::Vfs& files) {
    return layer_enabled_by_default(load_tray_layout(files));
}

std::vector<std::string> check_against_categories(const GroupDefinition& definition, const CategoryEnum& categories) {
    std::vector<std::string> problems;
    const auto extra = [&](const std::string_view name) {
        return std::any_of(definition.extra_groups.begin(), definition.extra_groups.end(),
            [&](const auto& group) { return core::iequals(group.name, name); });
    };
    for (const auto& group : definition.extra_groups) {
        if (is_category(categories, group.name) || core::iequals(group.name, "Buildings") || core::iequals(group.name, "Misc"))
            problems.push_back("Extra_Group " + group.name + " is named like a category, Buildings or Misc");
        if (group.where == ExtraGroup::Where::after && !is_category(categories, group.after))
            problems.push_back("Extra_Group " + group.name + " is placed after " + group.after + ", which the category enum does not define");
    }
    for (const auto& assignment : definition.assignments) {
        if (!core::iequals(assignment.group, "Buildings") && !extra(assignment.group) && !is_category(categories, assignment.group))
            problems.push_back("Type_Assignment " + assignment.type + " names the group " + assignment.group
                + ", which neither the file nor the category enum defines");
    }
    return problems;
}

std::size_t stale_assignments(const GroupDefinition& definition, const TypeIndex& types) {
    std::size_t stale{};
    for (const auto& assignment : definition.assignments) {
        const bool prefix = !assignment.type.empty() && assignment.type.back() == '*';
        const bool defined = prefix ? types.any_with_prefix(std::string_view(assignment.type).substr(0, assignment.type.size() - 1))
                                    : types.find(assignment.type) != nullptr;
        if (!defined) ++stale;
    }
    return stale;
}

} // namespace eawr::presentation::ui::build_tray

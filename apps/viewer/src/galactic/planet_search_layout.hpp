#pragma once

#include "eawr/core/text.hpp"
#include "eawr/data/ui/dialog_script.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {

// DEV-25 / PS-01: geometry is project-layer data; the kit supplies the normal
// dialog, edit and list skins, including per-control GUIDialogs.xml overrides.
struct PlanetSearchLayout {
    data::ui::Dialog dialog;
    float inset_x{}, inset_y{};
    bool picture{true}, emblem{true};
    // PS-04: the result rows' font size relative to the kit's List_Box size; both row icons follow the text height.
    float row_scale{1.0F};
};

[[nodiscard]] inline std::optional<PlanetSearchLayout> parse_planet_search_layout(const data::XmlDocument& document) {
    if (!core::iequals(document.root.name, "Planet_Search")) return {};
    const auto field = [](const data::XmlNode& node, std::string_view tag) {
        for (const auto& child : node.children) if (core::iequals(child.name, tag))
            return core::trim(child.raw_text, core::xml_whitespace);
        return std::string{};
    };
    const auto pair = [](const std::string& text, auto& x, auto& y) {
        auto first = text.data();
        const auto end = first + text.size();
        const auto read = [&](auto& value) {
            while (first != end && core::xml_whitespace.find(*first) != std::string_view::npos) ++first;
            const auto parsed = std::from_chars(first, end, value);
            first = parsed.ptr;
            return parsed.ec == std::errc{} && std::isfinite(static_cast<double>(value));
        };
        if (!read(x) || !read(y)) return false;
        while (first != end && core::xml_whitespace.find(*first) != std::string_view::npos) ++first;
        return first == end;
    };
    PlanetSearchLayout result;
    result.dialog.name = "IDD_EAWR_PLANET_SEARCH";
    result.dialog.rect = {};
    if (!pair(field(document.root, "Size"), result.dialog.rect.width, result.dialog.rect.height)
        || result.dialog.rect.width <= 0 || result.dialog.rect.height <= 0
        || !pair(field(document.root, "Offset"), result.inset_x, result.inset_y)
        || result.inset_x < 0 || result.inset_y < 0) return {};
    result.picture = !core::iequals(field(document.root, "Planet_Picture"), "false");
    result.emblem = !core::iequals(field(document.root, "Owner_Emblem"), "false");
    if (const auto scale = field(document.root, "Row_Scale"); !scale.empty()) {
        const auto first = scale.data(), end = scale.data() + scale.size();
        const auto parsed = std::from_chars(first, end, result.row_scale);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(result.row_scale)
            || result.row_scale < 0.5F || result.row_scale > 3.0F) return {};
    }
    for (const auto& node : document.root.children) {
        if (!core::iequals(node.name, "Control")) continue;
        data::ui::DialogControl control;
        for (const auto& attribute : node.attributes)
            if (core::iequals(attribute.name, "Name")) control.id_name = attribute.value;
        const auto type = field(node, "Type");
        if (core::iequals(type, "Text_Field")) control.statement = "EDITTEXT";
        else if (core::iequals(type, "List")) control.statement = "LISTBOX";
        else if (core::iequals(type, "TextButton")) control.statement = "PUSHBUTTON";
        else return {};
        if (control.id_name.empty() || !pair(field(node, "Offset"), control.rect.x, control.rect.y)
            || !pair(field(node, "Size"), control.rect.width, control.rect.height)
            || control.rect.x < 0 || control.rect.y < 0 || control.rect.width <= 0 || control.rect.height <= 0
            || static_cast<std::int64_t>(control.rect.x) + control.rect.width > result.dialog.rect.width
            || static_cast<std::int64_t>(control.rect.y) + control.rect.height > result.dialog.rect.height) return {};
        control.text = field(node, "Caption");
        control.style = {"WS_VISIBLE", "WS_TABSTOP"};
        result.dialog.controls.push_back(std::move(control));
    }
    const auto has = [&](std::string_view name, std::string_view type) {
        return std::count_if(result.dialog.controls.begin(), result.dialog.controls.end(), [&](const auto& control) {
            return control.id_name == name && control.statement == type;
        }) == 1;
    };
    if (!has("IDC_PLANET_SEARCH_TEXT", "EDITTEXT") || !has("IDC_PLANET_SEARCH_RESULTS", "LISTBOX")
        || !has("IDC_PLANET_SEARCH_CLOSE", "PUSHBUTTON")) return {};
    return result;
}

// A complete valid higher-layer definition replaces the lower one. Broken
// restyles retain the last valid layer and are reported once by the viewer.
[[nodiscard]] inline std::optional<PlanetSearchLayout> load_planet_search_layout(const vfs::Vfs& files, bool& invalid) {
    invalid = false;
    auto candidates = files.candidates("Data/XML/RevisedPlanetSearch.xml");
    if (!candidates) return {};
    std::optional<PlanetSearchLayout> result;
    std::vector<vfs::AssetRecord> layers;
    for (const auto& candidate : candidates.value()) {
        if (std::none_of(layers.begin(), layers.end(), [&](const auto& row) { return row.layer_id == candidate.layer_id; }))
            layers.push_back(candidate);
    }
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const auto document = data::load_document(files, *it);
        auto next = document ? parse_planet_search_layout(document.value()) : std::nullopt;
        if (next) result = std::move(next); else invalid = true;
    }
    return result;
}

} // namespace eawr::viewer::galactic

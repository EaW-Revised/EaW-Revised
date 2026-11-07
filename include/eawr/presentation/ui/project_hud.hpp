#pragma once

#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/text.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::presentation::ui {

struct ProjectHudCandidate {
    std::string anchor;
    std::string side;
    double x{}, y{};
};
struct ProjectHudControl {
    bool enabled{true};
    bool stock_art{};
    double width{}, height{}, list_width{}, row_height{};
    // Space between recipient rows and between the list and the button; stock 2, authored 0.
    double row_gap{};
    std::string caption, tooltip, normal, hover, pressed, disabled;
    std::vector<ProjectHudCandidate> candidates;
};
using ProjectHudDefinitions = std::map<std::string, ProjectHudControl>;

// Full-viewport input proxies are not themselves occupied: their actual hit areas
// and any ordinary child controls must be recorded separately.
inline void record_project_hud_occupancy(std::vector<PixelRect>& occupied, const PixelRect rect,
    const bool input_wrapper) {
    if (!input_wrapper) occupied.push_back(rect);
}

[[nodiscard]] inline ProjectHudControl default_gift() {
    ProjectHudControl gift;
    gift.stock_art = true;
    gift.width = 96; gift.height = 30; gift.list_width = 180; gift.row_height = 30; gift.row_gap = 2;
    gift.caption = "TEXT_EAWR_GIFT_UNITS";
    // The stock position (above attack, else bottom-left) stays first; the others only apply when it is blocked.
    gift.candidates = {{"attack", "above", 0, -4}, {"viewport", "inside_bottom_left", 12, -190},
        {"viewport", "inside_bottom_right", -100, -190}, {"move", "above", 0, -4},
        {"viewport", "inside_top_left", 40, 120}, {"viewport", "inside_top_right", -100, 120}};
    return gift;
}

// Remembers the sampled layout values of one frame so the HUD re-docks only when they change.
// After the first frames it allocates nothing: both buffers keep their capacity.
class ProjectHudLayoutWatch {
public:
    void begin() { sample_.clear(); }
    void add(const double value) { sample_.push_back(value); }
    void add(const double x, const double y, const double width, const double height) {
        sample_.insert(sample_.end(), {x, y, width, height});
    }
    // True when the sample differs from the last committed one.
    [[nodiscard]] bool commit() {
        if (sample_ == committed_) return false;
        committed_.swap(sample_);
        ++changes_;
        return true;
    }
    [[nodiscard]] std::size_t changes() const noexcept { return changes_; }
private:
    std::vector<double> sample_, committed_;
    std::size_t changes_{};
};

// Project files merge per named control, with the winning candidate from each layer.
[[nodiscard]] inline ProjectHudDefinitions load_project_hud(const vfs::Vfs& filesystem, bool* invalid = nullptr) {
    if (invalid) *invalid = false;
    ProjectHudDefinitions definitions{{"gift", default_gift()}};
    auto records = filesystem.candidates("data/xml/eawrhud.xml");
    if (!records) return definitions;
    std::vector<vfs::AssetRecord> layers;
    for (const auto& record : records.value()) {
        if (std::none_of(layers.begin(), layers.end(), [&](const auto& other) { return other.layer_id == record.layer_id; }))
            layers.push_back(record);
    }
    const auto value = [](const data::XmlNode& node, const std::string_view key) {
        for (const auto& child : node.children) if (core::iequals(child.name,key)) {
            const auto first = child.raw_text.find_first_not_of(core::xml_whitespace);
            if (first == std::string::npos) return std::string{};
            return child.raw_text.substr(first, child.raw_text.find_last_not_of(core::xml_whitespace) - first + 1);
        }
        return std::string{};
    };
    const auto pair = [](const std::string& text, double& x, double& y) {
        std::istringstream input(text);
        double first{}, second{};
        if (!(input >> first >> second) || !std::isfinite(first) || !std::isfinite(second)) return false;
        x = first; y = second; return true;
    };
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        auto doc = data::load_document(filesystem,*it);
        // An unreadable higher layer must not silently resurrect lower-layer controls.
        if (!doc || !core::iequals(doc.value().root.name,"Eawr_Hud")) {
            if (invalid) *invalid = true;
            definitions.clear();
            continue;
        }
        for (const auto& node : doc.value().root.children) {
            if (!core::iequals(node.name,"Control")) continue;
            std::string name;
            for (const auto& a : node.attributes) if (core::iequals(a.name,"Name")) name = a.value;
            if (name.empty()) continue;
            name = core::to_lower(name);
            ProjectHudControl control;
            const auto enabled = value(node,"Enabled");
            control.enabled = !core::iequals(enabled,"false") && enabled != "0";
            pair(value(node,"Size"),control.width,control.height);
            pair(value(node,"Recipient_Size"),control.list_width,control.row_height);
            control.caption = value(node,"Caption"); control.tooltip = value(node,"Tooltip");
            control.normal = value(node,"Normal_Texture"); control.hover = value(node,"Hover_Texture");
            control.pressed = value(node,"Pressed_Texture"); control.disabled = value(node,"Disabled_Texture");
            for (const auto& candidate : node.children) {
                if (!core::iequals(candidate.name,"Candidate")) continue;
                ProjectHudCandidate c;
                c.anchor = value(candidate,"Anchor"); c.side = value(candidate,"Side");
                pair(value(candidate,"Offset"),c.x,c.y);
                control.candidates.push_back(std::move(c));
            }
            definitions[name] = std::move(control);
        }
    }
    return definitions;
}

// The recipient list sits above the button: one row pitch per recipient plus the gap to the button.
[[nodiscard]] inline PixelRect project_hud_list_rect(const ProjectHudControl& control, const PixelRect button,
    const double scale, const std::size_t recipients) {
    const double rows = (control.row_height + control.row_gap) * scale * static_cast<double>(recipients);
    return {button.x, button.y - rows - control.row_gap * scale, control.list_width * scale, rows};
}

// Rectangles use viewport pixels; candidates and sizes scale from UI reference units.
[[nodiscard]] inline std::optional<PixelRect> dock_project_hud(const ProjectHudControl& control,
    const PixelRect viewport, const double scale, const std::map<std::string,PixelRect>& anchors,
    const std::vector<PixelRect>& occupied, const std::size_t recipients = 0) {
    if (!control.enabled || scale <= 0 || control.width <= 0 || control.height <= 0) return std::nullopt;
    const double w = control.width * scale, h = control.height * scale;
    for (const auto& candidate : control.candidates) {
        PixelRect anchor;
        if (core::iequals(candidate.anchor, "viewport")) anchor = viewport;
        else {
            const auto it = std::find_if(anchors.begin(),anchors.end(),[&](const auto& row) { return core::iequals(row.first,candidate.anchor); });
            if (it == anchors.end()) continue;
            anchor = it->second;
        }
        double x = anchor.x, y = anchor.y;
        if (candidate.side == "above") y -= h;
        else if (candidate.side == "below") y += anchor.height;
        else if (candidate.side == "left") x -= w;
        else if (candidate.side == "right") x += anchor.width;
        else if (candidate.side == "inside_top_left") {}
        else if (candidate.side == "inside_top_right") x += anchor.width - w;
        else if (candidate.side == "inside_bottom_left") y += anchor.height - h;
        else if (candidate.side == "inside_bottom_right") { x += anchor.width - w; y += anchor.height - h; }
        else continue;
        x += candidate.x * scale; y += candidate.y * scale;
        const PixelRect button{x,y,w,h};
        std::vector<PixelRect> proposed{button};
        if (recipients) {
            auto list = project_hud_list_rect(control, button, scale, recipients);
            list.height += control.row_gap * scale; // the gap above the button is kept clear too
            proposed.push_back(list);
        }
        bool safe = true;
        for (const auto& r : proposed) {
            if (r.width <= 0 || r.height <= 0 || r.x < viewport.x || r.y < viewport.y || r.right() > viewport.right() || r.bottom() > viewport.bottom()) { safe = false; break; }
            for (const auto& other : occupied) if (r.x < other.right() && r.right() > other.x && r.y < other.bottom() && r.bottom() > other.y) { safe = false; break; }
            if (!safe) break;
        }
        if (safe) return button;
    }
    return std::nullopt;
}
} // namespace eawr::presentation::ui

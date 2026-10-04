#include "battle_input.hpp"

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;

namespace {
[[nodiscard]] std::string json(const std::string& text) {
    std::string result{"\""};
    for (const char character : text) {
        if (character == '"' || character == '\\') result += '\\';
        result += static_cast<unsigned char>(character) < 0x20U ? ' ' : character;
    }
    return result + "\"";
}

} // namespace

void BattleInput::draw() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr || !canvas_item_.is_valid()) return;
    rendering->canvas_item_clear(canvas_item_);
    // #424: FoC's selection circles, bars, squadron icons and hardpoint reticles.
    if (live_ != nullptr) {
        WorldUiView::Frame view;
        view.units = &units_;
        view.selection = &selection_;
        view.hovered = hovered_;
        view.hovered_icon = hovered_icon_;
        view.pointer = pointer_;
        view.camera = frame_;
        view.viewport = viewport_;
        view.project = [this](const ui::Vec3f& source) { return project(source); };
        view.live = live_;
        view.abilities = ability_state_ != nullptr ? ability_state_ : &ready_abilities_;
        view.brackets = overview_ui_.unit_brackets;
        world_ui_->draw(view, canvas_item_);
    }
    // FoC's drag box (SelectBoxBorderColor / SelectBoxFillColor): once the drag passes the select
    // distance, or at once while nothing is selected.
    if (left_ && left_->moved
        && (ui::drag_extent(left_->start, left_->end) > ui::minimum_drag_select_distance || selection_.empty())) {
        const ui::ScreenRect rect = ui::drag_rect(left_->start, left_->end);
        const float width = std::max(3.0F, rect.max_x - rect.min_x);
        const float height = std::max(3.0F, rect.max_y - rect.min_y);
        const Color fill(200.0F / 255.0F, 50.0F / 255.0F, 50.0F / 255.0F, 50.0F / 255.0F);
        const Color border(200.0F / 255.0F, 50.0F / 255.0F, 50.0F / 255.0F, 1.0F);
        rendering->canvas_item_add_rect(canvas_item_, Rect2(rect.min_x + 1.0F, rect.min_y + 1.0F, width - 1.0F, height - 1.0F), fill);
        const Vector2 a(rect.min_x, rect.min_y), b(rect.min_x + width, rect.min_y), c(rect.min_x + width, rect.min_y + height),
            d(rect.min_x, rect.min_y + height);
        rendering->canvas_item_add_line(canvas_item_, a, b, border, 1.0F);
        rendering->canvas_item_add_line(canvas_item_, b, c, border, 1.0F);
        rendering->canvas_item_add_line(canvas_item_, c, d, border, 1.0F);
        rendering->canvas_item_add_line(canvas_item_, d, a, border, 1.0F);
    }
}

void BattleInput::write_report(std::ostream& output, const SpaceEnvironment& space) const {
    output << "  \"battle_input\": {\"events\": " << events_ << ", \"pickable_units\": " << units_.size();
    output << ", \"cursor\": ";
    cursor_.write_report(output);
    output << ", \"cursor_samples\": [";
    const auto& samples = cursor_history_.samples();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        output << (index ? "," : "") << "{\"frame\":" << sample.frame << ",\"id\":" << json(sample.id)
            << ",\"hover\":" << json(sample.hover) << '}';
    }
    output << ']';
    // #665: how many pick volumes carry a collision mesh and an override sphere.
    std::size_t meshes = 0;
    std::size_t spheres = 0;
    for (const ui::BattleUnit& unit : units_) {
        meshes += unit.mesh != nullptr && !unit.mesh->triangles.empty() ? 1U : 0U;
        spheres += unit.sphere_radius > 0.0F ? 1U : 0U;
    }
    output << ", \"pick_meshes\": " << meshes << ", \"pick_spheres\": " << spheres << ", \"double_click_candidates\": [";
    for (std::size_t click = 0; click < double_click_candidates_.size(); ++click) {
        output << (click ? ", " : "") << "[";
        const auto& candidates = double_click_candidates_[click];
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const PickCandidate& candidate = candidates[index];
            const auto number = [](const std::optional<float> value) {
                return value ? std::to_string(*value) : std::string("null");
            };
            output << (index ? ", " : "") << "{\"entity\": " << candidate.entity << ", \"part\": "
                   << (candidate.part == sim::invalid_entity_id ? std::string("null") : std::to_string(candidate.part))
                   << ", \"contact_z\": " << number(candidate.contact_z) << ", \"volume\": "
                   << (candidate.volume != nullptr ? json(candidate.volume) : std::string("null"))
                   << ", \"box_z\": " << number(candidate.box_z) << "}";
        }
        output << "]";
    }
    output << "], \"selected\": [";
    for (std::size_t index = 0; index < selection_.units().size(); ++index) {
        output << (index ? ", " : "") << selection_.units()[index];
    }
    output << "], \"groups\": {";
    bool first = true;
    for (std::size_t group = 0; group < ui::control_group_count; ++group) {
        if (selection_.group(group).empty()) continue;
        output << (first ? "" : ", ") << "\"" << group << "\": [";
        first = false;
        for (std::size_t index = 0; index < selection_.group(group).size(); ++index) {
            output << (index ? ", " : "") << selection_.group(group)[index];
        }
        output << "]";
    }
    output << "}, \"orders\": " << orders_ << ", \"hardpoint_orders\": " << hardpoint_orders_
           << ", \"refused\": " << refused_ << ", \"boxes\": " << boxes_
           << ", \"camera_focuses\": " << focuses_ << ", \"camera_follow_moves\": " << follow_moves_
           << ", \"scripted_fired\": " << scripted_fired_
           << ", \"overview\": " << json(space.live_camera_overview()) << ", \"scripted_points\": [";
    for (std::size_t index = 0; index < scripted_points_.size(); ++index) {
        const ScriptedPoint& point = scripted_points_[index];
        output << (index ? ", " : "") << "{\"kind\": " << json(point.kind) << ", \"tick\": " << point.tick
               << ", \"at\": [" << point.at[0] << ", " << point.at[1] << "]";
        if (point.to) output << ", \"to\": [" << (*point.to)[0] << ", " << (*point.to)[1] << "]";
        output << "}";
    }
    output << "], \"overview_samples\": [";
    for (std::size_t index = 0; index < overview_samples_.size(); ++index) {
        const OverviewSample& sample = overview_samples_[index];
        output << (index ? ", " : "") << "{\"tick\": " << sample.tick
               << ", \"level\": " << json(sample.level) << ", \"circles\": " << sample.drawn.circles
               << ", \"health_bars\": " << sample.drawn.health_bars << ", \"shield_bars\": " << sample.drawn.shield_bars
               << ", \"icons\": " << sample.drawn.icons << ", \"reticles\": " << sample.drawn.reticles
               << ", \"hud\": " << sample.hud << '}';
    }
    output << "], \"log\": [";
    for (std::size_t index = 0; index < log_.size(); ++index) output << (index ? ", " : "") << json(log_[index]);
    output << "], \"camera_samples\": [";
    const auto vector = [&output](const std::array<float, 3>& value) {
        output << "[" << value[0] << ", " << value[1] << ", " << value[2] << "]";
    };
    for (std::size_t index = 0; index < camera_samples_.size(); ++index) {
        const CameraSample& sample = camera_samples_[index];
        output << (index ? ", " : "") << "{\"label\": " << json(sample.label) << ", \"eye\": ";
        vector(sample.frame.eye);
        output << ", \"target\": ";
        vector(sample.frame.target);
        output << "}";
    }
    // #425: the cards the HUD draws for the selection, and the card clicks taken.
    output << "], \"unit_cards\": {\"slots\": " << card_slots_ << ", \"clicks\": " << card_clicks_
           << ", \"groups\": " << card_layout_.groups << ", \"cards\": [";
    for (std::size_t index = 0; index < card_layout_.cards.size(); ++index) {
        const ui::UnitCard& card = card_layout_.cards[index];
        const ui::CardUnit& unit = card_units_[card.unit];
        output << (index ? ", " : "") << "{\"slot\": " << card.slot << ", \"unit\": " << unit.id
               << ", \"type\": " << json(unit.type) << ", \"squadron\": " << (unit.squadron ? "true" : "false")
               << ", \"ability\": " << card.ability << ", \"count\": " << card.count
               << ", \"stacked\": " << (card.stacked ? "true" : "false") << ", \"health_level\": " << card.health_level
               << ", \"shield\": ";
        if (card.shield) output << *card.shield;
        else output << "null";
        output << ", \"members\": [";
        for (std::size_t member = 0; member < unit.members.size(); ++member) {
            output << (member ? ", " : "") << unit.members[member];
        }
        output << "]}";
    }
    output << "], \"borders\": [";
    constexpr std::array<const char*, 4> pieces{"full", "left", "centre", "right"};
    for (std::size_t index = 0; index < card_layout_.borders.size(); ++index) {
        const ui::CardBorder& border = card_layout_.borders[index];
        output << (index ? ", " : "") << "{\"column\": " << border.column << ", \"piece\": \""
               << pieces[static_cast<std::size_t>(border.piece)] << "\"}";
    }
    output << "]}";
    // #454: the ability buttons the HUD draws, and the requests the buttons and keys made.
    output << ", \"ability_bar\": {\"requests\": " << ability_requests_ << ", \"hotkeys\": " << ability_hotkeys_
           << ", \"targeted\": " << ability_targeted_ << ", \"target_cancels\": " << ability_target_cancels_
           << ", \"targeting\": " << (ability_target_ ? "true" : "false")
           << ", \"demo\": " << (ability_demo_ ? "true" : "false") << ", \"buttons\": [";
    for (std::size_t index = 0; index < ability_bar_.buttons.size(); ++index) {
        const ui::AbilityButton& button = ability_bar_.buttons[index];
        output << (index ? ", " : "") << "{\"component\": " << button.component << ", \"ability\": "
               << json(std::string(ui::ability_name(button.ability))) << ", \"units\": " << button.units.size() << "}";
    }
    output << "]}";
    // #530: the station's build buttons while it is the production object, and the clicks.
    const auto producer = production_station();
    output << ", \"production\": {\"station\": " << (producer ? std::to_string(*producer) : "null")
           << ", \"pad\": " << (pad_palette_.entity() ? "true" : "false")
           << ", \"clicks\": " << build_clicks_ << ", \"buys\": " << buys_ << ", \"placing\": "
           << (placing_ ? std::to_string(*placing_) : "null") << ", \"placements\": " << placements_
           << ", \"placements_cancelled\": " << placements_cancelled_ << ", \"buttons\": [";
    for (std::size_t index = 0; index < build_buttons_.size(); ++index) {
        const ui::BuildButton& button = build_buttons_[index];
        output << (index ? ", " : "") << "{\"slot\": " << button.slot << ", \"type\": " << button.type << ", \"price\": "
               << button.price << ", \"enabled\": " << (button.enabled ? "true" : "false") << ", \"state\": "
               << static_cast<int>(button.state) << "}";
    }
    output << "]}";
    output << ", \"hovered\": ";
    if (hovered_ && *hovered_ < units_.size()) {
        const ui::BattleUnit& unit = units_[*hovered_];
        output << (unit.part != sim::invalid_entity_id ? unit.part : unit.entity);
    } else {
        output << "null";
    }
    output << ", \"hovered_icon\": ";
    if (hovered_icon_) output << *hovered_icon_;
    else output << "null";
    output << "},\n";
    world_ui_->write_report(output);
}

} // namespace eawr::presentation::godot_backend

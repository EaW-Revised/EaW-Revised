#include "story_summary_widget.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/battle_overlay.hpp"
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <algorithm>

namespace eawr::presentation::godot_backend {
namespace story = sim::galactic::story;
using presentation::ui::StorySummary;

namespace {
StorySummary::Request request_of(const story::SummaryAction action) {
    switch (action) {
    case story::SummaryAction::open: return StorySummary::Request::open;
    case story::SummaryAction::refresh: return StorySummary::Request::refresh;
    case story::SummaryAction::flash_button: return StorySummary::Request::flash_button;
    case story::SummaryAction::refresh_or_flash: return StorySummary::Request::refresh_or_flash;
    }
    return StorySummary::Request::flash_button;
}
// Dialog files live in Data/Scripts/Story/<name>.txt (WST-58).
std::string dialog_text(const vfs::Vfs& files, const std::string& name) {
    auto bytes = files.open("data/scripts/story/" + name + ".txt");
    if (!bytes) return {};
    return std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
}
} // namespace

void StorySummaryWidget::feed(GalacticMode::State& state) {
    const auto published = state.game->story_presentation_after(cursor_, sim::SeatId{state.galaxy->local.seat});
    // Expired one-shot notifications stay dropped. Fail closed rather than refresh from a stale goal cache;
    // full saved-goal rehydration after load or a feed gap remains a presentation follow-up.
    if (published.incomplete) { goals_.clear(); model_.close(); }
    for (const auto& row : published.events) {
        cursor_ = row.cursor;
        if (const auto* goal = std::get_if<story::GoalChanged>(&row.event.payload)) {
            if (goal->value) goals_[goal->tag] = *goal->value; else goals_.erase(goal->tag);
        } else if (const auto* summary = std::get_if<story::SummaryRequest>(&row.event.payload)) {
            model_.request(request_of(summary->action), summary->tag, summary->chapter);
        }
    }
    cursor_ = published.through; // the cursor includes filtered notifications for the other owners
}

void StorySummaryWidget::build(GalacticMode::State& state) {
    root_ = memnew(godot::Control);
    root_->set_name("galactic_story_summary"); root_->set_z_index(20);
    root_->set_anchors_and_offsets_preset(godot::Control::PRESET_FULL_RECT);
    root_->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE); root_->hide();
    panel_ = memnew(godot::PanelContainer);
    panel_->set_anchors_and_offsets_preset(godot::Control::PRESET_CENTER);
    panel_->set_custom_minimum_size({640, 0});
    panel_->set_h_grow_direction(godot::Control::GROW_DIRECTION_BOTH);
    panel_->set_v_grow_direction(godot::Control::GROW_DIRECTION_BOTH);
    godot::Ref<godot::StyleBoxFlat> style; style.instantiate();
    style->set_bg_color(godot::Color(0.02F, 0.04F, 0.08F, 0.93F));
    style->set_border_color(godot::Color(0.35F, 0.6F, 0.9F, 1.0F));
    style->set_border_width_all(2); style->set_content_margin_all(24);
    panel_->add_theme_stylebox_override("panel", style);
    auto* column = memnew(godot::VBoxContainer);
    column->add_theme_constant_override("separation", 16);
    title_ = memnew(godot::Label); title_->add_theme_font_size_override("font_size", 28);
    title_->set_autowrap_mode(godot::TextServer::AUTOWRAP_WORD_SMART);
    body_ = memnew(godot::Label); body_->add_theme_font_size_override("font_size", 20);
    body_->set_autowrap_mode(godot::TextServer::AUTOWRAP_WORD_SMART);
    body_->set_custom_minimum_size({592, 0});
    relay_ = memnew(EawrOverlayButton);
    relay_->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE);
    relay_->set_action([this] { model_.close(); });
    close_ = memnew(godot::Button);
    close_->set_text("OK"); close_->set_focus_mode(godot::Control::FOCUS_NONE);
    close_->connect("pressed", callable_mp(relay_, &EawrOverlayButton::activate));
    relay_->add_child(close_);
    column->add_child(title_); column->add_child(body_); column->add_child(relay_);
    panel_->add_child(column); root_->add_child(panel_); state.hud->add_child(root_);
}

void StorySummaryWidget::redraw(GalacticMode::State& state) {
    if (!text_) {
        auto database = data::ui::load_language_text_database(*state.filesystem, "english");
        if (database) text_ = std::make_shared<data::ui::TextDatabase>(std::move(database).value());
    }
    const auto resolve = [&](const std::string& key) {
        const auto* value = text_ ? text_->find(key) : nullptr;
        return godot::String::utf8((value ? data::ui::to_utf8(value->value) : key).c_str());
    };
    const auto goal = goals_.find(model_.tag());
    const auto chapter = goal == goals_.end() ? std::nullopt
        : presentation::ui::story_dialog_chapter(dialog_text(*state.filesystem, goal->second.dialog), model_.chapter());
    if (!chapter) { model_.close(); return; } // nothing sourced to show: stay shut rather than invent text
    title_->set_text(resolve(chapter->title));
    godot::String body;
    for (std::size_t i = 0; i < chapter->lines.size(); ++i)
        body += (i ? godot::String("\n\n") : godot::String()) + resolve(chapter->lines[i]);
    body_->set_text(body);
}

void StorySummaryWidget::service(GalacticMode::State& state) {
    if (!state.hud || !state.galaxy || !state.game || !state.filesystem) return;
    feed(state);
    if (!root_) build(state);
    if (model_.revision() != drawn_) { drawn_ = model_.revision(); if (model_.visible()) redraw(state); }
    if (model_.visible() && godot::Input::get_singleton()->is_action_just_pressed("ui_cancel")) model_.close();
    root_->set_visible(model_.visible() && !state.tactical && !state.loading);
}

void service_story_summary(GalacticMode::State& state) {
    if (!state.story_summary) state.story_summary = std::make_unique<StorySummaryWidget>();
    state.story_summary->service(state);
}
} // namespace eawr::presentation::godot_backend

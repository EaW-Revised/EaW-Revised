#include "story_summary_widget.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/battle_overlay.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <algorithm>
#include <cstdint>

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
// The summary frame (#6065, presentation::ui::scroll_panel_layout): 640 wide, 24 margins, 16 between parts, and
// never closer than 16 px to a screen edge. The chapter text scrolls inside the frame; the OK button stays in it.
constexpr double frame_width = 640.0, frame_margin = 24.0, frame_separation = 16.0, frame_screen_margin = 16.0;

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
    panel_->set_anchors_and_offsets_preset(godot::Control::PRESET_TOP_LEFT);
    godot::Ref<godot::StyleBoxFlat> style; style.instantiate();
    style->set_bg_color(godot::Color(0.02F, 0.04F, 0.08F, 0.93F));
    style->set_border_color(godot::Color(0.35F, 0.6F, 0.9F, 1.0F));
    style->set_border_width_all(2); style->set_content_margin_all(static_cast<float>(frame_margin));
    panel_->add_theme_stylebox_override("panel", style);
    auto* column = memnew(godot::VBoxContainer);
    column->add_theme_constant_override("separation", static_cast<int>(frame_separation));
    title_ = memnew(godot::Label); title_->add_theme_font_size_override("font_size", 28);
    title_->set_autowrap_mode(godot::TextServer::AUTOWRAP_WORD_SMART);
    body_ = memnew(godot::Label); body_->add_theme_font_size_override("font_size", 20);
    body_->set_autowrap_mode(godot::TextServer::AUTOWRAP_WORD_SMART);
    body_->set_h_size_flags(godot::Control::SIZE_EXPAND_FILL);
    scroll_ = memnew(godot::ScrollContainer);
    scroll_->set_horizontal_scroll_mode(godot::ScrollContainer::SCROLL_MODE_DISABLED);
    scroll_->set_v_size_flags(godot::Control::SIZE_EXPAND_FILL);
    scroll_->add_child(body_);
    relay_ = memnew(EawrOverlayButton);
    relay_->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE);
    relay_->set_action([this] { model_.close(); });
    close_ = memnew(godot::Button);
    close_->set_text("OK"); close_->set_focus_mode(godot::Control::FOCUS_NONE);
    close_->connect("pressed", callable_mp(relay_, &EawrOverlayButton::activate));
    relay_->add_child(close_);
    // The overlay button is a bare control: relayout() gives it the button's size (measured once the button is in the
    // tree and has its theme), or the footer would have no height.
    relay_->set_h_size_flags(godot::Control::SIZE_SHRINK_END);
    column->add_child(title_); column->add_child(scroll_); column->add_child(relay_);
    panel_->add_child(column); root_->add_child(panel_); state.hud->add_child(root_);
}

// Fits the frame to the screen: labels take the frame's inner width, the body's window gets the height the title and
// the button leave, and the frame is centred. Runs every visible frame; sets only what changed, so a resize or a
// late-measured autowrap height settles without churn.
void StorySummaryWidget::relayout() {
    if (!root_->is_inside_tree()) return;
    const auto view = root_->get_viewport()->get_visible_rect().size;
    const presentation::ui::Viewport viewport{static_cast<std::uint32_t>(std::max(view.x, 0.0F)),
                                              static_cast<std::uint32_t>(std::max(view.y, 0.0F))};
    const double width = std::max(std::min(frame_width, static_cast<double>(view.x) - 2.0 * frame_screen_margin), 0.0);
    const float inner = static_cast<float>(std::max(width - 2.0 * frame_margin, 0.0));
    title_->set_custom_minimum_size({inner, 0});
    body_->set_custom_minimum_size({inner, 0});
    presentation::ui::ScrollPanelSpec spec;
    spec.width = frame_width; spec.margin = frame_margin; spec.separation = frame_separation;
    spec.screen_margin = frame_screen_margin;
    spec.header_height = title_->get_combined_minimum_size().y;
    spec.body_height = body_->get_combined_minimum_size().y;
    // Measured here, with the button in the tree: at build time it has no theme yet and reports a smaller (or no) height.
    const auto button = close_->get_combined_minimum_size();
    if (relay_->get_custom_minimum_size() != button) relay_->set_custom_minimum_size(button);
    spec.footer_height = button.y;
    const auto layout = presentation::ui::scroll_panel_layout(viewport, spec);
    const godot::Vector2 window{static_cast<float>(layout.body.width), static_cast<float>(layout.body.height)};
    if (scroll_->get_custom_minimum_size() != window) scroll_->set_custom_minimum_size(window);
    const godot::Vector2 position{static_cast<float>(layout.panel.x), static_cast<float>(layout.panel.y)};
    const godot::Vector2 size{static_cast<float>(layout.panel.width), static_cast<float>(layout.panel.height)};
    if (panel_->get_position() != position) panel_->set_position(position);
    if (panel_->get_size() != size) panel_->set_size(size);
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
    scroll_->set_v_scroll(0); // a new chapter opens at its first line
}

void StorySummaryWidget::open(GalacticMode::State& state) {
    if (!has_goals()) return;
    const auto tag = model_.tag();
    model_.request(StorySummary::Request::open, tag, model_.chapter());
    service(state);
}

void StorySummaryWidget::service(GalacticMode::State& state) {
    if (!state.hud || !state.galaxy || !state.game || !state.filesystem) return;
    feed(state);
    if (!root_) build(state);
    if (model_.revision() != drawn_) { drawn_ = model_.revision(); if (model_.visible()) redraw(state); }
    if (model_.visible() && godot::Input::get_singleton()->is_action_just_pressed("ui_cancel")) model_.close();
    const bool shown = model_.visible() && !state.tactical && !state.loading;
    root_->set_visible(shown);
    if (shown) relayout();
}

void service_story_summary(GalacticMode::State& state) {
    if (!state.story_summary) state.story_summary = std::make_unique<StorySummaryWidget>();
    state.story_summary->service(state);
}
} // namespace eawr::presentation::godot_backend

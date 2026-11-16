#include "story_text_widget.hpp"
#include "galactic_mode_internal.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

namespace eawr::presentation::godot_backend {
namespace story = sim::galactic::story;

namespace {
constexpr float column_width = 420.0F;
constexpr float screen_margin = 24.0F;
constexpr int font_size = 18;

void add_label(godot::VBoxContainer& column, const godot::String& text, const godot::Color& colour) {
    auto* label = memnew(godot::Label);
    label->set_text(text);
    label->set_custom_minimum_size({column_width, 0});
    label->set_autowrap_mode(godot::TextServer::AUTOWRAP_WORD_SMART);
    label->add_theme_font_size_override("font_size", font_size);
    label->add_theme_color_override("font_color", colour);
    label->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE);
    column.add_child(label);
}
void clear(godot::VBoxContainer& column) {
    while (column.get_child_count() > 0) {
        auto* child = column.get_child(0);
        column.remove_child(child);
        child->queue_free();
    }
}
} // namespace

void StoryTextWidget::build(GalacticMode::State& state) {
    root_ = memnew(godot::Control);
    root_->set_name("galactic_story_text"); root_->set_z_index(8);
    root_->set_anchors_and_offsets_preset(godot::Control::PRESET_FULL_RECT);
    root_->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE);
    objectives_ = memnew(godot::VBoxContainer);
    objectives_->set_anchors_and_offsets_preset(godot::Control::PRESET_TOP_RIGHT);
    objectives_->set_offset(godot::SIDE_LEFT, -(column_width + screen_margin));
    objectives_->set_offset(godot::SIDE_RIGHT, -screen_margin);
    objectives_->set_offset(godot::SIDE_TOP, 4 * screen_margin);
    lines_ = memnew(godot::VBoxContainer);
    lines_->set_anchors_and_offsets_preset(godot::Control::PRESET_CENTER_TOP);
    lines_->set_offset(godot::SIDE_LEFT, -column_width / 2.0F);
    lines_->set_offset(godot::SIDE_RIGHT, column_width / 2.0F);
    lines_->set_offset(godot::SIDE_TOP, 6 * screen_margin);
    root_->add_child(objectives_); root_->add_child(lines_);
    state.hud->add_child(root_);
}

void StoryTextWidget::redraw(GalacticMode::State& state) {
    if (!text_ && !text_tried_) {
        text_tried_ = true;
        auto database = data::ui::load_language_text_database(*state.filesystem, "english");
        if (database) text_ = std::make_shared<data::ui::TextDatabase>(std::move(database).value());
    }
    const auto resolve = [&](const std::string& key) {
        const auto* value = text_ ? text_->find(key) : nullptr;
        return value ? data::ui::to_utf8(value->value) : key; // a missing id shows as itself
    };
    // The signature of what would be drawn: the columns are rebuilt only when it changes.
    std::string signature;
    if (model_.objectives_visible()) for (const auto& row : model_.objectives())
        signature += "O" + std::to_string(row.number) + ":" + std::to_string(static_cast<int>(row.status)) + ":" + row.text_id + ";";
    for (const auto& line : model_.lines()) signature += "L" + line.text_id + ";";
    if (signature == drawn_) return;
    drawn_ = std::move(signature);
    clear(*objectives_); clear(*lines_);
    if (model_.objectives_visible()) for (const auto& row : model_.objectives()) {
        using Status = story::ObjectiveStatus;
        const auto colour = row.status == Status::complete ? godot::Color(0.4F, 0.9F, 0.4F)
            : row.status == Status::failed ? godot::Color(0.9F, 0.35F, 0.35F) : godot::Color(1, 1, 1);
        add_label(*objectives_, godot::String::utf8((std::to_string(row.number) + ". " + resolve(row.text_id)).c_str()), colour);
    }
    for (const auto& line : model_.lines()) add_label(*lines_, godot::String::utf8(resolve(line.text_id).c_str()), godot::Color(1, 1, 1));
}

void StoryTextWidget::reset() {
    model_ = {};
    cursor_ = 0; ticks_ = 0;
    drawn_.clear();
    if (objectives_) clear(*objectives_);
    if (lines_) clear(*lines_);
}

void StoryTextWidget::service(GalacticMode::State& state) {
    if (!state.game || !state.galaxy || !state.hud) return;
    if (!root_) build(state);
    const auto published = state.game->story_presentation_after(cursor_, sim::SeatId{state.galaxy->local.seat});
    if (published.incomplete) {
        // The feed dropped events this reader had not seen: rebuild the objective list from the published state and skip
        // the events (the state already includes them).
        model_.resync(published.objectives.value_or(story::GoalsState{}));
    } else {
        for (const auto& row : published.events) {
            cursor_ = row.cursor;
            static_cast<void>(model_.apply(row.event));
        }
    }
    cursor_ = std::max(cursor_, published.through);
    const auto now = godot::Time::get_singleton()->get_ticks_msec();
    model_.advance(ticks_ ? static_cast<double>(now - ticks_) / 1000.0 : 0.0);
    ticks_ = now;
    redraw(state);
}

void service_story_text(GalacticMode::State& state) {
    if (!state.story_text) state.story_text = std::make_unique<StoryTextWidget>();
    state.story_text->service(state);
}
} // namespace eawr::presentation::godot_backend

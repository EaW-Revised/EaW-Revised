#include "story_text_widget.hpp"
#include "galactic_mode_internal.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/text_paragraph.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

namespace eawr::presentation::godot_backend {
namespace story = sim::galactic::story;

namespace {
constexpr float column_width = 420.0F;
constexpr float screen_margin = 24.0F;
constexpr int font_size = 18;

godot::Label* add_label(godot::VBoxContainer& column, const godot::String& text, const godot::Color& colour) {
    auto* label = memnew(godot::Label);
    label->set_text(text);
    label->set_custom_minimum_size({column_width, 0});
    label->set_autowrap_mode(godot::TextServer::AUTOWRAP_WORD_SMART);
    label->add_theme_font_size_override("font_size", font_size);
    label->add_theme_color_override("font_color", colour);
    label->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE);
    column.add_child(label);
    return label;
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
    if (const auto constants = data::load_document(*state.filesystem, "data/xml/gameconstants.xml"))
        reveal_rate_ = viewer::galactic::text_reveal_rate(constants.value().root);
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
    for (const auto& line : model_.lines()) signature += "L" + std::to_string(line.identity) + ";";
    const float wall_now = static_cast<float>(godot::Time::get_singleton()->get_ticks_msec()) / 1000.0F;
    if (signature != drawn_) {
        drawn_ = std::move(signature);
        clear(*objectives_); clear(*lines_);
        if (model_.objectives_visible()) for (const auto& row : model_.objectives()) {
            using Status = story::ObjectiveStatus;
            const auto colour = row.status == Status::complete ? godot::Color(0.4F, 0.9F, 0.4F)
                : row.status == Status::failed ? godot::Color(0.9F, 0.35F, 0.35F) : godot::Color(1, 1, 1);
            add_label(*objectives_, godot::String::utf8((std::to_string(row.number) + ". " + resolve(row.text_id)).c_str()), colour);
        }
        auto previous = std::move(revealed_);
        revealed_.clear();
        for (const auto& line : model_.lines()) {
            RevealedText shown;
            shown.identity = line.identity;
            const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& row) { return row.identity == line.identity; });
            if (old != previous.end()) {
                shown.reveal = std::move(old->reveal);
                shown.text = std::move(old->text);
            } else {
                const auto text = godot::String::utf8(resolve(line.text_id).c_str());
                godot::Ref<godot::TextParagraph> paragraph;
                paragraph.instantiate();
                // The current story column remains placeholder chrome; wrap the full text
                // before revealing so the layout never changes as the prefix grows.
                auto* probe = add_label(*lines_, {}, godot::Color(1, 1, 1));
                paragraph->set_width(column_width);
                paragraph->set_break_flags(godot::TextServer::BREAK_MANDATORY | godot::TextServer::BREAK_WORD_BOUND
                    | godot::TextServer::BREAK_ADAPTIVE);
                paragraph->add_string(text, probe->get_theme_font("font"), font_size);
                lines_->remove_child(probe); probe->queue_free();
                std::vector<std::size_t> lengths;
                for (int i = 0; i < paragraph->get_line_count(); ++i) {
                    const auto range = paragraph->get_line_range(i);
                    auto wrapped = text.substr(range.x, range.y - range.x);
                    // The explicit row already supplies the line break.
                    while (wrapped.ends_with("\n") || wrapped.ends_with("\r")) wrapped = wrapped.substr(0, wrapped.length() - 1);
                    lengths.push_back(viewer::galactic::wide_length({wrapped.ptr(), static_cast<std::size_t>(wrapped.length())}));
                    shown.text.push_back(std::move(wrapped));
                }
                shown.reveal.begin(lengths, line.teletype && reveal_rate_.has_value(), wall_now);
            }
            for (const auto& text : shown.text) {
                auto* label = add_label(*lines_, text, godot::Color(1, 1, 1));
                label->set_autowrap_mode(godot::TextServer::AUTOWRAP_OFF);
                shown.labels.push_back(label);
            }
            revealed_.push_back(std::move(shown));
        }
    }
    for (auto& shown : revealed_) {
        if (reveal_rate_) shown.reveal.advance(wall_now, *reveal_rate_);
        for (std::size_t i = 0; i < shown.labels.size(); ++i) {
            const auto& text = shown.text[i];
            shown.labels[i]->set_visible_characters(static_cast<int>(viewer::galactic::wide_prefix(
                {text.ptr(), static_cast<std::size_t>(text.length())}, shown.reveal.visible(i))));
        }
    }
}

void StoryTextWidget::reset() {
    model_ = {};
    cursor_ = 0; ticks_ = 0;
    drawn_.clear();
    revealed_.clear();
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

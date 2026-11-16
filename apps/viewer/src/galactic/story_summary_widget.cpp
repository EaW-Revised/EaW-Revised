#include "story_summary_widget.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/battle_overlay.hpp"
#include "ui/input_routing.hpp"
#include "ui/kit.hpp"
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <sstream>

namespace eawr::presentation::godot_backend {
namespace story = sim::galactic::story;
using presentation::ui::StoryGoalRow;
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
StoryGoalRow row_of(const story::Goal& goal) {
    StoryGoalRow row;
    row.tag = goal.tag; row.dialog = goal.dialog; row.chapter = goal.chapter;
    row.completed = goal.completed; row.hidden = goal.hidden; row.remove_after_viewed = goal.remove_after_viewed;
    return row;
}

// The room a row has inside a list: its width less the panel's margins and the scroll bar.
float row_room(godot::ItemList& list) {
    float width = list.get_size().x;
    if (const auto box = list.get_theme_stylebox("panel"); box.is_valid()) width -= box->get_minimum_size().x;
    if (auto* bar = list.get_v_scroll_bar()) width -= bar->get_combined_minimum_size().x;
    return std::max(32.0F, width - 4.0F);
}
// Greedy word wrap of one authored line to the list's width. Leading spaces are kept on the first row (the authored
// victory conditions are indented that way); `\n` (a real one or the two characters) starts another row.
std::vector<std::string> wrap_row(const godot::ItemList& list, const std::string& text, const float room) {
    std::vector<std::string> rows;
    const auto font = list.get_theme_font("font");
    const auto pixels = list.get_theme_font_size("font_size");
    const auto width = [&](const std::string& value) {
        return font.is_valid() ? font->get_string_size(godot::String::utf8(value.c_str()),
            godot::HORIZONTAL_ALIGNMENT_LEFT, -1, pixels).x : static_cast<float>(value.size() * 8);
    };
    std::vector<std::string> paragraphs(1);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') continue;
        if (text[i] == '\n') { paragraphs.emplace_back(); continue; }
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { paragraphs.emplace_back(); ++i; continue; }
        paragraphs.back() += text[i];
    }
    for (const auto& paragraph : paragraphs) {
        std::size_t indent = 0;
        while (indent < paragraph.size() && paragraph[indent] == ' ') ++indent;
        std::string line(indent, ' '), word;
        bool first = true;
        const auto push_word = [&] {
            if (word.empty()) return;
            const auto candidate = first ? line + word : line + ' ' + word;
            if (!first && width(candidate) > room) { rows.push_back(line); line = word; }
            else line = candidate;
            first = false; word.clear();
        };
        for (std::size_t i = indent; i < paragraph.size(); ++i) {
            if (paragraph[i] == ' ') push_word(); else word += paragraph[i];
        }
        push_word();
        rows.push_back(line);
    }
    return rows;
}
void caption(godot::Control* control, const std::string& value) {
    if (!control) return;
    const auto text = godot::String::utf8(value.c_str());
    if (auto* label = godot::Object::cast_to<EawrUiLabel>(control)) label->set_text(text);
    if (auto* button = godot::Object::cast_to<EawrUiButton>(control)) button->set_text(text);
    control->set_meta("story_caption", text);
}
std::string text_of(const godot::String& value) { return std::string(value.utf8().get_data()); }

// Dialog files live in Data/Scripts/Story/<name>.txt (WST-58).
std::string read_dialog(const vfs::Vfs& files, const std::string& name) {
    auto bytes = files.open("data/scripts/story/" + name + ".txt");
    if (!bytes) return {};
    return std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
}
} // namespace

void StorySummaryWidget::reset() {
    model_ = StorySummary{};
    cursor_ = 0; drawn_ = 0; dirty_ = true;
    recap_ = {}; list_tags_.clear();
    if (root_) root_->hide();
}

std::string StorySummaryWidget::resolve(GalacticMode::State& state, const std::string& key) {
    if (!text_ && !text_tried_) {
        text_tried_ = true;
        auto database = data::ui::load_language_text_database(*state.filesystem, "english");
        if (database) text_ = std::make_shared<data::ui::TextDatabase>(std::move(database).value());
    }
    const auto* value = text_ ? text_->find(key) : nullptr;
    return value ? data::ui::to_utf8(value->value) : key;
}

const std::string& StorySummaryWidget::dialog_text(GalacticMode::State& state, const std::string& name) {
    const auto at = dialog_texts_.find(name);
    if (at != dialog_texts_.end()) return at->second;
    return dialog_texts_.emplace(name, read_dialog(*state.filesystem, name)).first->second;
}

std::optional<presentation::ui::StoryChapterText> StorySummaryWidget::chapter_of(GalacticMode::State& state, const StoryGoalRow& goal) {
    return presentation::ui::story_dialog_chapter(dialog_text(state, goal.dialog), goal.chapter);
}

void StorySummaryWidget::feed(GalacticMode::State& state) {
    const auto published = state.game->story_presentation_after(cursor_, sim::SeatId{state.galaxy->local.seat});
    if (published.incomplete) {
        // The reader fell off the bounded feed: rebuild the goal list from the owner's published state.
        model_.clear_goals();
        if (published.objectives)
            for (auto goal = published.objectives->goals.rbegin(); goal != published.objectives->goals.rend(); ++goal)
                model_.set_goal(row_of(*goal)); // oldest first: each one goes to the head
    } else {
        for (const auto& row : published.events) {
            if (const auto* goal = std::get_if<story::GoalChanged>(&row.event.payload)) {
                if (goal->value) model_.set_goal(row_of(*goal->value)); else model_.erase_goal(goal->tag);
            } else if (const auto* summary = std::get_if<story::SummaryRequest>(&row.event.payload)) {
                model_.request(request_of(summary->action), summary->tag, summary->chapter);
            }
        }
    }
    cursor_ = published.through; // the cursor includes filtered notifications for the other owners
}

void StorySummaryWidget::close() {
    model_.close(); // WST-68: goals read and marked remove-after-viewed go with it
}

void StorySummaryWidget::choose(const std::int64_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= list_tags_.size()) return;
    model_.select(list_tags_[static_cast<std::size_t>(index)]); // refreshed on the next service, outside the signal
}

// Builds the authored dialog afresh: on first use and whenever the screen size changes (the dialog's scale follows it).
void StorySummaryWidget::rebuild(GalacticMode::State& state) {
    for (auto* relay : relays_) relay->queue_free();
    relays_.clear();
    if (dialog_ && dialog_->frame) { dialog_->frame->hide(); dialog_->frame->queue_free(); }
    dialog_.reset();
    built_size_ = root_->get_size();
    if (built_size_.x < 1 || built_size_.y < 1) return; // not laid out yet: retried next frame, nothing built
    // A build that fails (no builder, or the catalog has no such dialog) is tried once per size, with one diagnostic: the
    // builder constructs the whole theme before it looks the dialog up, which must not run every frame.
    const auto fail = [&](const char* why) {
        failed_size_ = built_size_; ++build_failures_;
        if (build_failures_ == 1) godot::UtilityFunctions::push_warning(godot::String("[eawr-story] the story summary dialog was not built: ") + why);
    };
    if (state.hud_fonts) {
        const auto& families = state.hud_fonts->system_families();
        system_family_count_ = families.size(); arial_families_.clear();
        for (const auto& family : families) {
            std::string lower = family;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower.find("arial") != std::string::npos) arial_families_.push_back(family);
        }
    }
    if (!state.battle_dialog_builder) { fail("no dialog builder"); return; }
    auto built = state.battle_dialog_builder("IDD_STORY_SUMMARY_DIALOG", built_size_);
    if (!built.frame) { fail("IDD_STORY_SUMMARY_DIALOG is not in the dialog catalog"); return; }
    failed_size_ = godot::Vector2();
    dialog_ = std::move(built);
    root_->add_child(dialog_->frame);
    ++rebuilds_;
    // The RC declares its group box after the tabs and lists it frames, but a group box is a background: paint it first.
    for (const auto& gadget : dialog_->gadgets)
        if (gadget.kind == "group" && gadget.control) dialog_->frame->move_child(gadget.control, 0);
    const auto relay = [&](godot::Object* source, const char* signal, std::function<void()> action) {
        auto* node = memnew(EawrOverlayButton); root_->add_child(node); node->hide();
        node->set_action(std::move(action)); relays_.push_back(node);
        source->connect(signal, callable_mp(node, &EawrOverlayButton::activate));
    };
    for (auto& gadget : dialog_->gadgets) {
        if (!gadget.control) continue;
        if (gadget.id == "IDC_SUMMARY_DIALOG_CLOSE_BUTTON") relay(gadget.control, "pressed", [this] { close(); });
        else if (gadget.id == "IDC_RADIO_ACTIVE") relay(gadget.control, "pressed", [this] { model_.choose_tab(StorySummary::Tab::active); dirty_ = true; }); // a press on the shown tab must not leave it unpressed
        else if (gadget.id == "IDC_RADIO_COMPLETED") relay(gadget.control, "pressed", [this] { model_.choose_tab(StorySummary::Tab::completed); dirty_ = true; });
        else if (gadget.id == "IDC_GOAL_LIST") {
            gadget.control->connect("item_selected", callable_mp_static(+[](int64_t index, int64_t pointer) {
                reinterpret_cast<StorySummaryWidget*>(pointer)->choose(index);
            }).bind(reinterpret_cast<int64_t>(this)));
        }
    }
    dirty_ = true;
}

// Fills the dialog from the model: the tabs, the goal list, the chapter's header and recap. Runs when the model changed or
// the dialog was rebuilt; lists are refilled whole (a handful of rows).
void StorySummaryWidget::refresh(GalacticMode::State& state) {
    if (!dialog_ || !dialog_->frame) return;
    const auto* selected = model_.selected();
    recap_ = {};
    if (selected) {
        if (const auto chapter = chapter_of(state, *selected)) {
            if (!chapter->title.empty()) recap_.header = resolve(state, chapter->title);
            for (const auto& line : chapter->lines)
                recap_.lines.emplace_back(line.key.empty() ? std::string{} : resolve(state, line.key), line.colour);
        }
    }
    list_tags_.clear();
    for (auto& gadget : dialog_->gadgets) {
        auto* control = gadget.control;
        if (!control) continue;
        const auto& id = gadget.id;
        if (id == "IDC_RADIO_ACTIVE") {
            if (auto* button = godot::Object::cast_to<godot::BaseButton>(control))
                button->set_pressed_no_signal(model_.tab() == StorySummary::Tab::active);
        } else if (id == "IDC_RADIO_COMPLETED") {
            if (auto* button = godot::Object::cast_to<godot::BaseButton>(control))
                button->set_pressed_no_signal(model_.tab() == StorySummary::Tab::completed);
        } else if (id == "IDC_SUMMARY_DIALOG_ABANDON") {
            control->hide(); // WST-58: no goal can be abandoned, so the control never applies
        } else if (gadget.kind == "label" && id == "IDC_STATIC") {
            caption(control, recap_.header); // the incoming-transmission header names the speaker of the shown chapter
        } else if (id == "IDC_GOAL_LIST") {
            auto* list = godot::Object::cast_to<godot::ItemList>(control);
            if (!list) continue;
            list->clear();
            int chosen = -1;
            for (const auto* row : model_.rows()) {
                // The goal's name is its chapter's first text line; a goal whose file or chapter cannot be read shows its tag.
                std::string name = row->tag;
                if (const auto chapter = chapter_of(state, *row)) {
                    if (const auto key = chapter->first_text_key(); !key.empty()) name = resolve(state, key);
                }
                const int index = list->add_item(godot::String::utf8(name.c_str()));
                list_tags_.push_back(row->tag);
                if (selected && row->tag == selected->tag) chosen = index;
            }
            if (chosen >= 0) { list->select(chosen); list->ensure_current_is_visible(); }
            else list->deselect_all();
        } else if (id == "IDC_STORY_RECAP") {
            auto* list = godot::Object::cast_to<godot::ItemList>(control);
            if (!list) continue;
            list->clear();
            const float room = row_room(*list);
            for (const auto& [text, colour] : recap_.lines) {
                for (const auto& row : wrap_row(*list, text, room)) {
                    // A blank row is one space: the list gives an empty item no height, and the blank row would vanish.
                    const int index = list->add_item(godot::String::utf8(row.empty() ? " " : row.c_str()));
                    list->set_item_selectable(index, false);
                    if (colour) list->set_item_custom_fg_color(index, godot::Color(
                        static_cast<float>((*colour)[0]) / 255.0F, static_cast<float>((*colour)[1]) / 255.0F,
                        static_cast<float>((*colour)[2]) / 255.0F, static_cast<float>((*colour)[3]) / 255.0F));
                }
            }
            if (auto* bar = list->get_v_scroll_bar()) bar->set_value(0); // a new chapter opens at its first line
        }
    }
}

void StorySummaryWidget::open(GalacticMode::State& state) {
    if (!has_goals()) return;
    // The button reopens the window at the goal last shown, else at the first goal of the shown tab (newest).
    const auto* goal = model_.selected();
    if (!goal) { const auto rows = model_.rows(); goal = rows.empty() ? nullptr : rows.front(); }
    if (!goal) { // only completed goals left: show them
        model_.choose_tab(StorySummary::Tab::completed);
        goal = model_.selected();
    }
    model_.request(StorySummary::Request::open, goal ? goal->tag : model_.tag(), goal ? goal->chapter : model_.chapter());
    service(state);
}

void StorySummaryWidget::service(GalacticMode::State& state) {
    if (!state.hud || !state.galaxy || !state.game || !state.filesystem) return;
    feed(state);
    if (state.options.dismiss_story) close(); // capture only: keep the intro summary off the frame
    if (!root_) {
        // The modal layer stops every pointer event, the wheel included, and keeps keys from the map while it is up (#6758).
        root_ = memnew(EawrUiModalLayer);
        root_->set_name("galactic_story_summary"); root_->set_z_index(20);
        root_->set_anchors_and_offsets_preset(godot::Control::PRESET_FULL_RECT); root_->hide();
        state.hud->add_child(root_);
    }
    if (model_.visible() && godot::Input::get_singleton()->is_action_just_pressed("ui_cancel")) close();
    // WST-96: refresh keeps the same token; closing releases only this dialog's
    // token, including when a player or another modal already paused the galaxy.
    if (const auto pause = model_.galactic_pause_change(static_cast<bool>(state.options.lan_match))) {
        const auto seat = sim::SeatId{state.galaxy->local.seat};
        if (*pause) state.game->submit(platform::LiveGameOrder{seat, sim::galactic::PauseGalaxy{}, {}});
        else state.game->submit(platform::LiveGameOrder{seat, sim::galactic::ResumeGalaxy{}, {}});
    }
    const bool shown = model_.visible() && !state.tactical && !state.loading;
    if (root_->is_visible() != shown) root_->set_visible(shown);
    // A modal window: while it is up the map and bar beneath take no mouse input (the layer is hidden, so inert, otherwise).
    if (!shown) return;
    if ((!dialog_ || built_size_ != root_->get_size()) && failed_size_ != root_->get_size()) rebuild(state);
    if (dialog_ && (model_.revision() != drawn_ || dirty_)) { drawn_ = model_.revision(); dirty_ = false; refresh(state); }
}

std::string StorySummaryWidget::report_json() const {
    using galactic_detail::json;
    std::ostringstream out;
    out << "{\"visible\":" << (model_.visible() ? "true" : "false")
        << ",\"mounted\":" << (dialog_ ? "true" : "false") << ",\"rebuilds\":" << rebuilds_ << ",\"build_failures\":" << build_failures_ << ",\"system_families\":" << system_family_count_ << ",\"arial_families\":[" << [&] { std::string joined; for (const auto& f : arial_families_) joined += (joined.empty() ? "" : ",") + json(f); return joined; }() << "]"
        << ",\"flashing\":" << (model_.button_flashing() ? "true" : "false")
        << ",\"tab\":" << json(model_.tab() == StorySummary::Tab::active ? "active" : "completed")
        << ",\"selected\":" << json(model_.tag()) << ",\"chapter\":" << model_.chapter()
        << ",\"goals\":[";
    std::size_t count{};
    for (const auto& goal : model_.goals())
        out << (count++ ? "," : "") << "{\"tag\":" << json(goal.tag) << ",\"dialog\":" << json(goal.dialog)
            << ",\"chapter\":" << goal.chapter << ",\"completed\":" << (goal.completed ? "true" : "false")
            << ",\"remove_after_viewed\":" << (goal.remove_after_viewed ? "true" : "false")
            << ",\"viewed\":" << (model_.viewed(goal.tag) ? "true" : "false") << '}';
    out << "],\"header\":" << json(recap_.header) << ",\"controls\":[";
    count = 0;
    if (dialog_) for (const auto& gadget : dialog_->gadgets) if (gadget.control) {
        const auto rect = gadget.control->get_global_rect();
        const auto* button = godot::Object::cast_to<godot::BaseButton>(gadget.control);
        const godot::String text = gadget.control->get_meta("story_caption", godot::String());
        out << (count++ ? "," : "") << "{\"name\":" << json(gadget.id)
            << ",\"text\":" << json(text_of(text))
            << ",\"visible\":" << (gadget.control->is_visible_in_tree() ? "true" : "false")
            << ",\"pressed\":" << (button && button->is_pressed() ? "true" : "false")
            << ",\"rect\":[" << rect.position.x << ',' << rect.position.y << ',' << rect.size.x << ',' << rect.size.y << ']';
        if (auto* list = godot::Object::cast_to<godot::ItemList>(gadget.control)) {
            const auto font = list->get_theme_font("font");
            out << ",\"font\":" << json(font.is_valid() ? text_of(font->get_font_name()) : std::string{})
                << ",\"font_size\":" << list->get_theme_font_size("font_size");
            if (auto* bar = list->get_v_scroll_bar()) // where the list is scrolled, and how far it can go
                out << ",\"scroll\":" << bar->get_value() << ",\"scroll_max\":" << std::max(0.0, bar->get_max() - bar->get_page());
            out << ",\"items\":[";
            for (int item = 0; item < list->get_item_count(); ++item) {
                const auto colour = list->get_item_custom_fg_color(item);
                out << (item ? "," : "") << "{\"text\":" << json(text_of(list->get_item_text(item)))
                    << ",\"selected\":" << (list->is_selected(item) ? "true" : "false")
                    << ",\"colour\":[" << static_cast<int>(colour.r * 255.0F + 0.5F) << ',' << static_cast<int>(colour.g * 255.0F + 0.5F)
                    << ',' << static_cast<int>(colour.b * 255.0F + 0.5F) << ',' << static_cast<int>(colour.a * 255.0F + 0.5F) << "]}";
            }
            out << ']';
        }
        out << '}';
    }
    out << "]}";
    return out.str();
}

void service_story_summary(GalacticMode::State& state) {
    if (!state.story_summary) state.story_summary = std::make_unique<StorySummaryWidget>();
    state.story_summary->service(state);
}
} // namespace eawr::presentation::godot_backend

#include "ui/load_game_view.hpp"

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <utility>
#include <algorithm>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

constexpr const char* check_id = "IDC_CHECK_RECORDINGS";
constexpr const char* list_id = "IDC_SAVE_GAME_LIST";
constexpr const char* load_id = "IDC_LOAD_GAME_BUTTON";
constexpr const char* delete_id = "IDC_DELETE_GAME_BUTTON";
constexpr const char* cancel_id = "IDC_LOAD_CANCEL_BUTTON";
constexpr int64_t delete_yes = 1;
// WGS-155/161: the date column starts at 68% of the row and the name column at 0%.
constexpr float date_column_start = 0.68F;
constexpr float date_column_width = 0.30F;
// The loading box is drawn for this many frames before the launcher starts the restore.
constexpr int loading_box_frames = 2;

[[nodiscard]] String godot_text(const std::string& value) {
    return String::utf8(value.c_str(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] const BuiltGadget* gadget_named(const BuiltDialog& built, const std::string& id) {
    for (const BuiltGadget& gadget : built.gadgets) {
        if (gadget.id == id) return &gadget;
    }
    return nullptr;
}

} // namespace

void EawrLoadGameScreen::_bind_methods() {
    ADD_SIGNAL(MethodInfo("replay_chosen", PropertyInfo(Variant::STRING, "path"), PropertyInfo(Variant::STRING, "map")));
    ADD_SIGNAL(MethodInfo("save_chosen", PropertyInfo(Variant::STRING, "directory"), PropertyInfo(Variant::STRING, "name")));
    ADD_SIGNAL(MethodInfo("closed"));
}

bool EawrLoadGameScreen::open(Node& parent, const LoadGameContext& context) {
    context_ = context;
    const MessageBoxContext& boxes = context.boxes;
    if (boxes.catalog == nullptr || boxes.theme_model == nullptr) {
        failure_ = "the screen has no dialog catalogue or theme";
        return false;
    }
    const data::ui::Dialog* dialog = boxes.catalog->script.find("IDD_LOAD_GAME_DIALOG");
    if (dialog == nullptr) {
        failure_ = "dialog IDD_LOAD_GAME_DIALOG is not in the catalogue";
        return false;
    }
    built_ = build_dialog(*dialog, *boxes.theme_model, boxes.text, model::Placement::centre);
    for (const char* id : {check_id, list_id, load_id, delete_id, cancel_id}) {
        if (gadget_named(built_, id) == nullptr || gadget_named(built_, id)->control == nullptr) {
            failure_ = std::string("IDD_LOAD_GAME_DIALOG has no gadget ") + id;
            return false;
        }
    }
    model_ = model::LoadGameModel(context.store, context.saves);
    // Without a launcher Load on a saved game keeps its stated reason.
    if (context.launcher) model_.set_saved_load_unavailable({});
    model_.refresh();

    gadget(check_id)->connect("toggled", callable_mp(this, &EawrLoadGameScreen::on_check_toggled));
    gadget(list_id)->connect("item_selected", callable_mp(this, &EawrLoadGameScreen::on_item_selected));
    gadget(list_id)->connect("item_activated", callable_mp(this, &EawrLoadGameScreen::on_item_activated));
    gadget(load_id)->connect("pressed", callable_mp(this, &EawrLoadGameScreen::on_load));
    gadget(delete_id)->connect("pressed", callable_mp(this, &EawrLoadGameScreen::on_delete));
    gadget(cancel_id)->connect("pressed", callable_mp(this, &EawrLoadGameScreen::on_cancel));

    set_process_input(true);
    set_process(true);
    set_layer(layer_index);
    set_name("IDD_LOAD_GAME_DIALOG");
    modal_ = memnew(EawrUiModalLayer);
    modal_->set_position(Vector2());
    modal_->set_size(boxes.viewport_size);
    modal_->set_theme(boxes.theme);
    add_child(modal_);
    modal_->add_child(built_.frame);
    parent.add_child(this);
    sync();
    // A directory that could not be listed is told to the player, as the in-game dialog does.
    if (!model_.saved_games_problem().empty()) static_cast<void>(show_notice(model_.saved_games_problem()));
    return true;
}

Control* EawrLoadGameScreen::gadget(const std::string& id) const {
    const BuiltGadget* found = gadget_named(built_, id);
    return found != nullptr ? found->control : nullptr;
}

bool EawrLoadGameScreen::box_open() const noexcept {
    return box_ != nullptr && UtilityFunctions::is_instance_id_valid(box_id_);
}

EawrMessageBox* EawrLoadGameScreen::box() const noexcept { return box_open() ? box_ : nullptr; }

std::string EawrLoadGameScreen::load_tooltip() const {
    return model_.can_load() ? std::string{} : model_.load_disabled_reason();
}

// A reserved slot shows its localized label; a key the text database lacks falls back to the stored
// description, which the entry already carries.
std::string EawrLoadGameScreen::resolved_row(const model::LoadGameEntry& entry) const {
    if (entry.label_key.empty() || context_.boxes.text == nullptr) return entry.text;
    const std::string label = data::ui::to_utf8(context_.boxes.text->text(entry.label_key));
    return label.empty() || label == entry.label_key ? entry.text : label;
}

std::string EawrLoadGameScreen::row_text(const std::size_t index) const {
    const auto& rows = model_.visible();
    if (index >= rows.size()) return {};
    return rows[index].kind == model::EntryKind::saved_game ? resolved_row(rows[index]) : rows[index].text;
}

std::string EawrLoadGameScreen::row_date(const std::size_t index) const {
    const auto& rows = model_.visible();
    return index < rows.size() ? rows[index].date_text : std::string{};
}

// The list, the buttons and the tooltips follow the model.
void EawrLoadGameScreen::sync() {
    auto* list = Object::cast_to<ItemList>(gadget(list_id));
    for (auto& [index, label] : dates_) {
        static_cast<void>(index);
        label->queue_free();
    }
    dates_.clear();
    list->clear();
    for (const model::LoadGameEntry& entry : model_.visible()) {
        const bool saved = entry.kind == model::EntryKind::saved_game;
        const int32_t row = list->add_item(godot_text(saved ? resolved_row(entry) : entry.text));
        if (!entry.playable) list->set_item_tooltip(row, godot_text(entry.problem));
        if (!saved || entry.date_text.empty()) continue;
        auto* date = memnew(EawrUiLabel);
        list->add_child(date);
        date->set_text(godot_text(entry.date_text));
        date->set_wrap(false);
        date->set_role("List_Box");
        date->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        date->set_theme_type_variation(list->get_theme_type_variation());
        dates_.emplace_back(row, date);
    }
    if (const auto selected = model_.selected()) list->select(static_cast<int32_t>(*selected));
    place_dates();
    auto* load = Object::cast_to<BaseButton>(gadget(load_id));
    load->set_disabled(!model_.can_load());
    // The reason a disabled Load gives: the retail dialog has no free line for it.
    load->set_tooltip_text(godot_text(load_tooltip()));
    Object::cast_to<BaseButton>(gadget(delete_id))->set_disabled(!model_.can_delete());
    Object::cast_to<BaseButton>(gadget(check_id))->set_pressed_no_signal(model_.show_recordings());
}

// The date labels follow their rows as the list scrolls.
void EawrLoadGameScreen::place_dates() {
    auto* list = Object::cast_to<ItemList>(gadget(list_id));
    if (list == nullptr || dates_.empty()) return;
    const float width = list->get_size().x;
    const auto scroll = list->get_v_scroll_bar() != nullptr ? static_cast<float>(list->get_v_scroll_bar()->get_value()) : 0.0F;
    for (auto& [index, label] : dates_) {
        const Rect2 rect = list->get_item_rect(index);
        const float y = rect.position.y - scroll;
        label->set_position(Vector2(width * date_column_start, y));
        label->set_size(Vector2(width * date_column_width, rect.size.y));
        label->set_visible(y >= 0.0F && y + rect.size.y <= list->get_size().y);
    }
}

void EawrLoadGameScreen::_process(const double delta) {
    static_cast<void>(delta);
    if (finished_) return;
    place_dates();
    if (!pending_load_) return;
    if (launch_frames_ > 0) {
        --launch_frames_;
        return;
    }
    finish_launch();
}

// The launcher answers once the Loading box has been on screen: accepted closes the screen with the
// request, refused shows the reason over the screen.
void EawrLoadGameScreen::finish_launch() {
    const model::SavedGameRequest request = *std::exchange(pending_load_, std::nullopt);
    const std::string refusal = context_.launcher ? context_.launcher(request) : std::string(model::no_restore_launcher_reason);
    if (box_ != nullptr && UtilityFunctions::is_instance_id_valid(box_id_)) box_->queue_free();
    box_ = nullptr;
    box_id_ = 0;
    if (!refusal.empty()) {
        static_cast<void>(show_notice(refusal));
        return;
    }
    finished_ = true;
    emit_signal("save_chosen", godot_text(model::utf8_from_path(request.directory)), godot_text(request.name));
    queue_free();
}

void EawrLoadGameScreen::on_check_toggled(const bool pressed) {
    model_.set_show_recordings(pressed);
    model_.refresh();
    sync();
}

void EawrLoadGameScreen::on_item_selected(const int64_t index) {
    model_.select(static_cast<std::size_t>(index));
    sync();
}

void EawrLoadGameScreen::on_item_activated(const int64_t index) {
    model_.select(static_cast<std::size_t>(index));
    sync();
    on_load();
}

void EawrLoadGameScreen::on_load() {
    if (finished_ || box_open() || pending_load_) return;
    if (const auto saved = model_.saved_request()) {
        model::MessageBoxRequest loading;
        loading.kind = model::BoxKind::small_message;
        loading.caption = "TEXT_SAVELOAD_LOADING_MSG";
        box_ = memnew(EawrMessageBox);
        if (!box_->open(*this, context_.boxes, loading)) {
            failure_ = box_->failure();
            memdelete(box_);
            box_ = nullptr;
            return;
        }
        box_id_ = static_cast<int64_t>(box_->get_instance_id());
        pending_load_ = *saved;
        launch_frames_ = loading_box_frames;
        ++loads_started_;
        return;
    }
    const auto request = model_.play_request();
    if (!request) return;
    finished_ = true;
    emit_signal("replay_chosen", godot_text(model::utf8_from_path(request->path)), godot_text(request->map));
    queue_free();
}

void EawrLoadGameScreen::on_delete() {
    if (finished_ || box_open() || !model_.can_delete()) return;
    box_ = memnew(EawrMessageBox);
    if (!box_->open(*this, context_.boxes, model_.delete_confirmation())) {
        failure_ = box_->failure();
        memdelete(box_);
        box_ = nullptr;
        return;
    }
    box_id_ = static_cast<int64_t>(box_->get_instance_id());
    // The answer applies to the file asked about, not to whatever is selected when it comes back.
    pending_delete_ = model_.selected_entry()->file_name;
    box_->connect("result_chosen", callable_mp(this, &EawrLoadGameScreen::on_delete_answer));
}

bool EawrLoadGameScreen::show_notice(const std::string& reason) {
    if (finished_ || box_open()) return false;
    model::MessageBoxRequest request;
    request.caption = "TEXT_BUTTON_LOAD_GAME";
    request.lines[0] = reason;
    request.buttons = {model::BoxButton{"TEXT_BUTTON_OK", 0, true}};
    box_ = memnew(EawrMessageBox);
    if (!box_->open(*this, context_.boxes, request)) {
        failure_ = box_->failure();
        memdelete(box_);
        box_ = nullptr;
        return false;
    }
    box_id_ = static_cast<int64_t>(box_->get_instance_id());
    pending_delete_.reset();
    return true;
}

void EawrLoadGameScreen::on_delete_answer(const int64_t result) {
    const std::optional<std::string> name = std::exchange(pending_delete_, std::nullopt);
    if (result != delete_yes || !name) return;
    const model::DeleteResult outcome = model_.confirm_delete(*name);
    if (outcome != model::DeleteResult::deleted) {
        UtilityFunctions::printerr(String("load game: the entry could not be deleted"));
    }
    sync();
}

void EawrLoadGameScreen::on_cancel() {
    if (finished_ || box_open()) return;
    finished_ = true;
    emit_signal("closed");
    queue_free();
}

void EawrLoadGameScreen::_input(const Ref<InputEvent>& event) {
    if (finished_ || box_open()) return;
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    if (key == nullptr || !key->is_pressed() || key->is_echo() || key->get_keycode() != KEY_ESCAPE) return;
    if (Viewport* viewport = get_viewport()) viewport->set_input_as_handled();
    on_cancel();
}

void EawrSaveReplayScreen::_bind_methods() {
    ADD_SIGNAL(MethodInfo("closed"));
    ADD_SIGNAL(MethodInfo("saved", PropertyInfo(Variant::STRING, "name")));
}

Control* EawrSaveReplayScreen::gadget(const std::string& id) const {
    const auto* found = gadget_named(built_, id);
    return found ? found->control : nullptr;
}

platform::Path personal_replay_store(const platform::Path& override_path) {
    if (!override_path.empty()) return override_path;
    platform::Path store;
    const auto args = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < args.size(); ++index) {
        if (args[index] == String("--eawr-replay-store")) {
            store = platform::path_from_utf8(std::string(args[index + 1].utf8().get_data()));
            break;
        }
    }
    if (store.empty()) store = platform::path_from_utf8(std::string(
        ProjectSettings::get_singleton()->globalize_path("user://replays").utf8().get_data()));
    return store;
}

bool EawrSaveReplayScreen::open(Node& parent, SaveReplayContext context) {
    context_ = std::move(context);
    context_.store = personal_replay_store(context_.store);
    const auto& boxes = context_.boxes;
    if (!boxes.catalog || !boxes.theme_model) { failure_ = "Save replay has no dialog catalogue or theme."; return false; }
    const auto* dialog = boxes.catalog->script.find("IDD_SAVE_REPLAY_DIALOG");
    if (!dialog) { failure_ = "IDD_SAVE_REPLAY_DIALOG is not in the catalogue."; return false; }
    theme_model_ = *boxes.theme_model;
    context_.boxes.theme_model = &theme_model_;
    built_ = build_dialog(*dialog, theme_model_, boxes.text, model::Placement::centre);
    auto* edit = Object::cast_to<LineEdit>(gadget("IDC_SAVE_REPLAY_NAME_EDIT"));
    auto* list = Object::cast_to<ItemList>(gadget("IDC_SAVE_REPLAY_LIST"));
    auto* button = Object::cast_to<BaseButton>(gadget("IDC_SAVE_REPLAY_BUTTON"));
    auto* back = Object::cast_to<BaseButton>(gadget("IDC_SAVE_CANCEL_BUTTON"));
    if (!edit || !list || !button || !back) {
        failure_ = "Save replay is missing its name, list or buttons.";
        memdelete(built_.frame); built_ = {}; return false;
    }
    entries_ = platform::scan_replay_store(context_.store);
    std::sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
        return platform::replay_name_key(a.name) < platform::replay_name_key(b.name);
    });
    for (const auto& entry : entries_) list->add_item(godot_text(entry.name));
    edit->connect("text_changed", callable_mp(this, &EawrSaveReplayScreen::changed));
    list->connect("item_selected", callable_mp(this, &EawrSaveReplayScreen::selected));
    button->connect("pressed", callable_mp(this, &EawrSaveReplayScreen::save));
    back->connect("pressed", callable_mp(this, &EawrSaveReplayScreen::cancel));
    set_layer(EawrLoadGameScreen::layer_index);
    set_name("IDD_SAVE_REPLAY_DIALOG");
    auto* modal = memnew(EawrUiModalLayer);
    modal->set_size(boxes.viewport_size);
    modal->set_theme(boxes.theme);
    add_child(modal);
    modal->add_child(built_.frame);
    parent.add_child(this);
    set_process_input(true);
    changed(edit->get_text());
    edit->grab_focus();
    return true;
}

bool EawrSaveReplayScreen::box_open() const noexcept {
    return box_ && UtilityFunctions::is_instance_id_valid(box_id_);
}
EawrMessageBox* EawrSaveReplayScreen::box() const noexcept { return box_open() ? box_ : nullptr; }

void EawrSaveReplayScreen::changed(const String& value) {
    auto problem = platform::replay_name_problem(std::string(value.utf8().get_data()));
    if (context_.recording.empty() && !context_.recorder)
        problem = context_.recording_problem.empty() ? "There is no recording to save." : context_.recording_problem;
    auto* button = Object::cast_to<BaseButton>(gadget("IDC_SAVE_REPLAY_BUTTON"));
    button->set_disabled(!problem.empty());
    button->set_tooltip_text(godot_text(problem));
}

void EawrSaveReplayScreen::selected(const int64_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= entries_.size() || box_open()) return;
    auto* edit = Object::cast_to<LineEdit>(gadget("IDC_SAVE_REPLAY_NAME_EDIT"));
    edit->set_text(godot_text(entries_[static_cast<std::size_t>(index)].name));
    changed(edit->get_text());
}

void EawrSaveReplayScreen::save() {
    if (finished_ || box_open()) return;
    const auto* button = Object::cast_to<BaseButton>(gadget("IDC_SAVE_REPLAY_BUTTON"));
    if (!button || button->is_disabled()) return;
    const auto name = Object::cast_to<LineEdit>(gadget("IDC_SAVE_REPLAY_NAME_EDIT"))->get_text().utf8();
    write(std::string(name.get_data(), static_cast<std::size_t>(name.length())), false);
}

void EawrSaveReplayScreen::write(const std::string& name, const bool overwrite) {
    std::string error;
    if (context_.recording.empty() && context_.recorder) {
        context_.recording = context_.recorder(error);
        if (context_.recording.empty()) { notice(error.empty() ? "There is no recording to save." : error); return; }
    }
    const auto outcome = platform::write_replay_store(context_.store, name, context_.recording, overwrite, error);
    if (outcome == platform::ReplayWriteResult::saved) {
        emit_signal("saved", godot_text(name));
        close();
    } else if (outcome == platform::ReplayWriteResult::exists) {
        model::MessageBoxRequest request;
        request.caption = "TEXT_SAVE_REPLAY_TITLE";
        request.lines[0] = "TEXT_SAVE_CONFIRM";
        request.lines[1] = name;
        request.literal_lines[1] = true;
        request.buttons = {{"TEXT_BUTTON_YES", 1, false}, {"TEXT_BUTTON_NO", 0, true}};
        box_ = memnew(EawrMessageBox);
        if (!box_->open(*this, context_.boxes, request)) {
            failure_ = box_->failure(); memdelete(box_); box_ = nullptr; return;
        }
        box_id_ = static_cast<int64_t>(box_->get_instance_id());
        pending_name_ = name;
        box_->connect("result_chosen", callable_mp(this, &EawrSaveReplayScreen::answer));
    } else notice(error);
}

void EawrSaveReplayScreen::answer(const int64_t result) {
    const std::string name = std::exchange(pending_name_, {});
    box_ = nullptr;
    if (result == 1 && !name.empty()) write(name, true);
}

void EawrSaveReplayScreen::notice(const std::string& reason) {
    model::MessageBoxRequest request;
    request.caption = "TEXT_SAVE_REPLAY_TITLE";
    request.lines[0] = reason;
    request.literal_lines[0] = true;
    request.buttons = {{"TEXT_BUTTON_OK", 0, true}};
    box_ = memnew(EawrMessageBox);
    if (!box_->open(*this, context_.boxes, request)) {
        failure_ = box_->failure(); memdelete(box_); box_ = nullptr; return;
    }
    box_id_ = static_cast<int64_t>(box_->get_instance_id());
}

void EawrSaveReplayScreen::close() { finished_ = true; emit_signal("closed"); queue_free(); }
void EawrSaveReplayScreen::cancel() { if (!finished_ && !box_open()) close(); }
void EawrSaveReplayScreen::_input(const Ref<InputEvent>& event) {
    // Consume keys before the battle-end Continue handler underneath sees them.
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    if (!key || !key->is_pressed() || key->is_echo() || box_open()) return;
    const auto code = key->get_keycode();
    if (code != KEY_ESCAPE && code != KEY_ENTER && code != KEY_KP_ENTER) return;
    if (get_viewport()) get_viewport()->set_input_as_handled();
    if (code == KEY_ESCAPE) cancel();
    else save();
}

void register_load_game_classes() { GDREGISTER_CLASS(EawrLoadGameScreen); GDREGISTER_CLASS(EawrSaveReplayScreen); }

} // namespace eawr::presentation::godot_backend

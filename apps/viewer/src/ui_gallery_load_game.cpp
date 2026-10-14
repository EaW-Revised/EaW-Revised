#include "ui_gallery_internal.hpp"

#include "saved_games.hpp"
#include "ui/battle_overlay.hpp"

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// `--eawr-load-game <script|saves|live>` (MNU-39, MNU-41): the Load Game screen on the gallery's skin.
// `script` drives the recordings through Godot's real input dispatch and checks each step; `saves`
// drives the saved games the same way (list, selection, Delete with its confirmation, and Load through
// the launcher stand-in named by --eawr-load-launcher); `live` leaves it open until Cancel or Play.
// Play hands the chosen recording to the host (UiGalleryMode::take_replay_request), which starts the
// replay mode with it.
namespace eawr::presentation::godot_backend {
namespace {

void send_motion(Viewport& viewport, const Vector2 local) {
    const Vector2 window = viewport.get_final_transform().xform(local);
    Ref<InputEventMouseMotion> event;
    event.instantiate();
    event->set_position(window);
    event->set_global_position(window);
    event->set_relative(Vector2(1.0F, 0.0F));
    Input::get_singleton()->parse_input_event(event);
}

void send_click(Viewport& viewport, const Vector2 local) {
    send_motion(viewport, local);
    const Vector2 window = viewport.get_final_transform().xform(local);
    for (const bool pressed : {true, false}) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_button_index(MOUSE_BUTTON_LEFT);
        event->set_pressed(pressed);
        event->set_factor(1.0F);
        event->set_position(window);
        event->set_global_position(window);
        Input::get_singleton()->parse_input_event(event);
    }
}

} // namespace

UiGalleryMode::State* UiGalleryMode::State::load_active = nullptr;

void UiGalleryMode::State::on_load_replay(const String& path, const String& map) {
    if (load_active == nullptr) return;
    load_active->load_play = std::make_pair(std::string(path.utf8().get_data()), std::string(map.utf8().get_data()));
}

void UiGalleryMode::State::on_load_save(const String& directory, const String& name) {
    if (load_active == nullptr) return;
    load_active->load_save = std::make_pair(std::string(directory.utf8().get_data()), std::string(name.utf8().get_data()));
    if (load_active->load_screen != nullptr && UtilityFunctions::is_instance_id_valid(load_active->load_id)) {
        load_active->load_loading_boxes = load_active->load_screen->loads_started();
    }
}

void UiGalleryMode::State::on_load_closed() {
    if (load_active != nullptr) load_active->load_closed = true;
}

bool UiGalleryMode::State::load_alive() {
    if (load_screen != nullptr && !UtilityFunctions::is_instance_id_valid(load_id)) load_screen = nullptr;
    return load_screen != nullptr;
}

void UiGalleryMode::State::build_load_game_page(Control& root) {
    if (options.load_game != "script" && options.load_game != "saves" && options.load_game != "live" && options.load_game != "save-replay") {
        failure = "--eawr-load-game expects script, saves, live or save-replay";
        return;
    }
    if (options.load_launcher != "none" && options.load_launcher != "accept" && options.load_launcher != "refuse") {
        failure = "--eawr-load-launcher expects none, accept or refuse";
        return;
    }
    static_cast<void>(root);
    load_active = this;
    const std::filesystem::path store = personal_replay_store(options.replay_store);
    load_store = ViewerPath::utf8(store);
    std::filesystem::path saves = options.save_directory;
    if (saves.empty()) {
        // The save directory the in-campaign dialog uses by default: the user data directory's saves folder.
        const String directory = ProjectSettings::get_singleton()->globalize_path("user://saves");
        saves = ViewerPath{std::string(directory.utf8().get_data())}.native();
    }
    load_saves = ViewerPath::utf8(saves);
    load_refusal = "the saved game was made by another build";

    LoadGameContext context;
    context.boxes = MessageBoxContext{&*catalog, &*theme_model, text_lookup ? &*text_lookup : nullptr, theme,
                                      Vector2(static_cast<float>(width), static_cast<float>(height))};
    context.store = store;
    context.saves = std::make_shared<viewer::DirectorySavedGameStore>(saves, viewer::local_saved_game_clock(), false);
    if (options.load_launcher != "none") {
        // The stand-in for the restore hand-off: it counts the request and accepts or refuses it.
        const bool refuse = options.load_launcher == "refuse";
        context.launcher = [this, refuse](const model::SavedGameRequest&) {
            ++load_launches;
            return refuse ? load_refusal : std::string{};
        };
    }
    load_screen = memnew(EawrLoadGameScreen);
    if (!load_screen->open(*layer, context)) {
        failure = load_screen->failure();
        memdelete(load_screen);
        load_screen = nullptr;
        return;
    }
    load_id = static_cast<int64_t>(load_screen->get_instance_id());
    load_screen->connect("replay_chosen", callable_mp_static(&State::on_load_replay));
    load_screen->connect("save_chosen", callable_mp_static(&State::on_load_save));
    load_screen->connect("closed", callable_mp_static(&State::on_load_closed));
    if (const data::Dialog* found = catalog->script.find("IDD_LOAD_GAME_DIALOG")) {
        samples.push_back({"dialog", "normal", theme_model->variation(*found, nullptr), load_screen->frame(),
                           Rect2(load_screen->frame()->get_position(), load_screen->frame()->get_size()), 0U});
    }
    if (!options.notice.empty() && !load_screen->show_notice(options.notice)) {
        failure = load_screen->failure();
        return;
    }
    if (options.load_game != "script" && options.load_game != "saves" && options.load_game != "save-replay") return;

    Viewport* viewport = layer->get_viewport();
    const auto centre = [this](const std::string& id) -> Vector2 {
        Control* control = load_alive() ? load_screen->gadget(id) : nullptr;
        return control != nullptr ? control->get_global_rect().get_center() : Vector2(-1.0F, -1.0F);
    };
    const auto list = [this]() -> ItemList* {
        return load_alive() ? Object::cast_to<ItemList>(load_screen->gadget("IDC_SAVE_GAME_LIST")) : nullptr;
    };
    const auto button_disabled = [this](const std::string& id) {
        auto* button = load_alive() ? Object::cast_to<BaseButton>(load_screen->gadget(id)) : nullptr;
        return button == nullptr || button->is_disabled();
    };

    if (options.load_game == "save-replay") {
        const auto bytes = platform::read_text_file(options.save_replay_fixture);
        if (!bytes || bytes->empty()) { failure = "Save replay test requires a finished recording fixture."; return; }
        const auto* replay_dialog = catalog->script.find("IDD_BATTLE_END_DIALOG");
        if (!replay_dialog) { failure = "The fixture requires IDD_BATTLE_END_DIALOG."; return; }
        load_screen->hide();
        replay_end = memnew(EawrBattleOverlay);
        replay_end->set_size(context.boxes.viewport_size);
        root.add_child(replay_end);
        EawrBattleOverlay::Style style;
        style.victory_title = text_lookup ? String(data::to_utf8(text_lookup->text("TEXT_WIN_BATTLE")).c_str()) : String("Victory");
        style.exit_text = "Continue";
        style.space = [this] { return theme_model->space; };
        style.results_dialog = [this, replay_dialog](Vector2) {
            auto built = build_dialog(*replay_dialog, *theme_model, text_lookup ? &*text_lookup : nullptr, model::Placement::full_screen);
            built.frame->set_theme(theme);
            return built;
        };
        style.save_replay = [this, context, bytes]() -> CanvasLayer* {
            SaveReplayContext save;
            save.boxes = context.boxes;
            save.store = context.store;
            save.recorder = [this, bytes](std::string& error) {
                ++replay_serializations;
                error.clear();
                return std::vector<std::uint8_t>(bytes->begin(), bytes->end());
            };
            save_screen = memnew(EawrSaveReplayScreen);
            if (!save_screen->open(*layer, std::move(save))) {
                failure = save_screen->failure(); memdelete(save_screen); save_screen = nullptr; return nullptr;
            }
            save_id = static_cast<int64_t>(save_screen->get_instance_id());
            return save_screen;
        };
        replay_end->setup(std::move(style), [] {}, [] {});
        // UI/store coverage with a pinned recording fixture under multiplayer visibility.
        // Finished-session serialization has its own live-session contract.
        replay_end->allow_save_replay(true);
        replay_end->show_end(true);
        const auto save_alive = [this] { return save_screen && UtilityFunctions::is_instance_id_valid(save_id); };
        const auto edit_name = [this, save_alive](const String& name) {
            if (!save_alive()) return;
            auto* edit = Object::cast_to<LineEdit>(save_screen->gadget("IDC_SAVE_REPLAY_NAME_EDIT"));
            edit->set_text(name);
            edit->emit_signal("text_changed", name);
        };
        const std::string fixture_name = "Saved battle-\xF0\x9F\x98\x80-\xCE\xA3";
        const auto saved_file = store / model::path_from_utf8(fixture_name + ".eawr-replay");
        // Pre-existing same name exercises the confirmation against the actual file.
        std::string write_error;
        std::string seed = *bytes;
        if (seed.size() < 104U) { failure = "The recording fixture has no tactical header."; return; }
        seed[40] = static_cast<char>(static_cast<unsigned char>(seed[40]) ^ 1U); // a different final tick count
        const std::vector<std::uint8_t> original(seed.begin(), seed.end());
        if (platform::write_replay_store(store, fixture_name, original, false, write_error) != platform::ReplayWriteResult::saved) {
            failure = "Cannot seed the overwrite fixture: " + write_error; return;
        }
        box_steps.push_back({"battle end opens Save Replay", [this, viewport] {
            if (const auto rect = replay_end->control_rect("save_replay")) send_click(*viewport, rect->get_center());
        }, [this, save_alive]() -> std::string {
            if (!save_alive()) return "Save Replay did not open";
            const auto capture_path = options.capture_path;
            options.capture_path = capture_path.parent_path() / (capture_path.stem().native() + std::filesystem::path(".save.png").native());
            const bool captured = capture(); options.capture_path = capture_path;
            auto* button = Object::cast_to<BaseButton>(save_screen->gadget("IDC_SAVE_REPLAY_BUTTON"));
            return captured && button->is_disabled() && replay_serializations == 0 ? std::string{}
                : "empty name enabled Save, open called the recorder, or capture failed";
        }});
        box_steps.push_back({"invalid name disables Save and Enter does nothing", [edit_name] {
            edit_name("../escape");
            for (const bool pressed : {true, false}) {
                Ref<InputEventKey> event; event.instantiate(); event->set_keycode(KEY_ENTER); event->set_pressed(pressed);
                Input::get_singleton()->parse_input_event(event);
            }
        }, [this, save_alive]() -> std::string {
            return save_alive() && Object::cast_to<BaseButton>(save_screen->gadget("IDC_SAVE_REPLAY_BUTTON"))->is_disabled()
                && !save_screen->box_open() && replay_serializations == 0 ? std::string{} : "disabled Save was activated by Enter";
        }});
        box_steps.push_back({"Cancel never calls the recorder", [this, viewport, save_alive] {
            if (save_alive()) send_click(*viewport, save_screen->gadget("IDC_SAVE_CANCEL_BUTTON")->get_global_rect().get_center());
        }, [this, save_alive, saved_file, seed]() -> std::string {
            return !save_alive() && replay_serializations == 0 && platform::read_text_file(saved_file) == seed
                ? std::string{} : "Cancel serialized or changed the seed";
        }});
        box_steps.push_back({"Save Replay reopens without calling the recorder", [this, viewport] {
            if (const auto rect = replay_end->control_rect("save_replay")) send_click(*viewport, rect->get_center());
        }, [this, save_alive]() -> std::string {
            return save_alive() && replay_serializations == 0 ? std::string{} : "reopen called the recorder";
        }});
        box_steps.push_back({"saving an existing Unicode name asks with literal text", [this, viewport, edit_name, save_alive, fixture_name] {
            edit_name(String::utf8(fixture_name.c_str()));
            if (save_alive()) send_click(*viewport, save_screen->gadget("IDC_SAVE_REPLAY_BUTTON")->get_global_rect().get_center());
        }, [this, save_alive, fixture_name]() -> std::string {
            return save_alive() && save_screen->box_open() && replay_serializations == 1
                && save_screen->box()->line_text("IDC_USER_TEXT_STRING_2") == String::utf8(fixture_name.c_str())
                ? std::string{} : "confirmation did not show the literal Unicode name or call the recorder once";
        }});
        box_steps.push_back({"No preserves the recording", [this, viewport, save_alive] {
            if (save_alive() && save_screen->box()) send_click(*viewport,
                save_screen->box()->button("IDC_DIALOG_BOX_BUTTON_RIGHT")->get_global_rect().get_center());
        }, [this, save_alive, saved_file, seed]() -> std::string {
            return save_alive() && !save_screen->box_open() && platform::read_text_file(saved_file) == seed
                ? std::string{} : "No did not preserve the recording";
        }});
        box_steps.push_back({"save asks again", [this, viewport, save_alive] {
            if (save_alive()) send_click(*viewport, save_screen->gadget("IDC_SAVE_REPLAY_BUTTON")->get_global_rect().get_center());
        }, [this, save_alive]() -> std::string { return save_alive() && save_screen->box_open() ? std::string{} : "missing confirmation"; }});
        box_steps.push_back({"Yes saves unchanged replay bytes", [this, viewport, save_alive] {
            if (save_alive() && save_screen->box()) send_click(*viewport,
                save_screen->box()->button("IDC_DIALOG_BOX_BUTTON_LEFT")->get_global_rect().get_center());
        }, [this, save_alive, saved_file, bytes, seed]() -> std::string {
            return !save_alive() && replay_serializations == 1 && platform::read_text_file(saved_file) == bytes
                && platform::read_text_file(saved_file) != seed ? std::string{} : "save stayed open, kept the seed or changed replay bytes";
        }});
        box_steps.push_back({"Load Game opens after Save Replay", [this] {
            replay_end->hide(); load_screen->refresh(); load_screen->show();
        }, [this]() -> std::string { return load_alive() && load_screen->is_visible() ? std::string{} : "Load Game did not open"; }});
    }

    if (options.load_game == "saves") {
        const bool launcher = options.load_launcher != "none";
        const bool refuses = options.load_launcher == "refuse";
        // The first and last readable rows: the first one is Loaded, the last one is deleted.
        const auto readable = [this](const bool last) -> std::optional<std::size_t> {
            if (!load_alive()) return std::nullopt;
            std::optional<std::size_t> found;
            const auto& rows = load_screen->model().visible();
            for (std::size_t row = 0; row < rows.size(); ++row) {
                if (!rows[row].playable) continue;
                if (!found || last) found = row;
            }
            return found;
        };
        const auto click_row = [this, viewport, list](const std::optional<std::size_t> row) {
            ItemList* items = list();
            if (!row || items == nullptr || static_cast<int32_t>(*row) >= items->get_item_count()) return;
            const Rect2 rect = items->get_item_rect(static_cast<int32_t>(*row));
            send_click(*viewport, items->get_global_position() + rect.get_center());
        };
        const auto chosen = std::make_shared<std::string>(); // the row a step selected, for the next check
        const auto count_before = std::make_shared<std::size_t>();

        box_steps.push_back({"the saved games are listed with their dates and nothing is selected",
                             [] {},
                             [this, list, button_disabled]() -> std::string {
                                 if (!load_alive()) return "the screen is not open";
                                 const auto& rows = load_screen->model().visible();
                                 if (load_screen->model().show_recordings()) return "the screen opened on the recordings";
                                 if (rows.empty()) return "the save directory lists no saved games";
                                 if (list() == nullptr || static_cast<std::size_t>(list()->get_item_count()) != rows.size()) {
                                     return "the list does not show every saved game";
                                 }
                                 load_rows.clear();
                                 for (std::size_t row = 0; row < rows.size(); ++row) {
                                     if (rows[row].kind != model::EntryKind::saved_game) return "a row is not a saved game";
                                     load_rows.push_back({rows[row].file_name, load_screen->row_text(row),
                                                          load_screen->row_date(row), rows[row].problem, rows[row].playable});
                                 }
                                 if (!button_disabled("IDC_LOAD_GAME_BUTTON")) return "Load is enabled with nothing selected";
                                 if (load_screen->load_tooltip() != "no saved game is selected") {
                                     return "Load's reason is '" + load_screen->load_tooltip() + "'";
                                 }
                                 return button_disabled("IDC_DELETE_GAME_BUTTON") ? std::string{}
                                                                                  : "Delete is enabled with nothing selected";
                             }});
        box_steps.push_back({"selecting a readable save enables Delete, and Load once a launcher exists",
                             [click_row, readable] { click_row(readable(false)); },
                             [this, button_disabled, launcher]() -> std::string {
                                 if (!load_alive() || !load_screen->model().selected()) return "the click selected nothing";
                                 if (button_disabled("IDC_DELETE_GAME_BUTTON")) return "Delete stayed disabled";
                                 if (button_disabled("IDC_LOAD_GAME_BUTTON") == launcher) {
                                     return launcher ? "Load stayed disabled with a launcher" : "Load is enabled with no launcher";
                                 }
                                 if (!launcher && load_screen->load_tooltip() != std::string(model::no_restore_launcher_reason)) {
                                     return "Load's reason is '" + load_screen->load_tooltip() + "'";
                                 }
                                 return {};
                             }});
        box_steps.push_back({"Delete asks for confirmation and No keeps the save",
                             [this, viewport, centre, count_before] {
                                 if (load_alive()) *count_before = load_screen->model().visible().size();
                                 send_click(*viewport, centre("IDC_DELETE_GAME_BUTTON"));
                             },
                             [this]() -> std::string {
                                 return load_alive() && load_screen->box_open() ? std::string{}
                                                                                 : "Delete did not open the confirmation";
                             }});
        box_steps.push_back({"No closes the confirmation and keeps every save",
                             [this, viewport] {
                                 if (!load_alive() || !load_screen->box_open()) return;
                                 Control* answer = load_screen->box()->button("IDC_DIALOG_BOX_BUTTON_RIGHT");
                                 if (answer != nullptr) send_click(*viewport, answer->get_global_rect().get_center());
                             },
                             [this, count_before]() -> std::string {
                                 if (!load_alive()) return "the screen closed";
                                 if (load_screen->box_open()) return "the confirmation stayed open";
                                 return load_screen->model().visible().size() == *count_before ? std::string{}
                                                                                               : "No deleted a save";
                             }});
        box_steps.push_back({"selecting the last readable save",
                             [click_row, readable, this, chosen] {
                                 const auto row = readable(true);
                                 if (row) *chosen = load_screen->model().visible()[*row].file_name;
                                 click_row(row);
                             },
                             [this, chosen]() -> std::string {
                                 if (!load_alive() || load_screen->model().selected_entry() == nullptr) return "the click selected nothing";
                                 return load_screen->model().selected_entry()->file_name == *chosen ? std::string{}
                                                                                                    : "another row was selected";
                             }});
        box_steps.push_back({"Delete asks again",
                             [this, viewport, centre] { send_click(*viewport, centre("IDC_DELETE_GAME_BUTTON")); },
                             [this]() -> std::string {
                                 return load_alive() && load_screen->box_open() ? std::string{}
                                                                                 : "Delete did not open the confirmation";
                             }});
        box_steps.push_back({"Yes deletes the save file and its row",
                             [this, viewport] {
                                 if (!load_alive() || !load_screen->box_open()) return;
                                 Control* answer = load_screen->box()->button("IDC_DIALOG_BOX_BUTTON_LEFT");
                                 if (answer != nullptr) send_click(*viewport, answer->get_global_rect().get_center());
                             },
                             [this, chosen, count_before]() -> std::string {
                                 if (!load_alive()) return "the screen closed";
                                 if (load_screen->box_open()) return "the confirmation stayed open";
                                 const auto& rows = load_screen->model().visible();
                                 if (rows.size() + 1 != *count_before) return "the list did not lose exactly one row";
                                 for (const auto& row : rows) {
                                     if (row.file_name == *chosen) return "the deleted save is still listed";
                                 }
                                 std::error_code error;
                                 const std::filesystem::path file = model::path_from_utf8(load_saves) / (*chosen + ".eawrsave");
                                 return std::filesystem::exists(file, error) ? "the save file is still on disk" : std::string{};
                             }});
        if (!launcher) return;
        box_steps.push_back({"selecting the first readable save",
                             [click_row, readable, this, chosen] {
                                 const auto row = readable(false);
                                 if (row) *chosen = load_screen->model().visible()[*row].file_name;
                                 click_row(row);
                             },
                             [this, chosen, button_disabled]() -> std::string {
                                 if (!load_alive() || load_screen->model().selected_entry() == nullptr) return "the click selected nothing";
                                 if (load_screen->model().selected_entry()->file_name != *chosen) return "another row was selected";
                                 return button_disabled("IDC_LOAD_GAME_BUTTON") ? "Load is disabled" : std::string{};
                             }});
        box_steps.push_back({"Load opens the Loading box",
                             [this, viewport, centre] { send_click(*viewport, centre("IDC_LOAD_GAME_BUTTON")); },
                             [] { return std::string{}; }});
        box_steps.push_back({refuses ? "a refusing launcher keeps the screen open with the reason"
                                     : "the launcher gets the save and the screen closes",
                             [] {},
                             [this, chosen, refuses]() -> std::string {
                                 if (load_launches != 1) return "the launcher ran " + std::to_string(load_launches) + " times";
                                 if (refuses) {
                                     if (load_save) return "a refused load still chose a save";
                                     if (!load_alive() || !load_screen->box_open()) return "the refusal was not shown";
                                     for (const auto& line : load_screen->box()->plan().lines) {
                                         if (line.second == load_refusal) return {};
                                     }
                                     return "the box does not carry the refusal";
                                 }
                                 if (!load_save) return "Load did not choose a save";
                                 if (load_save->second != *chosen) return "the request names '" + load_save->second + "'";
                                 if (model::path_from_utf8(load_save->first) != model::path_from_utf8(load_saves)) {
                                     return "the request names directory '" + load_save->first + "'";
                                 }
                                 return load_loading_boxes >= 1 ? std::string{} : "the Loading box never opened";
                             }});
        return;
    }

    box_steps.push_back({"with no saved games the list is empty and Load and Delete are off",
                         [] {},
                         [this, list, button_disabled]() -> std::string {
                             if (!load_alive()) return "the screen is not open";
                             if (list() == nullptr || list()->get_item_count() != 0) return "the saved-game list is not empty";
                             if (!button_disabled("IDC_LOAD_GAME_BUTTON")) return "Load is enabled with no saved games";
                             return button_disabled("IDC_DELETE_GAME_BUTTON") ? std::string{}
                                                                              : "Delete is enabled with no saved games";
                         }});
    box_steps.push_back({"the recordings check lists the replay store",
                         [this, viewport, centre] { send_click(*viewport, centre("IDC_CHECK_RECORDINGS")); },
                         [this, list]() -> std::string {
                             if (!load_alive()) return "the screen closed";
                             if (!load_screen->model().show_recordings()) return "the check did not switch the list";
                             const std::size_t files = load_screen->model().visible().size();
                             if (files == 0) return "the replay store lists no recordings";
                             return list() != nullptr && static_cast<std::size_t>(list()->get_item_count()) == files
                                 ? std::string{} : "the list does not show every recording";
                         }});
    box_steps.push_back({"selecting a recording enables Load and Delete",
                         [this, viewport, list] {
                             ItemList* items = list();
                             if (items == nullptr || items->get_item_count() == 0) return;
                             const Rect2 rect = items->get_item_rect(0);
                             send_click(*viewport, items->get_global_position() + rect.get_center());
                         },
                         [this, button_disabled]() -> std::string {
                             if (!load_alive() || !load_screen->model().selected()) return "the click selected nothing";
                             if (button_disabled("IDC_LOAD_GAME_BUTTON")) return "Load stayed disabled";
                             return button_disabled("IDC_DELETE_GAME_BUTTON") ? "Delete stayed disabled" : std::string{};
                         }});
    box_steps.push_back({"Delete asks for confirmation and No keeps the file",
                         [this, viewport, centre] { send_click(*viewport, centre("IDC_DELETE_GAME_BUTTON")); },
                         [this]() -> std::string {
                             if (!load_alive() || !load_screen->box_open()) return "Delete did not open the confirmation";
                             return load_screen->model().visible().empty() ? "the list lost its recording" : std::string{};
                         }});
    box_steps.push_back({"No closes the confirmation and keeps the list",
                         [this, viewport] {
                             if (!load_alive() || !load_screen->box_open()) return;
                             Control* answer = load_screen->box()->button("IDC_DIALOG_BOX_BUTTON_RIGHT");
                             if (answer != nullptr) send_click(*viewport, answer->get_global_rect().get_center());
                         },
                         [this]() -> std::string {
                             if (!load_alive()) return "the screen closed";
                             if (load_screen->box_open()) return "the confirmation stayed open";
                             return load_screen->model().visible().empty() ? "No deleted the recording" : std::string{};
                         }});
    box_steps.push_back({"Load plays the selected recording",
                         [this, viewport, centre] { send_click(*viewport, centre("IDC_LOAD_GAME_BUTTON")); },
                         [this]() -> std::string {
                             if (!load_play) return "Load did not choose a recording";
                             return load_play->first.empty() ? "the chosen recording has no path" : std::string{};
                         }});
}

} // namespace eawr::presentation::godot_backend

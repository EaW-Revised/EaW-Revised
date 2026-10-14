#include "galactic_save.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/battle_overlay.hpp"
#include "ui/theme_builder.hpp"
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/property_tweener.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
void selected_slot(int64_t index, int64_t address) {
    reinterpret_cast<GalacticSaveControls*>(address)->select_slot(index);
}
void activated_slot(int64_t index, int64_t address) {
    reinterpret_cast<GalacticSaveControls*>(address)->activate_slot(index);
}
void caption(Control* control, const std::string& text) {
    if (auto* label = Object::cast_to<EawrUiLabel>(control)) label->set_text(String::utf8(text.c_str()));
    if (auto* button = Object::cast_to<EawrUiButton>(control)) button->set_text(String::utf8(text.c_str()));
}
void action(Control* control, std::function<void()> callback) {
    auto* button = Object::cast_to<BaseButton>(control);
    if (!button) return;
    auto* relay = memnew(EawrOverlayButton); button->add_child(relay); relay->hide();
    relay->set_action(std::move(callback));
    button->connect("pressed", callable_mp(relay, &EawrOverlayButton::activate));
}
std::tm local_time(std::int64_t seconds) {
    const auto time = static_cast<std::time_t>(seconds); std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &time);
#else
    localtime_r(&time, &result);
#endif
    return result;
}
std::string local_timestamp(const std::tm& time) {
    // WGS-155/161, FMN-E02/04: user-locale date + " - " + user-locale
    // time (GetDateFormat/GetTimeFormat with no explicit format).
#ifdef _WIN32
    SYSTEMTIME value{};
    value.wYear = static_cast<WORD>(time.tm_year + 1900);
    value.wMonth = static_cast<WORD>(time.tm_mon + 1);
    value.wDay = static_cast<WORD>(time.tm_mday);
    value.wHour = static_cast<WORD>(time.tm_hour);
    value.wMinute = static_cast<WORD>(time.tm_min);
    value.wSecond = static_cast<WORD>(time.tm_sec);
    wchar_t date[128]{};
    wchar_t clock[128]{};
    if (GetDateFormatW(LOCALE_USER_DEFAULT, 0, &value, nullptr, date, 128) &&
        GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &value, nullptr, clock, 128)) {
        const auto text = String::utf16(reinterpret_cast<const char16_t*>(date)) + " - " +
            String::utf16(reinterpret_cast<const char16_t*>(clock));
        return text.utf8().get_data();
    }
#endif
    std::ostringstream result;
    try { result.imbue(std::locale("")); }
    catch (const std::runtime_error&) { /* Unavailable locale uses the classic fallback. */ }
    result << std::put_time(&time, "%x - %X");
    return result.str();
}
}
std::string GalacticSaveControls::localized(std::string_view key) const {
    return lookup_ ? lookup_(key) : std::string(key);
}
viewer::galactic::SaveMenuEnabled GalacticSaveControls::admission() const {
    viewer::galactic::SaveMenuAdmission facts;
    if (state_.game) {
        const auto publication = state_.game->save_menu_facts();
        facts.pending_battle = publication.pending_battle;
        facts.tactical = publication.tactical;
    }
    // GC coop preview is an explicit multiplayer campaign route. Tutorial-player,
    // movie/letterbox and synchronized network admission need the save-owner view
    // (WGS-U04, legacy #1954); the generic tutorial control lock is not that fact.
    facts.multiplayer_campaign = state_.options.coop_preview;
    return viewer::galactic::save_menu_enabled(facts);
}
bool GalacticSaveControls::build_widgets() {
    if (root_) return true;
    if (!state_.filesystem || !state_.hud_fonts) return false;
    auto loaded = data::ui::load_dialog_catalog(*state_.filesystem);
    auto database = data::ui::load_language_text_database(*state_.filesystem, "english");
    if (!loaded || !database) { widget_failure_ = "Save dialog resources unavailable."; return false; }
    auto catalog = std::make_shared<data::ui::DialogCatalog>(std::move(loaded).value());
    auto text = std::make_shared<data::ui::TextDatabase>(std::move(database).value());
    auto loaded_atlas = assets::load_mega_texture_atlas(*state_.filesystem,
        "data/art/textures/" + catalog->skin.texture_file + ".mtd");
    if (!loaded_atlas) { widget_failure_ = "Save dialog skin unavailable."; return false; }
    auto atlas = std::make_shared<assets::MegaTextureAtlas>(std::move(loaded_atlas).value());
    auto textures = std::make_shared<UiTextures>(*atlas, *state_.filesystem);
    lookup_ = [text](std::string_view key) {
        const auto* value = text->find(key);
        return value ? data::ui::to_utf8(value->value) : std::string(key);
    };
    build_ = [this, catalog, text, atlas, textures](std::string_view id) {
        const auto* definition = catalog->script.find(id);
        if (!definition) return BuiltDialog{};
        const auto size = state_.host->get_viewport()->get_visible_rect().size;
        presentation::ui::ThemeSources sources;
        sources.catalog = catalog.get(); sources.atlas = &atlas->directory;
        sources.standalone = presentation::ui::vfs_standalone_textures(*state_.filesystem);
        sources.fonts = &state_.hud_fonts->cache(); sources.language = "english";
        const auto& families = state_.hud_fonts->system_families();
        sources.system = [&families](std::string_view face) {
            return presentation::ui::match_system_face(face, families).has_value();
        };
        auto theme = presentation::ui::build_theme_model(sources,
            presentation::ui::reference_space({static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)}));
        data::ui::TextLookup lookup(*text);
        auto built = build_dialog(*definition, theme, &lookup, presentation::ui::Placement::centre);
        built.frame->set_theme(build_theme(theme, *textures, *state_.hud_fonts));
        return built;
    };
    layer_ = memnew(CanvasLayer); layer_->set_name("GalacticSaveLayer"); layer_->set_layer(50);
    state_.host->add_child(layer_);
    root_ = memnew(Control); layer_->add_child(root_); root_->set_name("GalacticSaveMenu");
    root_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    root_->set_mouse_filter(Control::MOUSE_FILTER_STOP); root_->set_process_mode(Node::PROCESS_MODE_ALWAYS);
    root_->set_meta("save_world_leaks", world_leaks_);
    escape_dialog_ = build_("IDD_GAME_OPTIONS_DIALOG");
    save_dialog_ = build_("IDD_SAVE_GAME_DIALOG"); load_dialog_ = build_("IDD_LOAD_GAME_DIALOG");
    if (!escape_dialog_->frame || !save_dialog_->frame || !load_dialog_->frame) {
        widget_failure_ = "Save/load dialog definition unavailable.";
        for (auto* dialog : {&*escape_dialog_, &*save_dialog_, &*load_dialog_})
            if (dialog->frame) memdelete(dialog->frame);
        escape_dialog_.reset(); save_dialog_.reset(); load_dialog_.reset();
        memdelete(layer_); layer_ = nullptr; root_ = nullptr; return false;
    }
    for (auto* dialog : {&*escape_dialog_, &*save_dialog_, &*load_dialog_}) {
        root_->add_child(dialog->frame);
        for (auto& gadget : dialog->gadgets) {
            if (!gadget.control) continue;
            const auto& id = gadget.id; auto* control = gadget.control;
            controls_.push_back({id, control});
            if (id == "IDC_CLOSE_BUTTON" || id == "IDC_CHECK_RECORDINGS") { control->hide(); continue; }
            if (auto* button = Object::cast_to<BaseButton>(control)) buttons_.push_back(button);
            if (id == "IDC_SAVE_GAME") action(control, [this] { if (admission().save) show_screen(Screen::save); });
            else if (id == "IDC_LOAD_GAME") action(control, [this] { if (admission().load) show_screen(Screen::load); });
            else if (id == "IDC_GAME_OPTIONS_RESUME") action(control, [this] { close(); });
            else if (id == "IDC_SAVE_CANCEL_BUTTON" || id == "IDC_LOAD_CANCEL_BUTTON")
                action(control, [this] { show_screen(Screen::escape); });
            else if (id == "IDC_SAVE_GAME_BUTTON") action(control, [this] { save(); });
            else if (id == "IDC_LOAD_GAME_BUTTON") action(control, [this] { load(); });
            else if (id == "IDC_DELETE_GAME_BUTTON") action(control, [this] { erase(); });
            else if (id == "IDC_INGAME_OPTIONS" || id == "IDC_GAME_OPTIONS_QUIT") {
                // GHUD-S44/S47 own settings and exit dispatch; expose the authored
                // controls disabled until their provider routes are attached.
                control->set_meta("provider_seam", "GHUD-S44/S47");
            } else if (id == "IDC_SAVE_GAME_NAME_EDIT") {
                name_ = Object::cast_to<LineEdit>(control); name_->set_name("SaveName");
                name_->set_max_length(63); // Toolkit scrolling remains the filename-control seam (WGS-154).
            } else if (id == "IDC_SAVE_GAME_LIST") {
                auto* list = Object::cast_to<ItemList>(control);
                list->connect("item_selected", callable_mp_static(&selected_slot).bind(reinterpret_cast<int64_t>(this)));
                if (dialog == &*save_dialog_) save_slots_ = list;
                else {
                    load_slots_ = list;
                    list->connect("item_activated", callable_mp_static(&activated_slot).bind(reinterpret_cast<int64_t>(this)));
                }
            }
        }
    }
    modal_root_ = memnew(Control); root_->add_child(modal_root_);
    modal_root_->set_name("SaveModal"); modal_root_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    modal_root_->set_mouse_filter(Control::MOUSE_FILTER_STOP); modal_root_->hide();
    root_->hide(); return true;
}
void GalacticSaveControls::show_screen(Screen screen) {
    if (modal_ != Modal::none) return;
    screen_ = screen; selected_index_ = -1;
    escape_dialog_->frame->set_visible(screen != Screen::load);
    save_dialog_->frame->set_visible(screen == Screen::save);
    load_dialog_->frame->set_visible(screen == Screen::load);
    slots_ = screen == Screen::load ? load_slots_ : save_slots_;
    if (screen != Screen::escape) { populate_slots(); list(); }
}
void GalacticSaveControls::select_slot(std::int64_t index) {
    if (modal_ != Modal::none) return;
    selected_index_ = index;
    if (screen_ != Screen::save) return;
    if (index < 0 || static_cast<std::size_t>(index) >= rows_.size() || rows_[static_cast<std::size_t>(index)] == listed_.size())
        name_->clear();
    else name_->set_text(String::utf8(listed_[rows_[static_cast<std::size_t>(index)]].description.c_str()));
}
void GalacticSaveControls::activate_slot(std::int64_t index) {
    select_slot(index);
    if (screen_ == Screen::load) load();
}
void GalacticSaveControls::populate_slots() {
    if (!slots_ || screen_ == Screen::escape) return;
    for (auto& [index, label] : dates_) { static_cast<void>(index); memdelete(label); }
    dates_.clear();
    slots_->clear(); rows_.clear(); selected_index_ = -1;
    if (screen_ == Screen::save) {
        rows_.push_back(listed_.size()); slots_->add_item(String::utf8(localized("TEXT_EMPTY_SLOT").c_str()));
    }
    struct Row { std::size_t index; std::int64_t key; std::string date; };
    std::vector<Row> ordered;
    for (std::size_t i = 0; i < listed_.size() && (screen_ == Screen::save || i < 100); ++i) {
        const auto& slot = listed_[i];
        const auto time = local_time(slot.last_write_seconds.value_or(0));
        const auto date = slot.last_write_seconds ? local_timestamp(time) : std::string{};
        ordered.push_back({i, viewer::galactic::save_list_key(slot.description, time.tm_year + 1900,
            time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec), date});
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const Row& a, const Row& b) { return a.key < b.key; });
    for (const auto& row : ordered) {
        const auto& slot = listed_[row.index];
        const auto label = slot.description == "[AutoSave]" ? localized("TEXT_GUI_AUTO_SAVE")
            : slot.description == "[QuickSave]" ? localized("TEXT_GUI_QUICK_SAVE") : slot.name;
        rows_.push_back(row.index);
        // Name/date only: no fabricated campaign/difficulty/preview metadata.
        const auto index = slots_->add_item(String::utf8(label.c_str()));
        auto* date = memnew(EawrUiLabel); slots_->add_child(date);
        date->set_text(String::utf8(row.date.c_str())); date->set_wrap(false); date->set_role("List_Box");
        date->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        date->set_theme_type_variation(slots_->get_theme_type_variation());
        dates_.push_back({index, date});
    }
    if (screen_ == Screen::save) { slots_->select(0); select_slot(0); name_->grab_focus(); }
}
void GalacticSaveControls::position_dates() {
    if (!slots_ || screen_ == Screen::escape) return;
    const auto width = slots_->get_size().x;
    const auto scroll = static_cast<float>(slots_->get_v_scroll_bar()->get_value());
    for (auto& [index, date] : dates_) {
        const auto rect = slots_->get_item_rect(index);
        const auto y = rect.position.y - scroll;
        date->set_position(Vector2(width * 0.68F, y));
        date->set_size(Vector2(width * 0.30F, rect.size.y));
        date->set_visible(y >= 0 && y + rect.size.y <= slots_->get_size().y);
    }
}
void GalacticSaveControls::observe_world_selection() {
    if (!visible() || state_.map_input.selected == menu_selection_) return;
    ++world_leaks_; menu_selection_ = state_.map_input.selected;
    root_->set_meta("save_world_leaks", world_leaks_);
}
void GalacticSaveControls::show_modal(Modal modal, std::string text) {
    if (modal_dialog_) { modal_dialog_->frame->hide(); modal_dialog_->frame->queue_free(); }
    modal_ = modal; ++modal_admissions_; message(std::move(text));
    const bool progress = modal == Modal::progress_save || modal == Modal::progress_load;
    const bool confirmation = modal == Modal::overwrite || modal == Modal::erase;
    modal_dialog_ = build_(progress ? "IDD_DIALOG_BOX_SMALL" : confirmation ? "IDD_DIALOG_BOX2" : "IDD_DIALOG_BOX1");
    if (!modal_dialog_->frame) { widget_failure_ = "Save result dialog unavailable."; modal_ = Modal::none; return; }
    modal_root_->add_child(modal_dialog_->frame); modal_root_->show();
    for (auto& gadget : modal_dialog_->gadgets) {
        if (!gadget.control) continue;
        if (gadget.id == "IDC_CAPTION") { caption(gadget.control, text_); gadget.control->set_meta("save_caption", String::utf8(text_.c_str())); }
        else if (gadget.id == "IDC_DIALOG_BOX_BUTTON_LEFT") {
            caption(gadget.control, localized(confirmation ? "TEXT_BUTTON_YES" : "TEXT_BUTTON_OK"));
            action(gadget.control, [this, confirmation] { dismiss_modal(confirmation); });
        } else if (gadget.id == "IDC_DIALOG_BOX_BUTTON_RIGHT") {
            caption(gadget.control, localized("TEXT_BUTTON_NO")); action(gadget.control, [this] { dismiss_modal(); });
        } else gadget.control->hide();
    }
    if (progress) {
        // Project fade duration; WGS-158/162 source ordering, not milliseconds.
        modal_dialog_->frame->set_modulate(Color(1, 1, 1, 0));
        fade_ = modal_dialog_->frame->create_tween();
        fade_->tween_property(modal_dialog_->frame, "modulate:a", 1.0, 0.15);
    }
}
void GalacticSaveControls::dismiss_modal(bool yes) {
    if (modal_ == Modal::progress_save || modal_ == Modal::progress_load) return;
    const auto previous = modal_; modal_ = Modal::none; modal_root_->hide();
    if (previous == Modal::overwrite && yes) { begin_progress(true, pending_name_, true); return; }
    if (previous == Modal::erase && yes) {
        auto removed = state_.game->delete_save(pending_name_);
        // WGS-160 has no success acknowledgement. Preserve actual native error.
        if (!removed) show_modal(Modal::acknowledgement, removed.error().message);
        list(); return;
    }
    if (previous == Modal::acknowledgement && quick_route_) { quick_route_ = false; close(); }
}
void GalacticSaveControls::begin_progress(bool saving, std::string name, bool overwrite, bool quick) {
    pending_name_ = std::move(name); pending_overwrite_ = overwrite; pending_quick_ = quick;
    show_modal(saving ? Modal::progress_save : Modal::progress_load,
        localized(saving ? "TEXT_SAVELOAD_SAVING_MSG" : "TEXT_SAVELOAD_LOADING_MSG"));
}
} // namespace eawr::presentation::godot_backend

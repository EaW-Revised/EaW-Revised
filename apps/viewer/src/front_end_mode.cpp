#include "front_end_mode.hpp"

#include "attract_runner.hpp"
#include "audio_buses.hpp"
#include "battle_content.hpp"
#include "campaign_setup_screen.hpp"
#include "camera_input.hpp"
#include "options_screen.hpp"
#include "lan_lobby_screen.hpp"
#include "saved_games.hpp"
#include "settings_store.hpp"
#include "ui/load_game_view.hpp"
#include "skirmish_setup_mode.hpp"
#include "viewer_path.hpp"
#include "ui/dialog_builder.hpp"
#include "ui/font_provider.hpp"
#include "ui/kit.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/audio/sfx.hpp"
#include "eawr/presentation/ui/campaign_setup.hpp"
#include "eawr/presentation/ui/front_end.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/loading_screen.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/canvas_item.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;
namespace {

#if defined(EAWR_VIEWER_DEBUG_BUILD) && EAWR_VIEWER_DEBUG_BUILD
constexpr model::FrontEndBuild front_end_build{true};
#else
constexpr model::FrontEndBuild front_end_build{};
#endif

String text(std::string_view value) { return String::utf8(value.data(), static_cast<int64_t>(value.size())); }
std::string utf8(const String& value) { return value.utf8().get_data(); }

std::string lower(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) out.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
}

// FES-09: the events the dialogs' buttons play are the data's own: the registry names them, and a
// mod's override of the event changes the sound.
constexpr std::string_view hover_event = "GUI_Common_Dialog_Button_Hilight";
constexpr std::string_view press_event = "GUI_Common_Dialog_Button_Press";
// FES-03: every menu starts this music event when it opens.
constexpr std::string_view menu_music_event = "Main_Menu_Music_Event";

// The menus' sound: the main-menu music event (looping) and the dialog buttons' one-shots, read from
// the SFX and music event files of the loaded data. Presentation only.
class MenuAudio final {
public:
    MenuAudio(Node3D& host, const vfs::Vfs& filesystem, const data::Catalog& catalog) : filesystem_(&filesystem) {
        std::vector<const data::Definition*> definitions;
        for (const data::Definition& definition : catalog.definitions()) {
            if (definition.category == data::Category::sfx && definition.active) definitions.push_back(&definition);
        }
        std::stable_sort(definitions.begin(), definitions.end(), [](const data::Definition* a, const data::Definition* b) {
            return std::pair(a->registry_order, a->definition_order) < std::pair(b->registry_order, b->definition_order);
        });
        for (const data::Definition* definition : definitions) {
            std::vector<audio::Field> fields;
            for (const data::XmlNode& item : definition->root.children) fields.emplace_back(item.name, trim(item.raw_text));
            registry_.add(definition->id, fields, problems_);
        }
        if (auto document = data::load_document(filesystem, "data/xml/musicevents.xml")) {
            for (const data::XmlNode& node : document.value().root.children) {
                if (!same(node.name, "MusicEvent")) continue;
                std::string name;
                for (const data::XmlAttribute& attribute : node.attributes) {
                    if (same(attribute.name, "Name")) name = trim(attribute.value);
                }
                if (!same(name, menu_music_event)) continue;
                std::vector<audio::Field> fields;
                for (const data::XmlNode& item : node.children) fields.emplace_back(item.name, trim(item.raw_text));
                music_ = audio::parse_music_event(name, fields);
                have_music_ = true;
            }
        } else {
            problems_.push_back("data/xml/musicevents.xml: " + core::format_diagnostic(document.error()));
        }
        if (!have_music_ || music_.files.empty()) problems_.push_back(std::string(menu_music_event) + " names no playable file");
        // #3165: the run's shared buses (audio_buses.hpp), so the options sliders reach the menu's
        // sound. They exist from the viewer's start; a run without them gets them here at the
        // saved levels (FoC's 0.75 otherwise).
        if (!audio_buses::exist()) {
            const model::PlayerSettings* saved = ActiveSettings::get();
            audio_buses::ensure(saved != nullptr ? mix_levels(saved->audio) : audio::MixLevels{});
        }
        music_player_ = memnew(AudioStreamPlayer);
        music_player_->set_bus(audio_buses::names[audio_buses::music]);
        host.add_child(music_player_);
        for (auto& player : one_shots_) {
            player = memnew(AudioStreamPlayer);
            player->set_bus(audio_buses::names[audio_buses::sfx]);
            host.add_child(player);
        }
    }

    void set_music(const bool on) {
        if (on == music_on_) return;
        music_on_ = on;
        if (!on) {
            music_player_->stop();
            return;
        }
        start_music();
    }

    // FES-03: an event request on open starts a silent player, including an ended non-looping
    // mod track. A playing event keeps its track and phase; ordinary updates do not replay it.
    void start_music() {
        if (music_player_->is_playing()) return;
        if (!have_music_ || music_.files.empty()) return;
        if (!music_player_->get_stream().is_valid()) {
            const std::string file = lower(music_.files.front());
            if (auto bytes = filesystem_->open(audio::music_sample_path(file))) {
                PackedByteArray data;
                data.resize(static_cast<std::int64_t>(bytes.value().size()));
                std::memcpy(data.ptrw(), bytes.value().data(), bytes.value().size());
                Ref<AudioStreamMP3> stream;
                stream.instantiate();
                stream->set_data(data);
                stream->set_loop(music_.loop);
                if (stream->get_length() > 0.0) music_player_->set_stream(stream);
                else problems_.push_back("music " + file + ": undecodable");
            } else {
                problems_.push_back("music " + file + ": " + core::format_diagnostic(bytes.error()));
            }
        }
        // The event's own volume; the music and mix buses carry the sliders.
        music_player_->set_volume_db(static_cast<float>(UtilityFunctions::linear_to_db(music_.volume)));
        if (music_player_->get_stream().is_valid()) music_player_->play();
    }

    void play(const std::string_view event_name) {
        const audio::SfxEvent* event = registry_.find(event_name);
        if (event == nullptr) {
            problems_.push_back("sfx event " + std::string(event_name) + " is not in the data");
            return;
        }
        if (event->samples.empty()) return;
        const std::string& file = event->samples[next_sample_++ % event->samples.size()];
        Ref<AudioStreamWAV> stream = sample(file);
        if (!stream.is_valid()) return;
        auto* player = one_shots_[next_player_++ % one_shots_.size()];
        player->set_stream(stream);
        // Localized samples are speech, the rest effects: each plays on its slider's bus (#3165).
        player->set_bus(audio_buses::names[event->localized ? audio_buses::voice : audio_buses::sfx]);
        const double gain = event->min_volume / 100.0;
        player->set_volume_db(static_cast<float>(UtilityFunctions::linear_to_db(gain)));
        player->play();
    }

    [[nodiscard]] double music_position() const { return music_player_->get_playback_position(); }
    [[nodiscard]] bool music_playing() const { return music_player_->is_playing(); }
    [[nodiscard]] std::string music_bus() const { return utf8(String(music_player_->get_bus())); }
    [[nodiscard]] std::string music_file() const { return music_.files.empty() ? std::string{} : music_.files.front(); }
    [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }

private:
    Ref<AudioStreamWAV> sample(const std::string& name) {
        const auto found = samples_.find(name);
        if (found != samples_.end()) return found->second;
        Ref<AudioStreamWAV> stream;
        if (auto bytes = audio::open_sfx_sample(*filesystem_, name)) {
            std::string error;
            if (const auto pcm = audio::parse_wav(bytes.value(), error)) {
                PackedByteArray data;
                data.resize(static_cast<std::int64_t>(pcm->data.size()));
                std::memcpy(data.ptrw(), pcm->data.data(), pcm->data.size());
                stream.instantiate();
                stream->set_format(AudioStreamWAV::FORMAT_16_BITS);
                stream->set_mix_rate(static_cast<std::int32_t>(pcm->sample_rate));
                stream->set_stereo(pcm->channels == 2);
                stream->set_data(data);
            } else {
                problems_.push_back("sfx " + name + ": " + error);
            }
        } else {
            problems_.push_back("sfx " + name + ": " + core::format_diagnostic(bytes.error()));
        }
        samples_.emplace(name, stream);
        return stream;
    }

    const vfs::Vfs* filesystem_;
    audio::SfxRegistry registry_;
    audio::MusicEvent music_;
    bool have_music_{};
    bool music_on_{};
    AudioStreamPlayer* music_player_{};
    std::array<AudioStreamPlayer*, 4> one_shots_{};
    std::size_t next_player_{}, next_sample_{};
    std::map<std::string, Ref<AudioStreamWAV>> samples_;
    std::vector<std::string> problems_;
};

void click(Vector2 position) {
    for (bool pressed : {true, false}) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_position(position);
        event->set_global_position(position);
        event->set_button_index(MOUSE_BUTTON_LEFT);
        event->set_pressed(pressed);
        Input::get_singleton()->parse_input_event(event);
    }
}

// FES-04: a menu dialog's gadget as the shell holds it.
struct Entry final {
    std::string id;
    Control* control{};
    BaseButton* button{};
    model::GadgetRule rule;
    bool hovered{};
};

struct Menu final {
    model::FrontEndScreen screen{};
    Control* frame{};
    std::vector<Entry> entries;
    ItemList* hint{};
    bool from_data{};
};

// A button of the project-authored fallback menu (FES-10 and the malformed-dialog case below).
struct FallbackButton final {
    const char* id;
    const char* caption_key;
};
struct FallbackMenu final {
    const char* title_key;
    std::vector<FallbackButton> buttons;
};

const FallbackMenu& fallback_menu(const model::FrontEndScreen screen) {
    static const FallbackMenu main_menu{"TEXT_MAIN_MENU", {
        {"IDC_MAIN_MENU_SINGLE_PLAYER_GAMES", "TEXT_BUTTON_SINGLE_PLAYER_GAMES"},
        {"IDC_MAIN_MENU_MULTIPLAYER", "TEXT_BUTTON_MULTIPLAYER"},
        {"IDC_LOAD_GAME_MAIN", "TEXT_BUTTON_LOAD_GAME"},
        {"IDC_OPTIONS", "TEXT_BUTTON_OPTIONS"},
        {"IDC_MAIN_MENU_EXIT2", "TEXT_BUTTON_EXIT_GAME"},
        {"IDC_MAIN_MENU_TUTORIAL", "TEXT_BUTTON_TUTORIAL"}}};
    static const FallbackMenu single_player{"TEXT_SINGLE_PLAYER_GAMES_MENU_TITLE", {
        {"IDC_NEW_CAMPAIGN_BUTTON", "TEXT_BUTTON_NEW_CAMPAIGN"},
        {"IDC_CUSTOM_CAMPAIGN_BUTTON", "TEXT_BUTTON_CUSTOM_CAMPAIGN"},
        {"IDC_SKIRMISH_GAMES_BUTTON", "TEXT_BUTTON_SKIRMISH"},
#if defined(EAWR_VIEWER_DEBUG_BUILD) && EAWR_VIEWER_DEBUG_BUILD
        {"IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON", "TEXT_BUTTON_DEBUG_LOAD_SKIRMISH_MAP"},
#endif
        {"IDC_BACK_BUTTON_MAIN", "TEXT_BUTTON_BACK"}}};
    static const FallbackMenu multiplayer{"TEXT_MULTIPLAYER", {
        {"IDC_MULTIPLAYER_INTERNET_BUTTON", "TEXT_BUTTON_MULTIPLAYER_INTERNET"},
        {"IDC_MULTIPLAYER_LAN_BUTTON", "TEXT_BUTTON_MULTIPLAYER_LAN"},
        {"IDC_MAIN_OPTIONS_NETWORK", "TEXT_BUTTON_INTERNET_OPTIONS"},
        {"IDC_OK_MAIN", "TEXT_BUTTON_BACK"}}};
    switch (screen) {
    case model::FrontEndScreen::single_player: return single_player;
    case model::FrontEndScreen::multiplayer: return multiplayer;
    default: return main_menu;
    }
}

constexpr float canvas_width = 1280.0F;
constexpr float canvas_height = 720.0F;
constexpr double fade_up_seconds = 2.0;

std::size_t menu_index(const model::FrontEndScreen screen) {
    switch (screen) {
    case model::FrontEndScreen::single_player: return 1;
    case model::FrontEndScreen::multiplayer: return 2;
    default: return 0;
    }
}

constexpr std::array<model::FrontEndScreen, 3> menu_screens{model::FrontEndScreen::main_menu,
    model::FrontEndScreen::single_player, model::FrontEndScreen::multiplayer};

bool is_menu(const model::FrontEndScreen screen) { return !model::dialog_name(screen).empty(); }

// Screens drawn over the menu backdrop by the shell itself (the menus, the campaign staging screen) or
// by a screen the shell opens on top of it (settings, load game): the demo battle and the menu music
// stay up behind them.
bool over_backdrop(const model::FrontEndScreen screen) {
    return is_menu(screen) || screen == model::FrontEndScreen::options || screen == model::FrontEndScreen::network_options || screen == model::FrontEndScreen::load_game
        || screen == model::FrontEndScreen::campaign_setup;
}

} // namespace

struct FrontEndMode::State final {
    explicit State(Options value) : options(std::move(value)) {}

    Options options;
    Node3D* host{};
    SkirmishSetupMode* setup{};
    SkirmishSetupMode::UiResources resources;
    std::string failure;
    CanvasLayer* layer{};
    ColorRect* backdrop{};
    TextureRect* overlay{};
    Control* root{};
    ColorRect* fade{};
    std::optional<data::ui::TextDatabase> text_database;
    std::optional<data::ui::TextLookup> text_lookup;
    std::array<Menu, 3> menus;
    model::FrontEndNav nav;
    std::optional<model::FrontEndScreen> shown;
    // The settings screen (#2927): a node that leaves the tree itself when the player closes it.
    EawrOptionsScreen* options_screen{};
    // Godot frees the open screen with the host's children before this object is destroyed, so the
    // pointer is only trusted while the instance id is still valid.
    int64_t options_id{};
    bool options_open{};
    EawrLoadGameScreen* load_screen{};
    int64_t load_id{};
    bool campaign_started{};
    bool campaign_launching{}; // the delegated mode owns its report during synchronous startup
    // MNU-34: the custom-campaign staging screen, built from the loaded data; null when its data or
    // dialog could not be read (the single-player menu's entry is then off with the reason).
    std::unique_ptr<CampaignSetupScreen> campaign_screen;
    std::string campaign_problem;
    // Reasons shown as tooltips of entries the shell turns off at run time; the rules hold views.
    std::deque<std::string> reasons;
    // A one-button notice box over the screen (a refused campaign or saved game start).
    EawrMessageBox* notice_box{};
    int64_t notice_id{};

    // Drops a screen the engine has already freed.
    [[nodiscard]] EawrOptionsScreen* live_options() {
        if (options_screen != nullptr && !UtilityFunctions::is_instance_id_valid(options_id)) options_screen = nullptr;
        return options_screen;
    }
    // The demo battle behind the menus (MAT-01, MAT-14). Its nodes are children of the host and go
    // with it; the runner checks their instance ids before it frees them.
    std::unique_ptr<AttractRunner> attract;
    // One start attempt per stretch of screens: attract off, or no map, is not retried every frame.
    bool attract_tried{};
    bool attract_started{};
    std::string attract_failure;
    bool front_end_ready{};
    // Godot stops child audio players as the tree exits, before the host's predelete report.
    bool music_playing{};
    std::string exit_reason;
    bool report_written{};
    bool report_ok{true};
    std::unique_ptr<MenuAudio> audio;
    bool escape_was_down{true};
    std::uint32_t frames{};
    std::uint64_t fade_started_ms{};
    bool fading{};
    std::vector<std::string> notes;
    // The battle round trip's script: the frame (of this screen's own frames) the battle started
    // after, and how many frames the shell has had since the battle handed the screen back.
    bool battle_started{};
    bool battle_left_setup{};
    // A game was left for the main menu (return_to_main_menu): the scripted runs have nothing more to walk.
    bool returned_to_menu{};
    std::uint32_t frames_after_battle{};

    bool build();
    void build_campaign_screen();
    void refresh_single_player();
    std::filesystem::path save_directory() const;
    std::string restore_saved(const model::SavedGameRequest& request);
    void begin_campaign();
    void start_campaign(const model::CampaignStartRequest& request);
    void show_notice(const char* caption_key, const std::string& reason);
    [[nodiscard]] bool notice_open() const;
    std::optional<int> poll_campaign();
    std::optional<int> run_campaign_test();
    void build_menu(model::FrontEndScreen screen);
    void build_fallback(Menu& menu, model::FrontEndScreen screen);
    void apply_rule(Entry& entry, std::string_view dialog);
    void load_overlay();
    void layout();
    void show_screen();
    std::optional<int> poll_menu(Menu& menu);
    std::optional<int> step(model::Step result);
    void update_hint(Menu& menu, const Entry& entry);
    std::optional<int> run_test();
    std::optional<int> run_battle_test();
    [[nodiscard]] bool attract_wanted() const;
    void sync_attract();
    void stop_attract(const char* reason);
    void log_attract(const char* label);
    void log_behind(const char* label) const;
    void log_menu(const char* label, const Menu& menu) const;
    void log_text_scale(const char* label, const Control* control, std::string_view id) const;
    void capture(const char* label) const;
    Entry* find_entry(Menu& menu, std::string_view id);
    [[nodiscard]] std::string caption(std::string_view key);
    [[nodiscard]] bool write_report() const;
};

bool FrontEndMode::State::write_report() const {
    if (options.report_path.empty()) return true;
    std::error_code error;
    const auto parent = options.report_path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    std::ofstream output;
    if (!error) output.open(options.report_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        UtilityFunctions::printerr("EAWR front end report output could not be opened");
        return false;
    }
    const auto json = eawr::viewer::camera_input::json_string_literal;
    const std::string& report_failure = failure.empty() ? attract_failure : failure;
    output << std::boolalpha
        << "{\n  \"schema_version\": 1,\n  \"status\": " << json(report_failure.empty() ? "ok" : "failed")
        << ",\n  \"failure\": " << json(report_failure)
        << ",\n  \"front_end_ready\": " << front_end_ready
        << ",\n  \"current_screen\": " << json(model::to_string(nav.current()))
        << ",\n  \"attract_started\": " << attract_started
        << ",\n  \"attract_failed\": " << !attract_failure.empty()
        << ",\n  \"attract_failure\": " << json(attract_failure)
        << ",\n  \"music_playing\": " << music_playing
        << ",\n  \"frames\": " << frames
        << ",\n  \"exit_reason\": " << json(!failure.empty() ? "startup_failed" :
            !exit_reason.empty() ? exit_reason : "engine_shutdown")
        << ",\n  \"problems\": [";
    bool comma = false;
    const auto problem = [&](const std::string& value) {
        if (comma) output << ", ";
        output << json(value);
        comma = true;
    };
    for (const auto& note : notes) problem(note);
    if (audio) for (const auto& note : audio->problems()) problem(note);
    if (!attract_failure.empty()) problem(attract_failure);
    if (!failure.empty()) problem(failure);
    output << "]\n}\n";
    output.close();
    if (!output) {
        UtilityFunctions::printerr("EAWR front end report output could not be written");
        return false;
    }
    return true;
}

std::string FrontEndMode::State::caption(const std::string_view key) {
    return text_lookup ? data::ui::to_utf8(text_lookup->text(key)) : std::string(key);
}

// MAT-14: the menus, the settings screen and the staging screens keep the attract running; a start
// asked for on the staging screen ends it before the battle begins (MAT-13), and a staging screen
// the battle has hidden does not have it.
bool FrontEndMode::State::attract_wanted() const {
    if (!options.attract || campaign_started) return false;
    const auto screen = nav.current();
    if (over_backdrop(screen)) return true;
    return setup != nullptr && setup->visible() && !setup->start_pending();
}

void FrontEndMode::State::stop_attract(const char* reason) {
    if (attract && attract->running()) UtilityFunctions::print("EAWR front end attract stopped: ", reason);
    if (attract) attract->stop();
    attract_tried = false;
    if (backdrop != nullptr) backdrop->set_color(Color(0.0F, 0.0F, 0.0F));
    if (setup != nullptr) setup->scene_behind(false);
}

void FrontEndMode::State::sync_attract() {
    if (!attract_wanted()) {
        if (attract && (attract->running() || attract_tried))
            stop_attract(setup != nullptr && setup->start_pending() ? "game start" : "battle");
        return;
    }
    if (!attract) {
        AttractRunner::Options runner = AttractRunner::from_command_line();
        runner.game_root = options.game_root;
        runner.mod_root = options.mod_root;
        runner.profile = options.profile;
        runner.content = setup->content();
        runner.shaders = setup->shaders();
        // Only the front end's own flags apply; the standalone run's script and report do not.
        runner.report_path.clear();
        runner.capture_path.clear();
        runner.frames = 0;
        runner.steps.clear();
        attract = std::make_unique<AttractRunner>(std::move(runner));
    }
    if (!attract_tried) {
        attract_tried = true;
        if (attract->start(*host)) {
            attract_started = true;
            UtilityFunctions::print("EAWR front end attract started");
        }
        else if (!attract->failure().empty())
            UtilityFunctions::printerr("EAWR front end attract: ", text(attract->failure()));
    }
    if (attract->running()) {
        const bool focused = DisplayServer::get_singleton()->window_is_focused();
        // The menu is always a dialog for the attract: its keys are not the attract's (MAT-07).
        const bool main_menu = nav.current() == model::FrontEndScreen::main_menu;
        attract->set_front_end(true, main_menu);
        attract->process(host->get_process_delta_time(), true, main_menu, focused);
        // The battle shows through the overlay (MAT-01) and behind the single-player staging; the
        // multiplayer menus and the LAN staging hide it (MAT-14, owner 10-12).
        const model::FrontEndScreen screen = nav.current();
        const bool multiplayer = screen == model::FrontEndScreen::multiplayer;
        backdrop->set_color(Color(0.0F, 0.0F, 0.0F, multiplayer ? 1.0F : 0.0F));
        setup->scene_behind(!multiplayer && screen != model::FrontEndScreen::lan_setup && screen != model::FrontEndScreen::online_setup);
    }
    if (!attract->failure().empty()) attract_failure = attract->failure();
}

void FrontEndMode::State::log_attract(const char* label) {
    if (!attract || !attract->running()) {
        UtilityFunctions::print("EAWR front end attract ", label, ": running false");
        return;
    }
    const auto status = attract->status();
    UtilityFunctions::print("EAWR front end attract ", label, ": running true; units ", static_cast<int64_t>(status.units),
                            "; hud ", status.hud, "; input ", status.battle_input, "; camera_updates ",
                            static_cast<int64_t>(status.camera_updates), "; eye ", status.camera_eye[0], " ",
                            status.camera_eye[1], " ", status.camera_eye[2], "; first_eye ", status.camera_first_eye[0], " ",
                            status.camera_first_eye[1], " ", status.camera_first_eye[2]);
}

// MAT-14: what the staging screen's backing lets through, read from the rendered frame with the
// dialog's own controls hidden. Sampled on a grid; "differing" counts pixels off the flat backing.
void FrontEndMode::State::log_behind(const char* label) const {
    const auto image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null()) {
        UtilityFunctions::print("EAWR front end behind ", label, ": unreadable");
        return;
    }
    const Color flat(0.018F, 0.027F, 0.055F);
    std::set<std::uint32_t> distinct;
    std::int64_t sampled = 0;
    std::int64_t differing = 0;
    for (int32_t y = 0; y < image->get_height(); y += 8) {
        for (int32_t x = 0; x < image->get_width(); x += 8) {
            const Color pixel = image->get_pixel(x, y);
            ++sampled;
            const float gap = std::max({std::abs(pixel.r - flat.r), std::abs(pixel.g - flat.g), std::abs(pixel.b - flat.b)});
            if (gap * 255.0F > 6.0F) ++differing;
            distinct.insert(pixel.to_rgba32() >> 2U);
        }
    }
    UtilityFunctions::print("EAWR front end behind ", label, ": scene ", setup->scene_visible(), "; sampled ", sampled,
                            "; differing ", differing, "; distinct ", static_cast<int64_t>(distinct.size()));
}

bool FrontEndMode::State::build() {
    layer = memnew(CanvasLayer);
    layer->set_layer(9);
    host->add_child(layer);
    backdrop = memnew(ColorRect);
    backdrop->set_color(Color(0.0F, 0.0F, 0.0F));
    backdrop->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    layer->add_child(backdrop);
    overlay = memnew(TextureRect);
    overlay->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    overlay->set_stretch_mode(TextureRect::STRETCH_SCALE);
    overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    backdrop->add_child(overlay);
    root = memnew(Control);
    root->set_size(Vector2(canvas_width, canvas_height));
    root->set_theme(resources.theme);
    // The menus are laid out on a fixed canvas and scaled to the window by this node's transform; the
    // text is rasterised at the scale it is drawn at, not at the canvas size and stretched (#3176).
    root->set_oversampling_with_scale(CanvasItem::OVERSAMPLING_WITH_SCALE_ENABLED);
    backdrop->add_child(root);
    fade = memnew(ColorRect);
    fade->set_color(Color(0.0F, 0.0F, 0.0F, 0.0F));
    fade->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    layer->add_child(fade);

    load_overlay();
    for (const auto screen : menu_screens) build_menu(screen);
    if (menus[0].frame == nullptr) {
        failure = "the front end built no main menu";
        return false;
    }
    return true;
}

// FES-09: the backdrop is the overlay texture the game constants name for the language, drawn over
// the whole screen; the demo battle behind it is the AttractRunner's (sync_attract), and the
// backdrop turns transparent while it runs.
void FrontEndMode::State::load_overlay() {
    auto document = data::load_document(*resources.filesystem, "data/xml/gameconstants.xml");
    if (!document) {
        notes.push_back("game constants: " + core::format_diagnostic(document.error()) + "; no menu overlay");
        return;
    }
    std::string name;
    for (const data::XmlNode& node : document.value().root.children) {
        if (!same(node.name, "Localized_Menu_Overlay")) continue;
        const auto comma = node.raw_text.find(',');
        if (comma == std::string::npos) continue;
        if (same(trim(node.raw_text.substr(0, comma)), options.language)) name = trim(node.raw_text.substr(comma + 1));
    }
    if (name.empty()) {
        notes.push_back("game constants name no menu overlay for " + options.language);
        return;
    }
    // A texture reference is looked up as a .tga or a .dds of its stem under Data/Art/Textures.
    std::string stem = lower(name);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
    for (const char* extension : {".tga", ".dds"}) {
        auto texture = assets::load_texture(*resources.filesystem, "data/art/textures/" + stem + extension);
        if (!texture) continue;
        std::string problem;
        Ref<Image> image = texture_image(texture.value(), problem);
        if (image.is_valid()) {
            overlay->set_texture(ImageTexture::create_from_image(image));
            return;
        }
        notes.push_back("menu overlay " + name + ": " + problem);
    }
    notes.push_back("menu overlay " + name + " is not in the data");
}

void FrontEndMode::State::build_menu(const model::FrontEndScreen screen) {
    Menu& menu = menus[menu_index(screen)];
    menu.screen = screen;
    const std::string dialog_name(model::dialog_name(screen));
    const data::ui::Dialog* found = resources.dialogs->script.find(dialog_name);
    if (found == nullptr) {
        // A mod's malformed dialog is rejected on its own (EAWR-UI-0201) and the rest parse. The
        // menu cannot be left without its buttons, so a project-authored column stands in for it.
        notes.push_back(dialog_name + " is not in the catalogue (rejected or removed by the data); "
                        "using the project-authored fallback menu");
        build_fallback(menu, screen);
        return;
    }
    BuiltDialog built = build_dialog(*found, *resources.theme_model, text_lookup ? &*text_lookup : nullptr,
                                     model::Placement::upper_right);
    root->add_child(built.frame);
    built.frame->hide();
    menu.frame = built.frame;
    menu.from_data = true;
    for (const BuiltGadget& gadget : built.gadgets) {
        if (gadget.control == nullptr) continue;
        Entry entry;
        entry.id = gadget.id;
        entry.control = gadget.control;
        entry.button = Object::cast_to<BaseButton>(gadget.control);
        const model::GadgetFacts facts{gadget.kind == "button"};
        entry.rule = model::gadget_rule(screen, gadget.id, facts, front_end_build);
        if (entry.rule.kind == model::GadgetKind::hint) {
            menu.hint = Object::cast_to<ItemList>(gadget.control);
        }
        menu.entries.push_back(std::move(entry));
    }
    for (Entry& entry : menu.entries) apply_rule(entry, dialog_name);
}

void FrontEndMode::State::build_fallback(Menu& menu, const model::FrontEndScreen screen) {
    const FallbackMenu& spec = fallback_menu(screen);
    constexpr float width = 300.0F;
    constexpr float step_y = 36.0F;
    auto* frame = memnew(EawrUiFrame);
    const float height = 56.0F + step_y * static_cast<float>(spec.buttons.size());
    frame->set_size(Vector2(width, height));
    frame->set_position(Vector2(canvas_width - width, 260.0F));
    frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    frame->hide();
    root->add_child(frame);
    menu.frame = frame;
    auto* title = memnew(EawrUiLabel);
    title->set_role("L_Text");
    title->set_text(text(caption(spec.title_key)));
    title->set_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    title->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    title->set_position(Vector2(20, 14));
    title->set_size(Vector2(width - 40, 20));
    frame->add_child(title);
    float y = 46.0F;
    for (const FallbackButton& spec_button : spec.buttons) {
        auto* button = memnew(EawrUiButton);
        button->set_text(text(caption(spec_button.caption_key)));
        button->set_toggle_mode(true);
        button->set_position(Vector2(32, y));
        button->set_size(Vector2(width - 64, 24));
        frame->add_child(button);
        y += step_y;
        Entry entry;
        entry.id = spec_button.id;
        entry.control = button;
        entry.button = button;
        entry.rule = model::gadget_rule(screen, entry.id, {true}, front_end_build);
        menu.entries.push_back(std::move(entry));
    }
    for (Entry& entry : menu.entries) apply_rule(entry, std::string(model::dialog_name(screen)));
}

void FrontEndMode::State::apply_rule(Entry& entry, const std::string_view dialog) {
    switch (entry.rule.kind) {
    case model::GadgetKind::hidden:
        entry.control->hide();
        break;
    case model::GadgetKind::disabled:
        // FES-05: the retail button stays, disabled; the reason goes in the tooltip and the log.
        if (entry.button != nullptr) entry.button->set_disabled(true);
        entry.control->set_tooltip_text(text(entry.rule.reason));
        UtilityFunctions::print("EAWR front end: ", text(dialog), " ", text(entry.id), " disabled: ", text(entry.rule.reason));
        break;
    case model::GadgetKind::hint:
        entry.control->hide();
        break;
    default:
        break;
    }
    if (entry.button != nullptr && entry.rule.kind != model::GadgetKind::disabled) {
        entry.button->set_toggle_mode(true);
        entry.button->set_focus_mode(Control::FOCUS_NONE);
    }
}

void FrontEndMode::State::layout() {
    const Vector2 size = host->get_viewport()->get_visible_rect().size;
    backdrop->set_size(size);
    overlay->set_size(size);
    fade->set_size(size);
    static_cast<void>(fit_letterbox_canvas(*root, size, Vector2(canvas_width, canvas_height)));
}

void FrontEndMode::State::show_screen() {
    const model::FrontEndScreen screen = nav.current();
    if (shown && *shown == screen) return;
    const bool was_setup = shown && !over_backdrop(*shown);
    // MNU-34: the campaign staging screen closes when the shell leaves it (Back or Start).
    if (shown && *shown == model::FrontEndScreen::campaign_setup && campaign_screen) campaign_screen->close();
    shown = screen;
    UtilityFunctions::print("EAWR front end screen: ", text(model::to_string(screen)));
    for (Menu& menu : menus) {
        if (menu.frame != nullptr) menu.frame->set_visible(menu.screen == screen);
    }
    backdrop->set_visible(over_backdrop(screen));
    // FES-03, WGP-40: request the event on open, preserving an already playing track.
    if (audio && (is_menu(screen) || screen == model::FrontEndScreen::campaign_setup)) audio->start_music();
    if (was_setup) setup->leave();
    switch (screen) {
    case model::FrontEndScreen::skirmish_setup:
        setup->enter(false);
        break;
    case model::FrontEndScreen::lan_setup:
        setup->enter(true);
        break;
    case model::FrontEndScreen::online_setup:
        setup->enter_online(ActiveSettings::get() ? ActiveSettings::get()->network : SettingsStore{options.settings_file}.load().settings.network);
        break;
    case model::FrontEndScreen::single_player:
        refresh_single_player();
        break;
    case model::FrontEndScreen::campaign_setup:
        if (campaign_screen) campaign_screen->open();
        else static_cast<void>(nav.back());
        break;
    case model::FrontEndScreen::load_game: {
        LoadGameContext context;
        context.boxes = MessageBoxContext{resources.dialogs, resources.theme_model,
            text_lookup ? &*text_lookup : nullptr, resources.theme, host->get_viewport()->get_visible_rect().size};
        context.store = ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "replays";
        context.saves = std::make_shared<viewer::DirectorySavedGameStore>(save_directory());
        context.launcher = [this](const model::SavedGameRequest& request) { return restore_saved(request); };
        load_screen = memnew(EawrLoadGameScreen);
        load_id = static_cast<int64_t>(load_screen->get_instance_id());
        if (!load_screen->open(*host, context)) {
            UtilityFunctions::printerr("EAWR front end load screen: ", text(load_screen->failure()));
            memdelete(load_screen);
            load_screen = nullptr;
            static_cast<void>(nav.back());
        }
        break;
    }
    case model::FrontEndScreen::network_options:
    case model::FrontEndScreen::options: {
        if (options_screen == nullptr) {
            options_screen = memnew(EawrOptionsScreen);
            options_id = static_cast<int64_t>(options_screen->get_instance_id());
            EawrOptionsScreen::Options screen_options;
            // The content this load already mounted, so the screen reads the same files.
            if (const auto content = setup->content()) screen_options.filesystem = content->filesystem;
            screen_options.game_root = options.game_root;
            screen_options.mod_root = options.mod_root;
            screen_options.context = EawrOptionsScreen::Context::main_menu;
            screen_options.settings_file = options.settings_file;
            // A pinned capture window is not resized by a saved video setting.
            screen_options.apply_window = !options.test && options.capture_path.empty();
            if (!options_screen->build(std::move(screen_options))) {
                UtilityFunctions::printerr("EAWR front end: the settings screen could not be built: ",
                                           text(options_screen->failure()));
                memdelete(options_screen);
                options_screen = nullptr;
                static_cast<void>(nav.back());
                break;
            }
        }
        options_screen->show(*host, screen == model::FrontEndScreen::network_options ? EawrOptionsScreen::Page::network : EawrOptionsScreen::Page::hub);
        options_open = true;
        break;
    }
    default:
        break;
    }
}

std::filesystem::path FrontEndMode::State::save_directory() const {
    return options.save_directory.empty()
        ? ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "saves" : options.save_directory;
}

// The demo battle ends and the menus leave the frame once a campaign owns it.
void FrontEndMode::State::begin_campaign() {
    campaign_started = true;
    backdrop->hide();
    overlay->hide();
    fade->hide();
    if (audio) audio->set_music(false);
}

// MNU-39: Load hands a saved game to the host's launcher; an empty answer accepts it.
std::string FrontEndMode::State::restore_saved(const model::SavedGameRequest& request) {
    stop_attract("saved game restore");
    campaign_launching = true;
    const std::string refusal = options.saved_game_launcher
        ? options.saved_game_launcher(request) : "The saved game launcher is unavailable.";
    campaign_launching = false;
    if (refusal.empty()) begin_campaign();
    else UtilityFunctions::print("EAWR front end saved game restore refused: ", text(refusal));
    return refusal;
}

// MNU-34, WGP-29: the demo battle stops, then the host starts the chosen campaign.
void FrontEndMode::State::start_campaign(const model::CampaignStartRequest& request) {
    stop_attract("campaign start");
    campaign_launching = true;
    const std::string refusal = options.campaign_launcher
        ? options.campaign_launcher(request) : "The campaign launcher is unavailable.";
    campaign_launching = false;
    if (refusal.empty()) {
        begin_campaign();
        UtilityFunctions::print("EAWR front end campaign started: ", text(request.campaign), "; faction ",
                                text(request.faction), "; difficulty ", text(request.difficulty));
        return;
    }
    UtilityFunctions::print("EAWR front end campaign refused: ", text(refusal));
    show_notice("TEXT_BUTTON_CUSTOM_CAMPAIGN", refusal);
}

bool FrontEndMode::State::notice_open() const {
    return notice_box != nullptr && UtilityFunctions::is_instance_id_valid(notice_id);
}

void FrontEndMode::State::show_notice(const char* caption_key, const std::string& reason) {
    if (notice_open()) return;
    MessageBoxContext boxes{resources.dialogs, resources.theme_model, text_lookup ? &*text_lookup : nullptr,
                            resources.theme, host->get_viewport()->get_visible_rect().size};
    model::MessageBoxRequest request;
    request.caption = caption_key;
    request.lines[0] = reason;
    request.literal_lines[0] = true;
    request.buttons = {model::BoxButton{"TEXT_BUTTON_OK", 0, true}};
    notice_box = memnew(EawrMessageBox);
    if (!notice_box->open(*host, boxes, request)) {
        UtilityFunctions::printerr("EAWR front end notice: ", text(notice_box->failure()));
        memdelete(notice_box);
        notice_box = nullptr;
        return;
    }
    notice_id = static_cast<int64_t>(notice_box->get_instance_id());
}

// MNU-34: the data's campaigns, read once the menus exist. A failure keeps the entry off with the
// reason, and the rest of the menus work.
void FrontEndMode::State::build_campaign_screen() {
    auto screen = std::make_unique<CampaignSetupScreen>();
    CampaignSetupScreen::Resources inputs;
    inputs.filesystem = resources.filesystem;
    inputs.catalog = resources.catalog;
    inputs.dialogs = resources.dialogs;
    inputs.theme_model = resources.theme_model;
    inputs.text = text_lookup ? &*text_lookup : nullptr;
    // WGP-190: the host applies accepted technology options; difficulty remains authored.
    const model::CampaignHostSupport support{false, true};
    if (!screen->build(*root, inputs, support)) {
        campaign_problem = screen->failure();
        notes.push_back("campaign staging: " + campaign_problem);
        return;
    }
    campaign_screen = std::move(screen);
}

// FES-13: Custom Campaign is off while the data lists no campaign.
void FrontEndMode::State::refresh_single_player() {
    Menu& menu = menus[menu_index(model::FrontEndScreen::single_player)];
    const auto disable = [&](Entry& entry, const std::string& reason) {
        if (entry.button == nullptr || entry.rule.kind == model::GadgetKind::hidden) return;
        entry.button->set_disabled(!reason.empty());
        entry.control->set_tooltip_text(text(reason));
        if (!reason.empty()) UtilityFunctions::print("EAWR front end: ", text(entry.id), " disabled: ", text(reason));
    };
    if (Entry* custom = find_entry(menu, "IDC_CUSTOM_CAMPAIGN_BUTTON"); custom != nullptr) {
        disable(*custom, model::campaign_entry_disabled_reason(campaign_screen ? campaign_screen->listed_rows() : 0,
                                                               campaign_problem));
    }
}

std::optional<int> FrontEndMode::State::poll_campaign() {
    if (!campaign_screen) return std::nullopt;
    switch (campaign_screen->poll()) {
    case CampaignSetupScreen::Action::back:
        if (audio) audio->play(press_event);
        static_cast<void>(nav.back());
        break;
    case CampaignSetupScreen::Action::start:
        if (audio) audio->play(press_event);
        if (const auto request = campaign_screen->take_start()) start_campaign(*request);
        break;
    case CampaignSetupScreen::Action::none: break;
    }
    return std::nullopt;
}

void FrontEndMode::State::update_hint(Menu& menu, const Entry& entry) {
    if (menu.hint == nullptr) return;
    // FES-06: the hint list shows the Hint_ caption of the gadget under the pointer, or a blank line.
    std::string hint = " ";
    if (text_database) {
        if (const auto* found = text_database->find(model::hint_key(entry.id))) hint = data::ui::to_utf8(found->value);
    }
    menu.hint->clear();
    const double width = menu.hint->get_size().x / 7.0;
    for (const std::string& line : model::loading_wrap_lines(
             hint, std::max(8.0, width), [](const std::string_view value) { return static_cast<double>(value.size()); })) {
        menu.hint->add_item(text(line), Ref<Texture2D>(), false);
    }
    menu.hint->show();
}

std::optional<int> FrontEndMode::State::step(const model::Step result) {
    if (result == model::Step::quit) {
        exit_reason = "clean_quit";
        // MAT-13: Exit or Escape on the main menu stops the attract before the viewer ends.
        stop_attract("exit");
        UtilityFunctions::print("EAWR front end quit");
        return 0;
    }
    return std::nullopt;
}

std::optional<int> FrontEndMode::State::poll_menu(Menu& menu) {
    std::optional<model::GadgetRule> pressed;
    for (Entry& entry : menu.entries) {
        if (entry.button == nullptr || !entry.control->is_visible() || entry.rule.kind == model::GadgetKind::hidden) continue;
        const bool hovered = entry.button->is_hovered();
        if (hovered && !entry.hovered && !entry.button->is_disabled()) {
            if (audio) audio->play(hover_event);
            update_hint(menu, entry);
        }
        entry.hovered = hovered;
        if (!entry.button->is_pressed()) continue;
        entry.button->set_pressed_no_signal(false);
        if (entry.button->is_disabled() || pressed) continue;
        if (audio) audio->play(press_event);
        pressed = entry.rule;
    }
    if (pressed) {
        if (const auto result = step(model::press(nav, *pressed)); result) return result;
    }
    return std::nullopt;
}

Entry* FrontEndMode::State::find_entry(Menu& menu, const std::string_view id) {
    for (Entry& entry : menu.entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

void FrontEndMode::State::log_menu(const char* label, const Menu& menu) const {
    std::string line;
    for (const Entry& entry : menu.entries) {
        if (entry.button == nullptr) continue;
        const bool visible = entry.control->is_visible();
        line += " " + entry.id + "=" + (!visible ? "hidden" : entry.button->is_disabled() ? "disabled" : "enabled");
    }
    UtilityFunctions::print("EAWR front end menu ", label, ":", text(line));
}

// The GPU contract's text-sharpness probe: where one caption's control landed on the screen and at
// what scale the menu is drawn, so a test can crop the capture around it (#3176).
void FrontEndMode::State::log_text_scale(const char* label, const Control* control, const std::string_view id) const {
    if (control == nullptr) return;
    const Vector2 size = host->get_viewport()->get_visible_rect().size;
    const Rect2 rect = control->get_global_rect();
    UtilityFunctions::print("EAWR front end text scale: ", label, "; viewport ", static_cast<int64_t>(std::lround(size.x)),
                            "x", static_cast<int64_t>(std::lround(size.y)), "; scale ", root->get_scale().x, "; ",
                            text(id), " rect ", static_cast<int64_t>(std::lround(rect.position.x)), " ",
                            static_cast<int64_t>(std::lround(rect.position.y)), " ",
                            static_cast<int64_t>(std::lround(rect.size.x)), " ",
                            static_cast<int64_t>(std::lround(rect.size.y)));
}

void FrontEndMode::State::capture(const char* label) const {
    if (options.capture_path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(options.capture_path.parent_path(), error);
    const auto path = options.capture_path.parent_path()
        / ViewerPath{ViewerPath::utf8(options.capture_path.stem()) + "." + label + ".png"}.native();
    auto image = host->get_viewport()->get_texture()->get_image();
    if (error || image.is_null() || image->save_png(text(ViewerPath::utf8(path))) != OK)
        UtilityFunctions::printerr("EAWR front end capture failed: ", text(ViewerPath::utf8(path)));
}

// The GPU contract: real pointer events on the controls, the same handlers a player reaches.
std::optional<int> FrontEndMode::State::run_test() {
    if (options.battle_round_trip) return run_battle_test();
    if (!options.campaign_test.empty()) return run_campaign_test();
    const auto press_gadget = [&](model::FrontEndScreen screen, const char* id) {
        Menu& menu = menus[menu_index(screen)];
        if (Entry* entry = find_entry(menu, id); entry != nullptr && entry->button != nullptr)
            click(entry->button->get_global_rect().get_center());
        else UtilityFunctions::printerr("EAWR front end test: no gadget ", id);
    };
    using S = model::FrontEndScreen;
    if (options.online_routes_test) {
        switch (frames) {
        case 8: press_gadget(S::main_menu, "IDC_MAIN_MENU_MULTIPLAYER"); break;
        case 16: log_menu("multiplayer", menus[2]); press_gadget(S::multiplayer, "IDC_MULTIPLAYER_INTERNET_BUTTON"); break;
        case 24:
            if (auto* lobby = setup->online_lobby()) {
                UtilityFunctions::print("EAWR online service lobby source: ", text(lobby->snapshot().source));
                capture("online_service_lobby");
            }
            break;
        case 26: if (auto* lobby = setup->online_lobby()) static_cast<void>(lobby->run_action("cancel")); break;
        case 34: press_gadget(S::multiplayer, "IDC_MAIN_OPTIONS_NETWORK"); break;
        case 42:
            if (live_options()) UtilityFunctions::print("EAWR online service network page: ", EawrOptionsScreen::page_name(options_screen->page()));
            capture("online_service_network"); break;
        case 44: if (live_options()) static_cast<void>(options_screen->press("IDC_BACK")); break;
        case 52: press_gadget(S::multiplayer, "IDC_OK_MAIN"); break;
        case 60: press_gadget(S::main_menu, "IDC_OPTIONS"); break;
        case 68: if (live_options()) static_cast<void>(options_screen->press("IDC_MAIN_OPTIONS_NETWORK")); break;
        case 76:
            if (live_options()) UtilityFunctions::print("EAWR online service options page: ", EawrOptionsScreen::page_name(options_screen->page()));
            break;
        case 78: if (live_options()) static_cast<void>(options_screen->press("IDC_BACK")); break;
        case 82: if (live_options()) static_cast<void>(options_screen->press("IDC_OK_MAIN")); break;
        case 90: press_gadget(S::main_menu, "IDC_MAIN_MENU_EXIT2"); break;
        default: break;
        }
        return std::nullopt;
    }
    switch (frames) {
    case 8:
        log_menu("main_menu", menus[0]);
        for (const Entry& entry : menus[0].entries) {
            if (entry.button == nullptr || !entry.control->is_visible_in_tree() || entry.button->is_disabled()) continue;
            log_text_scale("main_menu", entry.button, entry.id);
            break;
        }
        capture("main_menu");
        break;
    case 9: log_attract("main_menu"); break;
    case 10: press_gadget(S::main_menu, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"); break;
    case 16: log_menu("single_player", menus[1]); log_attract("single_player"); capture("single_player"); break;
    case 18: press_gadget(S::single_player, "IDC_SKIRMISH_GAMES_BUTTON"); break;
    case 30:
        UtilityFunctions::print("EAWR front end setup: visible ", setup->visible(), "; lan ", setup->lan_selected());
        log_text_scale("skirmish_setup", setup->start_button(), "start");
        capture("skirmish_setup");
        log_attract("skirmish_setup");
        if (options.start_battle) {
            // The Start button, as a click on it would press it; process() ends the run once the
            // attract has stopped for it.
            if (auto* start = setup->start_button()) click(start->get_global_rect().get_center());
        }
        break;
    case 31:
        // #3168: the frame behind the staging's controls shows the battle (read at the next frame).
        if (!options.start_battle) setup->set_content_visible(false);
        break;
    case 32:
        if (!options.start_battle) {
            log_behind("skirmish_setup");
            setup->set_content_visible(true);
        }
        break;
    case 34:
        if (auto* back = setup->back_button()) click(back->get_global_rect().get_center());
        break;
    case 40: UtilityFunctions::print("EAWR front end back: ", text(model::to_string(nav.current()))); break;
    case 42: press_gadget(S::single_player, "IDC_BACK_BUTTON_MAIN"); break;
    case 48: UtilityFunctions::print("EAWR front end back: ", text(model::to_string(nav.current()))); break;
    case 50: press_gadget(S::main_menu, "IDC_MAIN_MENU_MULTIPLAYER"); break;
    case 56: log_menu("multiplayer", menus[2]); press_gadget(S::multiplayer, "IDC_MULTIPLAYER_LAN_BUTTON"); break;
    case 70:
        UtilityFunctions::print("EAWR front end setup: visible ", setup->visible(), "; lan ", setup->lan_selected());
        log_text_scale("lan_setup", setup->lan_host_button(), "lan_host");
        capture("lan_setup");
        setup->set_content_visible(false);
        break;
    case 71:
        // The LAN connection is a multiplayer menu: its backing hides the battle.
        log_behind("lan_setup");
        setup->set_content_visible(true);
        break;
    case 74:
        if (auto* back = setup->back_button()) click(back->get_global_rect().get_center());
        break;
    case 80: press_gadget(S::multiplayer, "IDC_OK_MAIN"); break;
    case 86: UtilityFunctions::print("EAWR front end back: ", text(model::to_string(nav.current()))); break;
    case 88: press_gadget(S::main_menu, "IDC_OPTIONS"); break;
    case 96:
        log_attract("options");
        UtilityFunctions::print("EAWR front end screen check: ", text(model::to_string(nav.current())));
        capture("options");
        if (options.quit_with_options_open) {
            // The shutdown repro: quit while the hub is still a child of the host.
            UtilityFunctions::print("EAWR front end quit with the settings screen open");
            return 0;
        }
        // The hub's own Back button, as a click on it would press it.
        if (live_options() == nullptr || !options_screen->press("IDC_OK_MAIN"))
            UtilityFunctions::printerr("EAWR front end test: the settings hub has no Back button");
        break;
    case 104: UtilityFunctions::print("EAWR front end back: ", text(model::to_string(nav.current()))); break;
    case 106: press_gadget(S::main_menu, "IDC_LOAD_GAME_MAIN"); break;
    case 114:
        if (auto* cancel = load_screen ? load_screen->gadget("IDC_LOAD_CANCEL_BUTTON") : nullptr)
            click(cancel->get_global_rect().get_center());
        else UtilityFunctions::printerr("EAWR front end test: no Load Game Cancel button");
        break;
    case 122: press_gadget(S::main_menu, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"); break;
    case 130:
        // Mods may omit the custom-campaign entry; stock must exercise its open/Back path.
        if (Entry* entry = find_entry(menus[1], "IDC_CUSTOM_CAMPAIGN_BUTTON");
            entry != nullptr && entry->button != nullptr && !entry->button->is_disabled())
            press_gadget(S::single_player, "IDC_CUSTOM_CAMPAIGN_BUTTON");
        break;
    case 138:
        if (nav.current() == S::campaign_setup) {
            if (auto* cancel = campaign_screen ? campaign_screen->gadget("IDC_CANCEL_BUTTON") : nullptr)
                click(cancel->get_global_rect().get_center());
            else UtilityFunctions::printerr("EAWR front end test: no campaign Back button");
        }
        break;
    case 146: press_gadget(S::single_player, "IDC_BACK_BUTTON_MAIN"); break;
    case 154: press_gadget(S::main_menu, "IDC_MAIN_MENU_EXIT2"); break;
    default: break;
    }
    return std::nullopt;
}

// MNU-34: the single-player menu to the campaign staging screen, a campaign row, Start. The campaign
// itself is the galactic mode's; this only watches the way in.
std::optional<int> FrontEndMode::State::run_campaign_test() {
    using S = model::FrontEndScreen;
    if (returned_to_menu) return std::nullopt; // the campaign was left from its menu; the driver ends the run
    const auto press_gadget = [&](model::FrontEndScreen screen, const char* id) {
        Menu& menu = menus[menu_index(screen)];
        if (Entry* entry = find_entry(menu, id); entry != nullptr && entry->button != nullptr)
            click(entry->button->get_global_rect().get_center());
        else UtilityFunctions::printerr("EAWR front end test: no gadget ", id);
    };
    const auto press_control = [&](const char* id) {
        if (auto* button = Object::cast_to<BaseButton>(campaign_screen ? campaign_screen->gadget(id) : nullptr))
            click(button->get_global_rect().get_center());
        else UtilityFunctions::printerr("EAWR front end test: no campaign gadget ", id);
    };
    const auto summary = [&](const char* label) {
        UtilityFunctions::print("EAWR front end campaign ", label, ": ",
                                text(campaign_screen ? campaign_screen->summary() : std::string("unavailable")));
    };
    const bool tech_five_test = options.campaign_test.starts_with("tech5:");
    const bool tech_test = tech_five_test || options.campaign_test.starts_with("tech:");
    const model::CampaignFacts* target{};
    if (tech_test && campaign_screen && campaign_screen->model()) {
        const auto wanted = std::string_view(options.campaign_test).substr(tech_five_test ? 6 : 5);
        for (const auto& entry : campaign_screen->model()->campaigns()) if (entry.name == wanted) target = &entry;
        if (!target) return 2;
    }
    const auto tech_plus = [&]() {
        if (auto* slider = campaign_screen ? campaign_screen->gadget("IDC_SKIRMISH_STARTING_TECH_LEVEL_SLIDER") : nullptr) {
            const auto rect = slider->get_global_rect();
            click(rect.position + Vector2(rect.size.x - 4.0F, rect.size.y * 0.5F));
        }
    };
    switch (frames) {
    case 8: press_gadget(S::main_menu, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"); break;
    case 16:
        log_menu("single_player", menus[1]);
        capture("single_player");
        press_gadget(S::single_player, "IDC_CUSTOM_CAMPAIGN_BUTTON");
        break;
    case 24: {
        UtilityFunctions::print("EAWR front end screen check: ", text(model::to_string(nav.current())));
        summary("opened");
        capture("campaign_setup");
        // The first listed row, as a click on it would pick it.
        auto* list = Object::cast_to<ItemList>(campaign_screen ? campaign_screen->gadget("IDC_STAGING_MAP_LIST") : nullptr);
        if (list != nullptr && list->get_item_count() > 0) {
            int32_t choice{};
            if (target) {
                const auto* facts = campaign_screen->model();
                bool found{};
                for (std::size_t i = 0; i < facts->rows().size(); ++i)
                    if (facts->campaigns()[facts->rows()[i].campaign].campaign_set == target->campaign_set) {
                        choice = static_cast<int32_t>(i); found = true;
                    }
                if (!found) return 2;
                list->select(choice); list->ensure_current_is_visible();
            }
            const Rect2 item = list->get_item_rect(choice, true);
            click(list->get_global_position() + item.get_center());
        } else {
            UtilityFunctions::printerr("EAWR front end test: the campaign list has no row");
        }
        break;
    }
    case 28:
        if (tech_test) {
            if (auto* combo = Object::cast_to<OptionButton>(campaign_screen->gadget("IDC_FACTION_COMBO"))) {
                const auto& choices = campaign_screen->model()->factions();
                bool found{};
                for (std::size_t i = 0; i < choices.size(); ++i)
                    if (target && choices[i].name == target->faction) {
                        combo->select(static_cast<int32_t>(i)); found = true;
                    }
                if (!found) return 2;
            }
        }
        break;
    case 32:
        summary("selected");
        capture("campaign_selected");
        press_control(tech_test ? "IDC_SELECT_OPTIONS" : "IDC_PLAY_BUTTON");
        break;
    case 34:
        if (tech_test) {
            if (auto* slider = campaign_screen->gadget("IDC_SKIRMISH_MAX_TECH_LEVEL_SLIDER")) {
                const auto rect = slider->get_global_rect();
                click(rect.position + Vector2(rect.size.x - 4.0F, rect.size.y * 0.5F));
            }
        }
        break;
    case 36:
        if (tech_test) {
            // The prior frame's ordinary screen poll commits the endpoint gesture.
            UtilityFunctions::print("EAWR campaign unchanged technology edit: ",
                campaign_screen->model()->tech_options().custom ? 1 : 0);
            tech_plus();
        }
        break;
    case 40:
        if (tech_test) { tech_plus(); break; }
        // The campaign did not start (a refusal opened a notice, or Start stayed off).
        if (!campaign_started) {
            UtilityFunctions::print("EAWR front end campaign not started");
            return 0;
        }
        break;
    case 44:
        if (tech_test) {
            const auto values = campaign_screen->model()->tech_options();
            UtilityFunctions::print("EAWR campaign technology options: start=", values.starting,
                " max=", values.maximum, " custom=", values.custom ? 1 : 0);
            capture("campaign_technology");
        }
        break;
    case 42:
        // The right-end button increments one level per click: the third
        // starting-slider gesture takes the stock authored display 2 to 5.
        if (tech_five_test) tech_plus();
        break;
    case 48:
        if (tech_test) press_control("IDC_BUTTON_ACCEPT");
        break;
    case 52:
        if (tech_test) press_control("IDC_PLAY_BUTTON");
        break;
    case 64:
        if (tech_test && !campaign_started) return 2;
        break;
    default: break;
    }
    return std::nullopt;
}

// MNU-45: skirmish setup -> battle -> the end dialog's Continue -> the setup screen again -> Back to
// the main menu -> quit. The battle itself is driven by the live-session flags of the same command
// line (a station damage order, the Begin and Continue gestures); this only watches the way back.
std::optional<int> FrontEndMode::State::run_battle_test() {
    const auto press_gadget = [&](model::FrontEndScreen screen, const char* id) {
        Menu& menu = menus[menu_index(screen)];
        if (Entry* entry = find_entry(menu, id); entry != nullptr && entry->button != nullptr)
            click(entry->button->get_global_rect().get_center());
        else UtilityFunctions::printerr("EAWR front end test: no gadget ", id);
    };
    using S = model::FrontEndScreen;
    if (!battle_started) {
        switch (frames) {
        case 8: press_gadget(S::main_menu, "IDC_MAIN_MENU_SINGLE_PLAYER_GAMES"); break;
        case 16: press_gadget(S::single_player, "IDC_SKIRMISH_GAMES_BUTTON"); break;
        case 40:
            if (auto* start = setup->start_button(); start != nullptr && !start->is_disabled()) {
                UtilityFunctions::print("EAWR front end battle: starting from ", text(model::to_string(nav.current())));
                battle_started = true;
                click(start->get_global_rect().get_center());
            } else {
                UtilityFunctions::printerr("EAWR front end test: the setup screen has no enabled Start button");
                return 2;
            }
            break;
        default: break;
        }
        return std::nullopt;
    }
    // The shell is also processed while the battle loads behind the hidden setup screen; count
    // only the frames since the setup screen came back.
    if (frames_after_battle == 0 && !setup->visible() && !returned_to_menu) {
        battle_left_setup = true;
        return std::nullopt;
    }
    if (!battle_left_setup) return std::nullopt;
    ++frames_after_battle;
    switch (frames_after_battle) {
    case 6:
        UtilityFunctions::print("EAWR front end battle returned: ", text(model::to_string(nav.current())),
                                "; setup visible ", setup->visible());
        capture("after_battle");
        break;
    case 20:
        UtilityFunctions::print("EAWR front end battle music: ",
                                audio && audio->music_playing() ? "playing " : "silent ", text(audio ? audio->music_file() : std::string{}));
        // A battle left through the in-game menu's Quit Game came back to the main menu: no setup screen to leave.
        if (auto* back = setup->back_button(); back != nullptr && setup->visible()) click(back->get_global_rect().get_center());
        break;
    case 30: UtilityFunctions::print("EAWR front end back: ", text(model::to_string(nav.current()))); break;
    case 32:
        if (nav.current() == S::single_player) press_gadget(S::single_player, "IDC_BACK_BUTTON_MAIN");
        break;
    case 40: press_gadget(S::main_menu, "IDC_MAIN_MENU_EXIT2"); break;
    default: break;
    }
    return std::nullopt;
}

FrontEndMode::Options FrontEndMode::from_command_line(const std::filesystem::path& capture_path,
                                                      const std::filesystem::path& game_root,
                                                      const std::filesystem::path& mod_root,
                                                      const std::string& profile) {
    Options options;
    options.capture_path = capture_path;
    options.game_root = game_root;
    options.mod_root = mod_root;
    options.profile = profile;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t i = 0; i < arguments.size(); ++i) {
        const std::string argument = utf8(arguments[i]);
        if (argument == "--eawr-report" && i + 1 < arguments.size())
            options.report_path = ViewerPath{utf8(arguments[i + 1])}.native();
        if ((argument == "--eawr-save-dir" || argument == "--eawr-galactic-save-dir") && i + 1 < arguments.size())
            options.save_directory = ViewerPath{utf8(arguments[i + 1])}.native();
        if (argument == "--eawr-front-end-test") options.test = true;
        if (argument == "--eawr-front-end-test-online") { options.test = true; options.online_routes_test = true; }
        if (argument == "--eawr-settings-file" && i + 1 < arguments.size()) options.settings_file = ViewerPath{utf8(arguments[i + 1])}.native();
        if (argument == "--eawr-front-end-test-quit-open") options.quit_with_options_open = true;
        if (argument == "--eawr-front-end-test-battle") options.battle_round_trip = true;
        if (argument == "--eawr-front-end-test-start") options.start_battle = true;
        if (argument == "--eawr-front-end-test-campaign" && i + 1 < arguments.size())
            options.campaign_test = utf8(arguments[i + 1]);
        if (argument == "--eawr-attract" && i + 1 < arguments.size() && utf8(arguments[i + 1]) == "off") options.attract = false;
        if (argument == "--eawr-audio" && i + 1 < arguments.size() && utf8(arguments[i + 1]) == "off") options.audio = false;
    }
    return options;
}

FrontEndMode::FrontEndMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
FrontEndMode::~FrontEndMode() {
    // The attract's battle is a child of the host; stop() checks the instance ids before it frees.
    if (state_ && state_->attract) state_->attract->stop();
    // A closed settings screen is out of the tree and nobody else frees it.
    // An open one is a child of the host, which frees it; a screen the engine already freed is skipped.
    if (state_ && state_->live_options() != nullptr && state_->options_screen->get_parent() == nullptr)
        memdelete(state_->options_screen);
}
const std::string& FrontEndMode::failure() const { return state_->failure; }

bool FrontEndMode::close() {
    State& state = *state_;
    if (state.campaign_started || state.campaign_launching) return true;
    if (!state.report_written) {
        state.report_ok = state.write_report();
        state.report_written = true;
    }
    return state.report_ok;
}

bool FrontEndMode::ready(Node3D& host, SkirmishSetupMode& setup) {
    State& state = *state_;
    state.host = &host;
    state.setup = &setup;
    state.resources = setup.ui_resources();
    if (state.resources.filesystem == nullptr || state.resources.dialogs == nullptr
        || state.resources.theme_model == nullptr || !state.resources.theme.is_valid() || state.resources.catalog == nullptr) {
        state.failure = "the front end needs the setup screen's data load";
        return false;
    }
    if (auto loaded = data::ui::load_language_text_database(*state.resources.filesystem, state.options.language)) {
        state.text_database.emplace(std::move(loaded.value()));
        state.text_lookup.emplace(*state.text_database);
    } else {
        state.notes.push_back("text database: " + core::format_diagnostic(loaded.error()) + "; captions show their keys");
    }
    if (!state.build()) return false;
    state.build_campaign_screen();
    setup.enable_back(state.caption("TEXT_BUTTON_BACK"));
    if (state.options.audio) {
        state.audio = std::make_unique<MenuAudio>(host, *state.resources.filesystem, *state.resources.catalog);
    }
    for (const std::string& note : state.notes) UtilityFunctions::print("EAWR front end note: ", text(note));
    if (state.audio) {
        for (const std::string& problem : state.audio->problems())
            UtilityFunctions::print("EAWR front end audio note: ", text(problem));
    }
    // FES-09: the first showing fades up from black over two seconds (not in a scripted run).
    if (!state.options.test) {
        state.fading = true;
        state.fade_started_ms = Time::get_singleton()->get_ticks_msec();
        state.fade->set_color(Color(0.0F, 0.0F, 0.0F, 1.0F));
    }
    state.layout();
    state.show_screen();
    state.front_end_ready = true;
    state.music_playing = state.audio && state.audio->music_playing();
    UtilityFunctions::print("EAWR front end ready");
    return true;
}

void FrontEndMode::input(const Ref<InputEvent>& event) {
    State& state = *state_;
    if (state.attract && state.attract->running()) state.attract->input(event);
}

void FrontEndMode::window_activated() {
    State& state = *state_;
    if (state.attract && state.attract->running()) state.attract->window_activated();
}

void FrontEndMode::return_to_main_menu() {
    State& state = *state_;
    state.campaign_started = false;
    state.campaign_launching = false;
    state.returned_to_menu = true;
    // begin_campaign() hid the shell's art; show_screen() draws the menu on the next frame and
    // closes whatever screen started the game (the setup screen, the campaign staging screen).
    if (state.overlay != nullptr) state.overlay->show();
    state.nav.reset();
    // A shell that never left the main menu still redraws it: the art was hidden for the game.
    if (state.shown && *state.shown == model::FrontEndScreen::main_menu) state.shown.reset();
    // The Escape that closed the in-game menu is not a press on the main menu (which quits).
    state.escape_was_down = true;
    UtilityFunctions::print("EAWR front end returned to the main menu");
}

bool FrontEndMode::owns_frame() const {
    return over_backdrop(state_->nav.current());
}

std::optional<int> FrontEndMode::process() {
    State& state = *state_;
    state.layout();
    if (state.fading) {
        const double elapsed = (Time::get_singleton()->get_ticks_msec() - state.fade_started_ms) / 1000.0;
        const double alpha = std::clamp(1.0 - elapsed / fade_up_seconds, 0.0, 1.0);
        state.fade->set_color(Color(0.0F, 0.0F, 0.0F, static_cast<float>(alpha)));
        if (alpha <= 0.0) { state.fading = false; state.fade->hide(); }
    }
    ++state.frames;
    // Escape is read every frame, so a key that closed the settings screen is not a new press on the
    // main menu (which quits).
    const bool escape = Input::get_singleton()->is_physical_key_pressed(KEY_ESCAPE);
    bool escape_edge = escape && !state.escape_was_down;
    state.escape_was_down = escape;
    const auto screen_at_start = state.nav.current();
    if (state.load_screen != nullptr && !UtilityFunctions::is_instance_id_valid(state.load_id)) {
        state.load_screen = nullptr;
        if (!state.campaign_started && state.nav.current() == model::FrontEndScreen::load_game)
            static_cast<void>(state.nav.back());
    }
    if (state.options_open && state.nav.current() == model::FrontEndScreen::network_options && state.live_options()
        && state.options_screen->page() == EawrOptionsScreen::Page::hub) static_cast<void>(state.options_screen->press("IDC_OK_MAIN"));
    if (state.options_open && (state.live_options() == nullptr || !state.options_screen->shown())) {
        // The settings screen closed itself (its hub's Back, or Esc on the hub).
        state.options_open = false;
        if (state.nav.current() == model::FrontEndScreen::options || state.nav.current() == model::FrontEndScreen::network_options) static_cast<void>(state.nav.back());
    }
    // Back from the setup screen (its Back button or Escape).
    const auto screen = state.nav.current();
    if ((screen == model::FrontEndScreen::skirmish_setup || screen == model::FrontEndScreen::lan_setup || screen == model::FrontEndScreen::online_setup)
        && state.setup->visible() && state.setup->take_back()) {
        static_cast<void>(state.nav.back());
    }
    state.show_screen();
    state.sync_attract();
    if (state.options.test && state.options.start_battle && state.setup->start_pending()) {
        UtilityFunctions::print("EAWR front end start pressed: attract running ",
                                state.attract != nullptr && state.attract->running());
        state.exit_reason = "test_complete";
        return 0;
    }
    if (state.audio) {
        const auto current = state.nav.current();
        state.audio->set_music(is_menu(current) || current == model::FrontEndScreen::options
            || current == model::FrontEndScreen::load_game || current == model::FrontEndScreen::campaign_setup
            || state.setup->visible());
        state.music_playing = state.audio->music_playing();
    }
    if (state.options.test) {
        // FES-03: sample before each scripted gesture, after the normal playback update.
        // Sample every loading/return frame: the shell pauses while a battle owns the frame.
        if (state.audio && (state.options.battle_round_trip || state.frames % 8 == 0))
            UtilityFunctions::print("EAWR front end music phase: ", text(model::to_string(state.nav.current())),
                                    "; playing ", state.audio->music_playing(), "; position ", state.audio->music_position());
        if (state.frames == 4 && state.audio) {
            UtilityFunctions::print("EAWR front end music: ", state.audio->music_playing() ? "playing " : "silent ",
                                    text(state.audio->music_file()));
            // #3165: the bus the music plays on and the levels of the shared layout, in dB.
            AudioServer* server = AudioServer::get_singleton();
            const auto level = [server](const audio_buses::Bus bus) {
                const int32_t index = server != nullptr ? server->get_bus_index(audio_buses::names[bus]) : -1;
                return index < 0 ? std::string("missing")
                                 : std::to_string(static_cast<double>(server->get_bus_volume_db(index)));
            };
            UtilityFunctions::print("EAWR front end buses: music_bus=", text(state.audio->music_bus()),
                                    " sfx_db=", text(level(audio_buses::sfx)), " voice_db=", text(level(audio_buses::voice)),
                                    " music_db=", text(level(audio_buses::music)), " mix_db=", text(level(audio_buses::mix)));
        }
        if (const auto result = state.run_test()) {
            if (state.exit_reason.empty()) state.exit_reason = "test_complete";
            return result;
        }
    }
    const auto current = state.nav.current();
    if (current != screen_at_start) escape_edge = false;
    // A notice box (a refused start) is modal: the screen under it waits.
    if (state.notice_open()) return std::nullopt;
    if (current == model::FrontEndScreen::campaign_setup) {
        // MNU-34: Escape is Cancel, as on the menus it came from.
        if (escape_edge) {
            if (state.campaign_screen && state.campaign_screen->cancel_options()) return std::nullopt;
            if (const auto result = state.step(model::escape(state.nav)); result) return result;
            return std::nullopt;
        }
        return state.poll_campaign();
    }
    if (!is_menu(current)) return std::nullopt;
    // FES-03: Escape is Back on the sub-menus and quit on the main menu.
    if (escape_edge) {
        if (const auto result = state.step(model::escape(state.nav)); result) return result;
        return std::nullopt;
    }
    return state.poll_menu(state.menus[menu_index(current)]);
}

} // namespace eawr::presentation::godot_backend

#include "eawr/core/load_profile.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/core/text.hpp"
// The tactical HUD over a map (P2-20a, #83): `--eawr-hud tactical` draws the
// space HUD shell for `--eawr-hud-faction <empire|rebel|underworld>` (default
// rebel), laid out by `--eawr-hud-rules <aspect|retail>` (default aspect, D4).
// A live session (#316) draws it by default for its local player's faction;
// `--eawr-hud off` leaves it out.
// Its fonts come from `--eawr-font-cache <dir>`, EAWR_FONT_CACHE or the
// checkout's out/fonts, as for `--eawr-ui-gallery`.

#include "battle_content.hpp"
#include "bink_load_notes.hpp"
#include "bink_child.hpp"
#include "map_mode_internal.hpp"
#include "memory_attribution.hpp"
#include "texture_census_journal.hpp"
#include "menu_timeline.hpp"
#include "viewer_path.hpp"
#include "skirmish_save_hints.hpp"
#include "ui/kit.hpp"

#include "eawr/platform/saved_games.hpp"

#include <godot_cpp/classes/os.hpp>

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/object.hpp>

#include "eawr/battle/start.hpp"

#include <array>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <sstream>

namespace eawr::presentation::godot_backend {

using namespace godot;

namespace {

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] double to_double(const sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}

[[nodiscard]] std::array<float, 7> battlecam_weights(const vfs::Vfs& filesystem) {
    std::array<float, 7> result{1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<std::string_view, 7> names{"Saliency_Size", "Saliency_Power", "Saliency_X",
        "Saliency_Y", "Saliency_Health", "Saliency_Targets", "Saliency_Speed"};
    if (const auto xml = data::load_document(filesystem, "data/xml/gameconstants.xml")) {
        for (const auto& node : xml.value().root.children) for (std::size_t i = 0; i < names.size(); ++i) {
            if (!core::iequals(node.name, names[i])) continue;
            data::tag_trace::used(&node);
            std::string_view value = node.raw_text;
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
            if (!value.empty() && (value.back() == 'f' || value.back() == 'F')) value.remove_suffix(1);
            float parsed{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error == std::errc{} && end == value.data() + value.size() && std::isfinite(parsed)) result[i] = parsed;
        }
    }
    return result;
}

} // namespace

namespace {

// The font cache directory for `flag` (--eawr-font-cache), else EAWR_FONT_CACHE, else the checkout's
// out/fonts, and which of the three chose it.
[[nodiscard]] std::filesystem::path font_cache_directory(const std::string& flag, std::string& source) {
    if (!flag.empty()) {
        source = "flag";
        return ViewerPath{flag}.native();
    }
    if (const String environment = OS::get_singleton()->get_environment("EAWR_FONT_CACHE"); !environment.is_empty()) {
        source = "environment";
        return ViewerPath{utf8(environment)}.native();
    }
    source = "checkout";
    const std::filesystem::path project =
        ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://"))}.native();
    return (project / ".." / ".." / ".." / "out" / "fonts").lexically_normal();
}

} // namespace

presentation::ui::FontCache load_hud_font_cache(const std::filesystem::path& directory) {
    // The cache is its own loose layer at fonts/; a missing directory is an empty cache.
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = directory,
                               .loose_logical_prefix = std::string(presentation::ui::font_cache_prefix),
                               .active_archives = {}};
    auto mounted = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    const vfs::Vfs unmounted;
    return presentation::ui::load_font_cache(mounted ? mounted.value() : unmounted, ViewerPath::utf8(directory));
}

std::string parse_hud_arguments(const PackedStringArray& arguments, const bool live_session,
                                std::optional<TacticalHud::Options>& hud, bool& faction_given) {
    std::string mode;
    std::string faction;
    std::string rules;
    std::string font_cache;
    bool probe = false;
    for (int64_t index = 0; index < arguments.size(); ++index) {
        const String argument = arguments[index];
        if (argument == String("--eawr-hud-probe")) {
            probe = true;
            continue;
        }
        std::string* target = argument == String("--eawr-hud") ? &mode
            : argument == String("--eawr-hud-faction") ? &faction
            : argument == String("--eawr-hud-rules") ? &rules
            : argument == String("--eawr-font-cache") ? &font_cache : nullptr;
        if (target == nullptr) continue;
        if (index + 1 >= arguments.size()) return "missing value for " + utf8(argument);
        *target = utf8(arguments[++index]);
    }
    faction_given = !faction.empty();
    if (mode.empty() && live_session) mode = "tactical";
    if (mode == "off") {
        if (!faction.empty() || !rules.empty() || probe) {
            return "--eawr-hud off contradicts --eawr-hud-faction, --eawr-hud-rules and --eawr-hud-probe";
        }
        return {};
    }
    if (mode.empty()) {
        if (!faction.empty() || !rules.empty() || probe) {
            return "--eawr-hud-faction, --eawr-hud-rules and --eawr-hud-probe need --eawr-hud tactical";
        }
        return {};
    }
    if (mode != "tactical") return "--eawr-hud expects tactical or off";
    TacticalHud::Options options;
    options.probe = probe;
    if (!faction.empty()) {
        const auto parsed = presentation::ui::hud_faction_from(faction);
        if (!parsed) return "--eawr-hud-faction expects empire, rebel or underworld";
        options.faction = *parsed;
    }
    if (!rules.empty() && rules != "aspect" && rules != "retail") return "--eawr-hud-rules expects aspect or retail";
    options.rules = rules == "retail" ? presentation::ui::LayoutRules::retail : presentation::ui::LayoutRules::aspect_correct;
    const std::filesystem::path directory = font_cache_directory(font_cache, options.font_cache_source);
    options.font_cache = load_hud_font_cache(directory);
    hud = std::move(options);
    return {};
}

std::string parse_perf_overlay_argument(const PackedStringArray& arguments, bool& requested) {
    requested = false;
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] != String("--eawr-perf-overlay")) continue;
        if (index + 1 >= arguments.size()) return "missing value for --eawr-perf-overlay";
        const std::string value = utf8(arguments[++index]);
        if (value == "on") requested = true;
        else if (value != "off") return "--eawr-perf-overlay expects on or off";
    }
    return {};
}

void MapMode::State::set_perf_overlay(const bool shown) {
    if (perf == nullptr) {
        if (!shown || perf_host == nullptr) return;
        // The viewer's UI font: the HUD's face from the font cache, the engine font without a cache.
        std::string flag;
        const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
        for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
            if (arguments[index] == String("--eawr-font-cache")) flag = utf8(arguments[index + 1]);
        }
        std::string source;
        perf_fonts = std::make_shared<FontProvider>(load_hud_font_cache(font_cache_directory(flag, source)));
        const presentation::ui::ResolvedFont face = perf_fonts->resolve({"EmpireAtWar-Medium", 7}, "ENGLISH");
        // Above the HUD (canvas layer 1) and the battle overlay.
        auto* layer = memnew(CanvasLayer);
        layer->set_name("EawrPerfOverlayLayer");
        layer->set_layer(100);
        perf = memnew(EawrPerfOverlay);
        perf->set_name("EawrPerfOverlay");
        perf->setup(perf_fonts->font(face), face.face.empty() ? std::string("engine font") : face.face);
        layer->add_child(perf);
        perf_host->add_child(layer);
    }
    perf->set_shown(shown);
}

void MapMode::State::sync_net_perf(const double delta) {
    if (!live_session || !live_session->multiplayer()) return;
    const LanView lan = live_session->lan_view();
    if (lan.phase == LanPhase::ended || lan.phase == LanPhase::idle) return;
    if (delta > 0.0) net_stats.record_frame(delta);
    for (const platform::LiveTickCost& cost : live_session->tick_costs_after(net_last_tick)) {
        net_last_tick = cost.tick;
        net_stats.record_tick({cost.tick, cost.total_ms, {}});
    }
    const presentation::ui::PerfSummary summary = net_stats.summary();
    presentation::ui::NetPerfReport report;
    report.seat = lan.seat;
    report.peers = lan.net_peers;
    report.stall = lan.net_stall;
    report.frame_ms = summary.frame_avg_ms;
    report.sim_ms = summary.tick_avg_ms;
    net_report = std::move(report);
    // Every 5 s, so a log read after the fact shows game against network for the whole match.
    net_log_clock += delta;
    if (net_log_clock >= presentation::ui::net_perf_log_seconds) {
        net_log_clock = 0.0;
        const std::string line = presentation::ui::net_summary_line(*net_report);
        UtilityFunctions::print(String::utf8(line.data(), static_cast<int64_t>(line.size())));
    }
}

std::string MapMode::State::net_perf_json() const {
    return net_report ? presentation::ui::net_report_json(*net_report) : std::string("null");
}

void MapMode::State::sync_perf_overlay() {
    if (perf == nullptr || !perf->shown()) return;
    perf->set_network(net_report);
    std::vector<presentation::ui::PerfTick> ticks;
    if (live_session) {
        // After a hidden spell the old ticks are skipped, not drawn as one burst.
        const bool rebase = perf->wants_rebase();
        for (const platform::LiveTickCost& cost : live_session->tick_costs_after(perf_last_tick)) {
            perf_last_tick = cost.tick;
            if (rebase) continue;
            presentation::ui::PerfTick tick{cost.tick, cost.total_ms, {}};
            for (const platform::LivePhaseCost& phase : cost.phases) tick.phases.push_back({phase.name, phase.ms});
            ticks.push_back(std::move(tick));
        }
    }
    perf->frame(ticks, live_session ? std::optional<std::size_t>(live_session->visible_units().size()) : std::nullopt,
                live_session != nullptr);
}

std::string MapMode::State::perf_report_json() const {
    if (perf != nullptr) return perf->report_json();
    return std::string("{\"key\": \"") + EawrPerfOverlay::key_name + "\", \"shown\": false}";
}

// MNU-19: the in-game menu opens while the battle runs and pauses it (UI-S1); a multiplayer battle
// cannot pause, so its menu opens over the running game. Resume and Esc close it.
bool MapMode::State::open_game_menu() {
    if (options.attract || !live_session) return false;
    if (live_session->phase() != LiveSessionView::Phase::running || live_session->time().ended()) return false;
    if (loading != nullptr && loading->shown()) return false;
    if (menu_handler) {
        menu_handler();
        return true;
    }
    if (menu_host == nullptr || !filesystem) return false;
    if (game_menu != nullptr && !UtilityFunctions::is_instance_id_valid(game_menu_id)) game_menu = nullptr;
    if (game_menu != nullptr && game_menu->shown()) return true;
    if (game_menu == nullptr) {
        auto* screen = memnew(EawrOptionsScreen);
        EawrOptionsScreen::Options screen_options;
        screen_options.filesystem = filesystem;
        screen_options.game_root = options.game_root;
        screen_options.mod_root = options.mod_root;
        screen_options.context = EawrOptionsScreen::Context::in_game;
        screen_options.multiplayer = live_session->observer_multiplayer();
        screen_options.apply_window = options.capture_path.empty();
        if (live_session->can_save()) {
            // #3989: a local single-player skirmish saves from its menu.
            screen_options.save_names = [directory = player_save_directory()] {
                std::vector<std::string> names;
                if (const auto listed = platform::list_saved_games(directory)) {
                    for (const auto& slot : listed.value()) names.push_back(slot.name);
                }
                return names;
            };
            screen_options.save_game = [this](const std::string& name, const bool overwrite) {
                return save_battle(name, overwrite);
            };
            screen_options.load_directory = player_save_directory();
            screen_options.load_game = options.battle_load;
        }
        if (!screen->build(std::move(screen_options))) {
            UtilityFunctions::printerr("EAWR in-game menu: ", String::utf8(screen->failure().c_str()));
            memdelete(screen);
            return false;
        }
        game_menu = screen;
        game_menu_id = static_cast<int64_t>(screen->get_instance_id());
    }
    menu_paused = false;
    if (!options.lan && !live_session->time().paused()) {
        live_session->press_pause();
        menu_paused = live_session->time().paused();
    }
    game_menu->show(*menu_host, EawrOptionsScreen::Page::game_menu);
    menu_open = game_menu->shown();
    UtilityFunctions::print("EAWR in-game menu opened: paused ", live_session->time().paused(), "; by menu ", menu_paused);
    return true;
}

std::filesystem::path MapMode::State::player_save_directory() const {
    return save_directory.empty()
        ? ViewerPath{std::string(OS::get_singleton()->get_user_data_dir().utf8().get_data())}.native() / "saves"
        : save_directory;
}

// #3989: the Save page's Save. The view saves the battle between two ticks and writes the file off the frame;
// the hints keep the camera and the map's name for the load and the list.
bool MapMode::State::save_battle(const std::string& name, const bool overwrite) {
    if (!live_session || !platform::valid_saved_game_name(name)) return false;
    eawr::viewer::SkirmishSaveHints hints;
    if (map_camera && map_camera->active()) {
        const auto& controller = map_camera->controller();
        hints.camera = eawr::viewer::SkirmishSaveCamera{controller.tactical_frame().target, controller.target_zoom(),
                                                  controller.yaw_degrees()};
    }
    hints.label = std::filesystem::path(options.map_path).stem().string();
    const auto scoring = live_session->save_scoring();
    if (scoring) hints.scoring = scoring.value();
    else UtilityFunctions::printerr("EAWR battle scoring save: ", String::utf8(core::format_diagnostic(scoring.error()).c_str()));
    const bool begun = live_session->save_game(player_save_directory(), name, overwrite,
                                                 eawr::viewer::encode_skirmish_save_hints(hints));
    UtilityFunctions::print("EAWR battle save requested: ", String::utf8(name.c_str()), "; started ", begun);
    return begun;
}

void MapMode::State::service_game_menu() {
    if (live_session) static_cast<void>(live_session->service_save());
    if (!menu_open) return;
    if (game_menu != nullptr && !UtilityFunctions::is_instance_id_valid(game_menu_id)) game_menu = nullptr;
    if (game_menu != nullptr && game_menu->shown()) return;
    menu_open = false;
    const bool resumed = menu_paused && live_session;
    if (resumed) live_session->resume();
    menu_paused = false;
    const bool quit = game_menu != nullptr && game_menu->quit_requested();
    UtilityFunctions::print("EAWR in-game menu closed: resumed ", resumed, "; quit ", quit);
    if (!quit) return;
    // Quit Game confirmed: leave the battle for the main menu. A multiplayer battle leaves through
    // its abort route (the other peers are told); the host then follows the battle's exit.
    if (live_session && live_session->leave()) leave_to_menu = true;
}

void MapMode::State::free_game_menu() {
    if (game_menu != nullptr && UtilityFunctions::is_instance_id_valid(game_menu_id)) {
        if (Node* parent = game_menu->get_parent()) parent->remove_child(game_menu);
        memdelete(game_menu);
    }
    game_menu = nullptr;
    menu_open = false;
}

std::optional<std::array<float, 2>> MapMode::State::game_menu_point(const std::string& name) const {
    if (game_menu == nullptr || !UtilityFunctions::is_instance_id_valid(game_menu_id)) return std::nullopt;
    const char* gadget = name == "menu_resume" ? "IDC_GAME_OPTIONS_RESUME" : name == "menu_options" ? "IDC_INGAME_OPTIONS"
        : name == "menu_quit" ? "IDC_GAME_OPTIONS_QUIT" : name == "menu_yes" ? "yes" : name == "menu_no" ? "no"
        : name == "menu_back" ? "IDC_OK_IN_GAME" : name == "menu_save" ? "IDC_SAVE_GAME"
        : name == "menu_load" ? "IDC_LOAD_GAME" : name == "menu_load_first" ? "load_first"
        : name == "menu_load_button" ? "load_button"
        : name == "menu_save_first" ? "IDC_SAVE_GAME_LIST#0" : name == "menu_save_button" ? "IDC_SAVE_GAME_BUTTON" : nullptr;
    if (gadget == nullptr) return std::nullopt;
    const auto point = game_menu->control_point(gadget);
    if (!point) return std::nullopt;
    return std::array<float, 2>{point->x, point->y};
}

bool MapMode::State::build_hud(Node3D& host, const std::optional<std::string>& context_name) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::hud);
    // #558: the overlay's parent; `--eawr-perf-overlay on` shows it from the first frame.
    perf_host = &host;
    menu_host = &host;
    if (perf_requested) set_perf_overlay(true);
    if (!hud_options) return true;
    if (live_session && !hud_faction_given) {
        // The live session's local player; a faction without a shell variant keeps the default.
        if (const auto faction = presentation::ui::hud_faction_from(live_session->local_faction())) {
            hud_options->faction = *faction;
        }
    }
    hud = std::make_unique<TacticalHud>(*hud_options);
    hud_shortcut_wasd.reset();
    // Over the 3D view on its own canvas layer. The host routes world input after
    // the GUI, so the HUD's hit mask keeps its clicks off the world (UI-I1).
    const auto& planet_context = options.campaign_planet ? options.campaign_planet : context_name;
    if (!hud->build(*filesystem, catalog ? &*catalog : nullptr, planet_context, host)) {
        failure = "--eawr-hud tactical: " + hud->failure();
        hud.reset();
        return false;
    }
    host.set_process_input(true);
    if (!options.campaign_briefing_movie.empty()) {
        const auto helper = ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://common/eawr_bink_helper.exe"))}.native();
        const auto game_root = options.game_root;
        hud->prepare_command_bar_movie(*filesystem, options.campaign_briefing_movie,
            [helper, game_root](std::vector<std::byte> source, std::shared_ptr<std::atomic_bool> cancelled) {
                return open_bink_child(helper, game_root, std::move(source), std::move(cancelled));
            },
            [](std::string file, std::string layer, std::string movie) {
                enqueue_bink_load_note(std::move(file), std::move(layer), std::move(movie));
            });
        if (loading && hud->hud()) {
            const auto identity = static_cast<uint64_t>(hud->hud()->get_instance_id());
            loading->hold_dismiss([identity] {
                // The child HUD may leave before its loading screen during teardown.
                const auto* node = godot::ObjectDB::get_instance(identity);
                return !node || static_cast<bool>(node->get_meta("eawr_movie_ready", true));
            });
        }
    }
    host.set_process_unhandled_input(true);
    // #459 and #453: the time panel's buttons, the pause banner's Resume Game and the end panel's
    // Quit Game drive the live session; scripted hud=<name> gestures find them by name.
    if (live_session) {
        LiveSessionView* live = live_session.get();
        // BE-06: LAN currently has no replay-permission setting; assume allowed.
        hud->set_replay_writer(live->multiplayer(), [live](std::string& error) { return live->finished_replay_bytes(error); },
            [live](std::string& error) { return live->finished_session_replay_bytes(error); });
        hud->set_movie_handler([this] { if (space) set_movie_mode(true); });
        hud->set_time_handlers({[live] { live->press_pause(); }, [live] { live->press_fast_forward(); },
                                [live] { live->resume(); }, [live] { live->quit(); }});
        // The options button (tooltip TEXT_BUTTON_MAIN_MENU) opens the in-game menu, as Escape does.
        hud->set_options_handler([this] { static_cast<void>(open_game_menu()); });
        if (battle) {
            TacticalHud* named = hud.get();
            // #1923: while the loading screen shows, its own Begin button is the "begin" a hand reaches.
            battle->set_hud_point([named, this](const std::string& name) -> std::optional<std::array<float, 2>> {
                if (name == "begin" && loading != nullptr && loading->shown()) {
                    if (const auto rect = loading->begin_rect()) {
                        const godot::Vector2 centre = rect->get_center();
                        return std::array<float, 2>{centre.x, centre.y};
                    }
                }
                if (name.starts_with("menu_")) return game_menu_point(name);
                return named->control_point(name);
            });
            BattleInput* input = battle.get();
            hud->set_special_weapon_handler([input, live] { input->special_weapon_click(*live); });
            hud->set_beacon_handler([input, live] { input->beacon_click(*live); });
            hud->set_gift_handler([input, live](const sim::SeatId recipient) { input->gift_click(recipient, *live); });
            hud->set_order_handler([input, live](const std::string_view name) {
                const ui::OrderMode mode = name == "attack" ? ui::OrderMode::attack
                    : name == "attack_move" ? ui::OrderMode::attack_move : name == "guard" ? ui::OrderMode::guard
                    : name == "waypoint" ? ui::OrderMode::waypoint : name == "move" ? ui::OrderMode::move : ui::OrderMode::none;
                input->command_click(mode, name == "stop", *live, ui::CommandOrigin::hud_button);
            });
        }
        sync_battle_hud();
    }
    // #425: the live battle's selection fills the command bar's unit cards, and a card click
    // changes the selection (presentation only).
    if (battle) {
        battle->set_world_group_fonts(hud->world_group_font(true), hud->world_group_font(false));
        if (EawrUnitCards* cards = hud->unit_cards()) {
            BattleInput* input = battle.get();
            battle->set_card_slots(cards->slot_count());
            if (live_session) {
                LiveSessionView* live = live_session.get();
                cards->set_click([this, input, live](const std::size_t slot, const bool shift) {
                    // #530 PU-60: while the station is the production object the slots are its
                    // build buttons, and a click buys.
                    if (input->production_station()) {
                        static_cast<void>(input->build_click(slot, *live));
                    } else {
                        input->card_click(slot, shift, *live);
                    }
                    sync_cards();
                });
            }
            if (live_session) {
                const LiveSessionView* live = live_session.get();
                cards->set_clock([live]() {
                    // Card clicks use wall time when interactive, presented time in a driven run.
                    if (live->options().real_time) {
                        using clock = std::chrono::steady_clock;
                        static const clock::time_point start = clock::now();
                        return std::chrono::duration<double>(clock::now() - start).count();
                    }
                    return live->presented_tick() / static_cast<double>(sim::tactical::logical_frames_per_second);
                });
            }
            battle->set_card_point([cards](const std::size_t slot) -> std::optional<std::array<float, 2>> {
                const auto rect = cards->slot_rect(slot);
                if (!rect) return std::nullopt;
                const Vector2 centre = rect->get_center();
                return std::array<float, 2>{centre.x, centre.y};
            });
        }
    }
    if (battle && live_session && space && hud->hero_tray() != nullptr) {
        hud->hero_tray()->set_click([this](const EawrHeroTray::Tray tray, const EawrHeroTray::Icon& icon,
                                         const bool right, const bool double_click) {
            if (!battle || !live_session || !space) return;
            battle->hero_click(icon.hero, tray == EawrHeroTray::Tray::hostile, right, double_click,
                               *live_session, *space);
            sync_cards();
        });
    }
    // #454: an ability button's release becomes the battle input's ability request (a report line
    // until #76); scripted ability=N gestures find the N-th shown button.
    if (battle && live_session) {
        battle->set_ability_icon([this](const ui::CardUnit& unit, const bool second) -> std::string_view {
            return hud ? std::string_view(hud->ability_button_icon(unit.type, second ? unit.second_ability : unit.ability)) : std::string_view{};
        });
        if (EawrAbilityButtons* abilities = hud->ability_buttons()) {
            BattleInput* input = battle.get();
            const LiveSessionView* live = live_session.get();
            TacticalHud* ability_hud = hud.get();
            abilities->set_click([input, live, ability_hud](const std::size_t index, const bool right) {
                static_cast<void>(input->ability_click(index, right, *live));
                ability_hud->set_ability_bar(input->ability_bar());
            });
            abilities->set_clock([live]() {
                if (live->options().real_time) {
                    using clock = std::chrono::steady_clock;
                    static const clock::time_point start = clock::now();
                    return std::chrono::duration<double>(clock::now() - start).count();
                }
                return live->presented_tick() / static_cast<double>(sim::tactical::logical_frames_per_second);
            });
            battle->set_ability_point([abilities](const std::size_t index) -> std::optional<std::array<float, 2>> {
                const auto rect = abilities->button_rect(index);
                if (!rect) return std::nullopt;
                const Vector2 centre = rect->get_center();
                return std::array<float, 2>{centre.x, centre.y};
            });
        }
    }
    // #530 PU-63, PU-68: a release on a queued slot cancels that entry; a press on a pool slot
    // starts placing its type. Both reach the simulation only as commands.
    if (battle && live_session) {
        if (EawrProductionPanel* production = hud->production()) {
            BattleInput* input = battle.get();
            LiveSessionView* live = live_session.get();
            production->set_cancel([live](const std::size_t component, const std::uint64_t entry_id) {
                const bool upgrades = component < presentation::ui::queue_slot_count;
                const auto queue = upgrades ? sim::tactical::BuildQueue::upgrades : sim::tactical::BuildQueue::units;
                static_cast<void>(live->cancel_build(queue, entry_id));
            });
            production->set_pick([this, input](const std::size_t slot) {
                if (!live_session->reinforcement_allowed() || input->placing() || slot >= pool_types.size()) return;
                if (live_session->economy().campaign && (slot >= pool_tokens.size()
                    || !live_session->select_campaign_reinforcement(pool_tokens[slot]))) return;
                input->begin_placement(pool_types[slot], *live_session);
            });
            production->set_open([live] { live->reinforcement_feedback(LiveSessionView::ReinforcementFeedback::Kind::pane); });
            production->set_drag([input](const Vector2 point) { input->placement_move({point.x, point.y}); },
                [input, live](const Vector2 point) { input->placement_drop({point.x, point.y}, *live); },
                [input, live] { input->cancel_placement(*live); },
                [live](const double detents) { static_cast<void>(live->rotate_reinforcement(detents)); });
        }
    }
    // #455 MM-11: a left press or drag on the minimap looks at the point; a right click orders the
    // selection there. Both are presentation or player commands only.
    if (battle && live_session && space && hud->minimap() != nullptr) {
        BattleInput* input = battle.get();
        LiveSessionView* live = live_session.get();
        SpaceEnvironment* battle_space = space.get();
        TacticalHud* minimap_hud = hud.get();
        hud->set_minimap_handlers(
            [battle_space](const double x, const double y) {
                battle_space->live_camera_focus(static_cast<float>(x), static_cast<float>(y));
            },
            [input, live](const double x, const double y, const bool double_click) {
                static_cast<void>(input->minimap_move(x, y, double_click, *live));
            });
        battle->set_minimap_point([minimap_hud](const double x, const double y) { return minimap_hud->minimap_point(x, y); });
    }
    return true;
}

void MapMode::State::fill_type_names() {
    if (!minimap_type_names.empty() || !live_session || live_session->tables() == nullptr) return;
    for (const units::UnitType& type : live_session->tables()->units) minimap_type_names.emplace(battle::type_id(type.id), type.id);
    for (const auto& type : live_session->tables()->obstacles) minimap_type_names.emplace(battle::type_id(type.id), type.id);
    // #530: the station build lists also name objects outside the unit tables (the upgrades, PU-20),
    // so their build buttons find their Icon_Name.
    for (const units::UnitType& type : live_session->tables()->units) {
        for (const auto& group : type.production.buildable) {
            for (const std::string& name : group.types) minimap_type_names.emplace(battle::type_id(name), name);
        }
    }
}

void MapMode::State::sync_cards() {
    if (!hud || !battle) return;
    const auto* production_hover = hud->shell_shown() && hud->production()
        ? hud->production()->hovered_entry() : nullptr;
    hud->set_production_hover(production_hover);
    if (live_session) {
        const auto research = live_session->team_research_view();
        // Name lookup is shared with production; no unit/world census for the dock.
        fill_type_names();
        hud->set_team_dock(research, live_session->economy().upgrades, minimap_type_names);
    }
    if (live_session && live_session->order_input()) {
        const auto mode = live_session->order_input()->mode();
        hud->set_order_mode(mode == ui::OrderMode::attack ? "attack" : mode == ui::OrderMode::attack_move ? "attack_move"
            : mode == ui::OrderMode::waypoint ? "waypoint" : mode == ui::OrderMode::guard ? "guard" : mode == ui::OrderMode::move ? "move" : "");
    }
    if (battle->production_station()) {
        if (EawrUnitCards* cards = hud->unit_cards(); cards && production_menu_cache.refresh(battle->build_buttons())) {
            if (production_menu_cache.layout_changed || !cards->update_build_states(battle->build_buttons())) {
                fill_type_names();
                std::vector<EawrUnitCards::Card> buttons;
                for (const presentation::ui::BuildButton& button : battle->build_buttons()) {
                    EawrUnitCards::Card card;
                    card.slot = button.slot;
                    const auto name = minimap_type_names.find(button.type);
                    card.type = name != minimap_type_names.end() ? name->second : std::to_string(button.type);
                    card.price = button.price;
                    // PU-62: an option the session never builds (PU-20) carries no price; show the listed one.
                    if (button.price == 0) card.price = hud->listed_build_cost(card.type).value_or(0);
                    card.room = button.room;
                    card.disabled = !button.enabled;
                    card.build_frames = button.build_frames;
                    card.pad = button.pad;
                    card.cooldown_progress = button.cooldown_progress;
                    card.disabled_reason = button.disabled_reason;
                    buttons.push_back(std::move(card));
                }
                cards->show(std::move(buttons), {});
            }
        }
    } else {
        production_menu_cache.close();
        hud->set_unit_cards(battle->card_layout(), battle->card_units());
    }
    hud->set_ability_bar(battle->ability_bar());
    // #944 (WHE-44): the hero tray follows the snapshot; its icons are rebuilt only when the roster changed.
    if (hud->hero_tray() != nullptr && live_session) {
        battle->refresh_heroes(*live_session);
        if (hero_tray_revision != battle->hero_revision()) {
            hero_tray_revision = battle->hero_revision();
            fill_type_names();
            const auto icons = [&](const std::vector<presentation::ui::HeroEntry>& entries) {
                std::vector<EawrHeroTray::Icon> out;
                out.reserve(entries.size());
                for (const presentation::ui::HeroEntry& entry : entries) {
                    const auto name = minimap_type_names.find(entry.type);
                    out.push_back({name != minimap_type_names.end() ? name->second : std::to_string(entry.type), entry.health, 0,
                                   entry.automatic, entry.hero, entry.represented});
                }
                return out;
            };
            hud->set_hero_tray(icons(battle->hero_roster().friendly), icons(battle->hero_roster().hostile));
        }
    }
    const bool dock_hover = hud->team_dock_hovered();
    bool ability_hover{};
    if (auto* abilities = hud->ability_buttons(); abilities && hud->shell_shown() && !dock_hover && !production_hover) {
        if (const auto index = abilities->hovered_button()) {
            const auto& button = abilities->bar().buttons[*index];
            if (!button.units.empty()) {
                const auto& units = battle->card_units();
                const auto source = std::find_if(units.begin(), units.end(), [&](const auto& unit) {
                    return unit.id == button.units.front();
                });
                if (source != units.end()) {
                    hud->set_ability_hover(source->type, button.ability, button.component);
                    ability_hover = true;
                }
            }
        }
    }
    if (hud->unit_cards() && !ability_hover && !dock_hover && !production_hover) {
        const auto hovered = hud->shell_shown() ? battle->encyclopedia_unit() : std::nullopt;
        const auto identity = hovered ? std::optional(std::pair{hovered->entity, hovered->type}) : std::nullopt;
        if (identity != encyclopedia_world_hover || encyclopedia_ui_hover) {
            encyclopedia_world_hover = identity;
            const auto name = hovered ? minimap_type_names.find(hovered->type) : minimap_type_names.end();
            hud->set_world_hover(hovered && name != minimap_type_names.end()
                ? std::optional(std::pair{static_cast<std::uint64_t>(hovered->entity), name->second}) : std::nullopt);
        }
    }
    encyclopedia_ui_hover = ability_hover || dock_hover || production_hover != nullptr;
}

void MapMode::State::sync_production() {
    if (!hud || !live_session || hud->production() == nullptr) return;
    const LiveSessionView& live = *live_session;
    fill_type_names();
    const auto name_of = [this](const sim::tactical::TypeId type) {
        const auto name = minimap_type_names.find(type);
        return name != minimap_type_names.end() ? name->second : std::to_string(type);
    };
    auto& view = production_view;
    view.garrison_progress.reset();
    if (const auto* timer = live.local_garrison_timer()) {
        view.garrison_progress = presentation::ui::hud_timer_progress(
            static_cast<std::uint64_t>(std::max(0.0, live.presented_tick())), timer->due_frame, timer->duration_frames);
    }
    view.notifications = live.reinforcement_notifications();
    view.notification_seconds = live.reinforcement_notification_tick() / sim::tactical::logical_frames_per_second;
    // Land reinforcement drops are not wired yet: the pane stays inert there.
    const bool movie = space && space->live_camera_movie();
    view.reinforcement_allowed = presentation::ui::reinforcement_available(
        live.reinforcement_allowed(), overview_ui.tactical_shell, movie, static_cast<bool>(land_population));
    view.tactical_population.clear();
    if ((!view.reinforcement_allowed || movie) && battle->placing()) battle->cancel();
    if (const sim::tactical::EconomyView* economy = live.local_economy()) {
        // WPR-04/41: only registered arrivals count; station capacity and queued
        // purchases do not change the tactical ledger readout.
        view.tactical_population = presentation::ui::population_text(economy->population, economy->population_cap);
        // Queue identities change only on purchase, cancellation or completion. Keep the
        // owned names and vectors between frames; progress and its short label update in place.
        if (production_queues != economy->queues) {
            production_queues = economy->queues;
            view.queue.clear();
            for (const auto& slot : presentation::ui::layout_build_queue(economy->queues, 0))
                view.queue.push_back({slot.component, name_of(slot.type), slot.progress, slot.percent, slot.entry_id});
        }
        const auto presented_frame = static_cast<std::uint64_t>(std::max(0.0, live.presented_tick()));
        for (auto& slot : view.queue) {
            if (!slot.percent) continue;
            const auto kind = slot.component < presentation::ui::queue_slot_count
                ? sim::tactical::BuildQueue::upgrades : sim::tactical::BuildQueue::units;
            const auto& front = economy->queues[static_cast<std::size_t>(kind)].front();
            const auto left = front.complete_frame > presented_frame ? front.complete_frame - presented_frame : 0U;
            const auto remaining = std::min<std::uint64_t>(left, front.frames);
            slot.progress = front.frames > 0 ? static_cast<double>(front.frames - remaining) / static_cast<double>(front.frames) : 0.0;
            std::array<char, 4> label{};
            const auto formatted = std::to_chars(label.data(), label.data() + label.size() - 1,
                static_cast<int>(slot.progress * 100.0));
            *formatted.ptr = '%';
            slot.percent->assign(label.data(), static_cast<std::size_t>(formatted.ptr - label.data()) + 1);
        }
        view.credits = presentation::ui::credits_text(economy->credits);
        view.tactical_tech = presentation::ui::TacticalTechView{
            static_cast<std::int32_t>(economy->tech_level),
            presentation::ui::hud_faction_from(live.local_faction()).value_or(presentation::ui::HudFaction::rebel),
            true, true}; // HCB-61: this viewer hosts multiplayer-style space skirmishes.
        // PU-21: a type's population value from the station menus.
        const auto population_of = [&live](const sim::tactical::TypeId type) {
            if (live.economy().campaign) for (const auto& [id, population] : live.economy().campaign_population)
                if (id == type) return population;
            for (const sim::tactical::StationMenu& menu : live.economy().menus) {
                if (const sim::tactical::BuildOption* option = menu.find(type)) return option->population;
            }
            return 0U;
        };
        // A drop made while paused leaves the pane before its command runs (TM-10).
        const std::size_t pending = live.pending_reinforcements();
        if (production_pool_cache.refresh(*economy) || pending != production_pool_pending) {
            production_pool_pending = pending;
            const auto used = economy->population;
            const auto pool = live.economy().campaign
                ? presentation::ui::layout_campaign_pool(live.campaign_reinforcement_pool(), used, economy->population_cap)
                : presentation::ui::layout_pool(live.reinforcement_pool(), used, economy->population_cap, population_of);
            view.pool.clear();
            pool_types.clear();
            pool_tokens.clear();
            for (const auto& slot : pool) {
                view.pool.push_back({slot.slot, name_of(slot.type), slot.text, slot.enabled});
                pool_types.push_back(slot.type);
                pool_tokens.push_back(slot.token);
            }
            view.population = presentation::ui::population_text(economy->population,
                                                                economy->population_cap);
            view.rows = presentation::ui::pool_rows(pool.size());
        }
    }
    hud->production()->show(view, live.presented_tick() / sim::tactical::logical_frames_per_second);
}

bool MapMode::State::cinematics_allowed() {
    if (in_game_cinematics) return *in_game_cinematics;
    if (!filesystem) return false;
    in_game_cinematics = true;
    if (const auto xml = data::load_document(*filesystem, "data/xml/gameconstants.xml")) {
        for (const auto& node : xml.value().root.children) {
            if (!core::iequals(node.name, "In_Game_Cinematics")) continue;
            data::tag_trace::used(&node);
            std::string_view value = node.raw_text;
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
            if (core::iequals(value, "false") || value == "0" || core::iequals(value, "no")) in_game_cinematics = false;
            break;
        }
    }
    return *in_game_cinematics;
}

std::vector<BattlecamCandidate> MapMode::State::movie_candidates() {
    if (!live_session || !space_population) return {};
    const auto& visible = live_session->visible_units();
    if (!movie_weights && filesystem) movie_weights = battlecam_weights(*filesystem);
    const auto tick = static_cast<std::uint64_t>(std::max(0.0, live_session->presented_tick()));
    const auto snapshot = live_session->snapshot_at(tick);
    const auto previous_snapshot = tick ? live_session->snapshot_at(tick - 1) : nullptr;
    const auto* tables = live_session->tables();
    if (!snapshot || !tables || !movie_weights) return {};
    std::map<sim::tactical::TypeId, const units::UnitType*> types;
    for (const auto& type : tables->units) types.emplace(battle::type_id(type.id), &type);
    std::map<sim::EntityId, const sim::tactical::TacticalInstance*> instances, previous;
    std::map<sim::EntityId, float> targets;
    for (const auto& instance : snapshot->instances()) {
        instances.emplace(instance.entity_id, &instance);
        const auto add_target = [&](const sim::EntityId id) {
            if (id) { targets[instance.entity_id] += 1.0F; targets[id] += 1.0F; }
        };
        add_target(instance.presentation_attack_target);
        for (const auto& turret : instance.hardpoint_turrets) add_target(turret.target);
    }
    if (previous_snapshot) for (const auto& instance : previous_snapshot->instances()) previous.emplace(instance.entity_id, &instance);
    std::vector<BattlecamCandidate> candidates;
    for (const auto& unit : visible) {
        const auto model = space_population->live_ship_box(unit.ship);
        const auto info = types.find(unit.type);
        const auto state = instances.find(unit.entity);
        if (!model || info == types.end() || state == instances.end() || !info->second->movie_object
            || !info->second->locomotion || !state->second->durability || state->second->durability->hull.raw() <= 0) continue;
        if (std::any_of(info->second->category_mask.begin(), info->second->category_mask.end(),
            [](const auto& category) { return core::iequals(category, "Wall"); })) continue;
        const float x = (model->high[0] - model->low[0]) * 0.5F;
        const float y = (model->high[1] - model->low[1]) * 0.5F;
        const float z = (model->high[2] - model->low[2]) * 0.5F;
        float distance_squared{};
        if (const auto old = previous.find(unit.entity); old != previous.end()) for (std::size_t axis = 0; axis < 3; ++axis) {
            const float displacement = static_cast<float>(to_double(state->second->fixed_transform.rows[axis][3])
                - to_double(old->second->fixed_transform.rows[axis][3]));
            distance_squared += displacement * displacement;
        }
        const float power = info->second->ai_combat_power ? static_cast<float>(to_double(*info->second->ai_combat_power)) : 0.0F;
        const auto squadron = live_session->squadron_of().find(unit.entity);
        const auto selection_id = squadron == live_session->squadron_of().end() ? unit.entity : squadron->second;
        candidates.push_back({unit.entity, unit.type, state->second->presentation_attack_target,
            {x*x+y*y+z*z, power, static_cast<float>(unit.position[0]), static_cast<float>(unit.position[1]),
             static_cast<float>(to_double(state->second->durability->hull)), targets[unit.entity], std::sqrt(distance_squared)},
            battle && battle->selected(selection_id), 0.0F});
    }
    return candidates;
}

namespace {
[[nodiscard]] std::string_view shot_kind_name(const BattlecamShotKind kind) noexcept {
    switch (kind) {
    case BattlecamShotKind::chase: return "chase";
    case BattlecamShotKind::circle: return "circle";
    case BattlecamShotKind::flyby: return "flyby";
    case BattlecamShotKind::belly: return "belly";
    case BattlecamShotKind::floating: return "floating";
    case BattlecamShotKind::frigate: return "frigate";
    }
    return "";
}
} // namespace

bool MapMode::State::begin_movie_shot(const LiveSessionView::VisibleUnit& subject) {
    if (!space_population || !live_session || !live_session->tables()) return false;
    const auto box = space_population->live_ship_box(subject.ship);
    if (!box) return false;
    std::array<float, 3> extents{};
    for (std::size_t i = 0; i < 3; ++i) extents[i] = (box->high[i] - box->low[i]) * 0.5F;
    if (!(extents[0]*extents[0] + extents[1]*extents[1] + extents[2]*extents[2] > 0.0F)) return false;
    BattlecamCategory category;
    movie_tec.reset();
    movie_hardpoint.reset();
    movie_frigate_target = 0;
    std::optional<std::array<float, 3>> frigate_target_extents;
    sim::EntityId transfer_target{};
    movie_anchor = {static_cast<float>(subject.position[0]), static_cast<float>(subject.position[1]),
                    static_cast<float>(subject.position[2])};
    movie_elapsed = 0.0;
    for (const auto& type : live_session->tables()->units) {
        if (battle::type_id(type.id) != subject.type) continue;
        if (!type.glory_cinematics.empty() && filesystem) {
            const auto& name = type.glory_cinematics[movie_random.next() % type.glory_cinematics.size()];
            if (const auto bytes = filesystem->open("Data/Art/Cinematics/" + name))
                movie_tec = parse_battlecam_tec(bytes.value());
            if (movie_tec) {
                for (std::size_t row = 0; row < 3; ++row) for (std::size_t col = 0; col < 3; ++col)
                    movie_tec_rotation[row][col] = static_cast<float>(to_double(box->model_to_world.rows[row][col]));
                movie_shot.duration = static_cast<float>(movie_tec->duration) / 30.0F;
                return true;
            }
        }
        const auto snapshot = live_session->snapshot_at(static_cast<std::uint64_t>(std::max(0.0, live_session->presented_tick())));
        if (snapshot) {
            const auto instance = std::find_if(snapshot->instances().begin(), snapshot->instances().end(),
                [&](const auto& item) { return item.entity_id == subject.entity; });
            if (instance != snapshot->instances().end()) {
                const auto& visible = live_session->visible_units();
                const auto transfer = std::find_if(visible.begin(), visible.end(),
                    [&](const auto& unit) { return unit.entity == instance->presentation_attack_target; });
                if (transfer != visible.end() && frigate_target_eligible(static_cast<float>(subject.yaw),
                    static_cast<float>(transfer->yaw), static_cast<float>(transfer->position[0] - subject.position[0]),
                    static_cast<float>(transfer->position[1] - subject.position[1]))) {
                    if (const auto target_box = space_population->live_ship_box(transfer->ship)) {
                        std::array<float, 3> half{};
                        for (std::size_t axis = 0; axis < 3; ++axis)
                            half[axis] = (target_box->high[axis] - target_box->low[axis]) * 0.5F;
                        frigate_target_extents = half;
                        transfer_target = transfer->entity;
                    }
                }
                std::vector<MovieHardpoint> hardpoints;
                for (const auto& weapon : instance->hardpoint_turrets) {
                    if (!weapon.available || !weapon.target || weapon.hardpoint >= type.hardpoints.size()) continue;
                    if (instance->durability && weapon.hardpoint < instance->durability->hardpoints.size()
                        && !instance->durability->hardpoints[weapon.hardpoint].enabled) continue;
                    const auto& hardpoint = type.hardpoints[weapon.hardpoint];
                    if (!hardpoint.fire_a.owner_bone_index || !hardpoint.attachment.owner_bone_index) continue;
                    const auto target = std::find_if(visible.begin(), visible.end(),
                        [&](const auto& unit) { return unit.entity == weapon.target; });
                    if (target == visible.end() || !space_population->live_ship_box(target->ship)) continue;
                    const auto fire = space_population->live_bone_frame(subject.entity, *hardpoint.fire_a.owner_bone_index);
                    const auto anchor = space_population->live_bone_frame(subject.entity, *hardpoint.attachment.owner_bone_index);
                    if (!fire || !anchor || !fire->visible || !anchor->visible) continue;
                    std::array<float, 3> direction{};
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        direction[axis] = static_cast<float>(to_double(fire->transform.rows[axis][0]));
                    if (!hardpoint_camera_offset(direction)) continue;
                    hardpoints.push_back({*hardpoint.fire_a.owner_bone_index, *hardpoint.attachment.owner_bone_index, weapon.target});
                }
                if (!hardpoints.empty()) {
                    movie_hardpoint = hardpoints[movie_random.next() % hardpoints.size()];
                    movie_shot.duration = static_cast<float>(120U + movie_random.next() % 61U) / 30.0F;
                    return true;
                }
            }
        }
        for (const auto& value : type.category_mask) {
            category.fighter = category.fighter || core::iequals(value, "Fighter");
            category.bomber = category.bomber || core::iequals(value, "Bomber");
            category.corvette = category.corvette || core::iequals(value, "Corvette");
            category.frigate = category.frigate || core::iequals(value, "Frigate");
        }
        break;
    }
    movie_shot = make_battlecam_shot(extents, category, movie_random, frigate_target_extents);
    if (movie_shot.kind == BattlecamShotKind::frigate) movie_frigate_target = transfer_target;
    return true;
}

std::optional<AttractCameraPose> MapMode::State::movie_pose(const LiveSessionView::VisibleUnit& subject) const {
    if (movie_hardpoint && !movie_preview) {
        const auto fire = space_population->live_bone_frame(subject.entity, movie_hardpoint->fire_bone);
        const auto anchor = space_population->live_bone_frame(subject.entity, movie_hardpoint->attachment_bone);
        const auto& visible = live_session->visible_units();
        const auto target = std::find_if(visible.begin(), visible.end(),
            [this](const auto& unit) { return unit.entity == movie_hardpoint->target; });
        if (!fire || !anchor || !fire->visible || !anchor->visible || target == visible.end()) return std::nullopt;
        std::array<float, 3> forward{};
        for (std::size_t i = 0; i < 3; ++i) forward[i] = static_cast<float>(to_double(fire->transform.rows[i][0]));
        const auto offset = hardpoint_camera_offset(forward);
        if (!offset) return std::nullopt;
        std::array<float, 3> source{};
        for (std::size_t i = 0; i < 3; ++i) source[i] = static_cast<float>(to_double(anchor->transform.rows[i][3])) + (*offset)[i];
        const auto eye = presentation::space::render_from_source({source[0], source[1], source[2]});
        const auto aim = presentation::space::render_from_source({static_cast<float>(target->position[0]),
            static_cast<float>(target->position[1]), static_cast<float>(target->position[2])});
        return AttractCameraPose{{eye.x, eye.y, eye.z}, {aim.x, aim.y, aim.z}};
    }
    const auto box = space_population->live_ship_box(subject.ship);
    if (!box) return std::nullopt;
    if (!movie_preview && !movie_tec && movie_shot.kind == BattlecamShotKind::frigate) {
        const auto& visible = live_session->visible_units();
        const auto target = std::find_if(visible.begin(), visible.end(),
            [this](const auto& unit) { return unit.entity == movie_frigate_target; });
        if (target == visible.end()) return std::nullopt;
        const auto target_box = space_population->live_ship_box(target->ship);
        if (!target_box) return std::nullopt;
        const float t = std::clamp(static_cast<float>(movie_elapsed) / movie_shot.duration, 0.0F, 1.0F);
        const auto point = [](const auto& unit, const auto& frame, const auto& offset) {
            std::array<float, 3> result{};
            for (std::size_t row = 0; row < 3; ++row) {
                result[row] = static_cast<float>(unit.position[row]);
                for (std::size_t col = 0; col < 3; ++col)
                    result[row] += static_cast<float>(to_double(frame.model_to_world.rows[row][col])) * offset[col];
            }
            return result;
        };
        const auto start = point(subject, *box, movie_shot.start);
        const auto middle = point(subject, *box, movie_shot.middle);
        const auto end = point(*target, *target_box, movie_shot.end);
        std::array<float, 3> eye{}, aim{};
        const float phase = t < 0.5F ? t * 2.0F : (t - 0.5F) * 2.0F;
        for (std::size_t i = 0; i < 3; ++i) {
            const float from = t < 0.5F ? start[i] : middle[i];
            const float to = t < 0.5F ? middle[i] : end[i];
            eye[i] = from + (to - from) * phase;
            aim[i] = static_cast<float>(subject.position[i] + (target->position[i] - subject.position[i]) * t);
        }
        const auto rendered_eye = presentation::space::render_from_source({eye[0], eye[1], eye[2]});
        const auto rendered_aim = presentation::space::render_from_source({aim[0], aim[1], aim[2]});
        return AttractCameraPose{{rendered_eye.x, rendered_eye.y, rendered_eye.z}, {rendered_aim.x, rendered_aim.y, rendered_aim.z}};
    }
    std::array<float, 3> offset{};
    std::array<float, 3> target_offset{};
    if (movie_preview) {
        offset = preview_offset(preview_distance, preview_azimuth, preview_elevation);
    } else if (movie_tec) {
        offset = tec_position(movie_tec->camera, static_cast<float>(movie_elapsed * 30.0));
        target_offset = tec_position(movie_tec->target, static_cast<float>(movie_elapsed * 30.0));
    } else { offset = movie_shot.at(static_cast<float>(movie_elapsed)); target_offset = movie_shot.target; }
    std::array<float, 3> source{}, target{};
    for (std::size_t row = 0; row < 3; ++row) {
        const bool floating = !movie_preview && !movie_tec && movie_shot.kind == BattlecamShotKind::floating;
        source[row] = floating ? movie_anchor[row] : static_cast<float>(subject.position[row]);
        target[row] = floating ? movie_anchor[row] : static_cast<float>(subject.position[row]);
        for (std::size_t col = 0; col < 3; ++col) {
            const float rotation = movie_tec && !movie_preview ? movie_tec_rotation[row][col]
                : static_cast<float>(to_double(box->model_to_world.rows[row][col]));
            source[row] += floating ? (row == col ? offset[col] : 0.0F) : rotation * offset[col];
            if (!movie_preview) target[row] += rotation * target_offset[col];
        }
    }
    const auto eye = presentation::space::render_from_source({source[0], source[1], source[2]});
    const auto aim = presentation::space::render_from_source({target[0], target[1], target[2]});
    return AttractCameraPose{{eye.x, eye.y, eye.z}, {aim.x, aim.y, aim.z}};
}

std::optional<float> MapMode::State::movie_fov() const {
    if (movie_preview || !movie_tec) return std::nullopt;
    const auto horizontal = tec_fov(movie_tec->fov, static_cast<float>(movie_elapsed * 30.0));
    if (!horizontal) return std::nullopt;
    const auto vertical = presentation::camera::vertical_fov_degrees(*horizontal);
    return vertical ? std::optional<float>{vertical.value()} : std::nullopt;
}

void MapMode::State::enter_unit_preview() {
    if (!space || !live_session || !space_population || !battle
        || live_session->phase() != LiveSessionView::Phase::running || !cinematics_allowed()) return;
    if (movie_preview && space->live_camera_movie()) {
        preview_azimuth = std::fmod(preview_azimuth - 45.0F + 360.0F, 360.0F);
        preview_elevation = 30.0F;
        perf_camera.shot_cut = true; // trace only: the orbit steps 45 degrees at once
        advance_movie(0.0);
        return;
    }
    const auto& visible = live_session->visible_units();
    const auto hovered = battle->hovered_subject();
    const auto subject = std::find_if(visible.begin(), visible.end(), [&](const auto& unit) {
        if (hovered) return unit.entity == *hovered;
        const auto team = live_session->squadron_of().find(unit.entity);
        const auto selected = team == live_session->squadron_of().end() ? unit.entity : team->second;
        return battle->selected(selected);
    });
    if (subject == visible.end() || find_preview_subject(visible, live_session->alive_units(),
        live_session->sensor_visible_units(), subject->entity, live_session->reveal_all()) == visible.end()) return;
    const auto box = space_population->live_ship_box(subject->ship);
    if (!box) return;
    float length_squared{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float extent = (box->high[i] - box->low[i]) * 0.5F;
        length_squared += extent * extent;
    }
    preview_radius = std::sqrt(length_squared);
    if (!(preview_radius > 0.0F)) return;
    movie_preview = true;
    movie_tec.reset();
    movie_subject = subject->entity;
    // Trace only: the C-key preview starts here (a cut for the drawn camera).
    ++perf_camera.shot_id;
    perf_camera.shot_cut = true;
    perf_camera.input.reset();
    perf_camera.shot_type = "preview";
    preview_azimuth = 0.0F;
    preview_elevation = 30.0F;
    // #6569: the smaller playable-map dimension caps the start and the zoom-out limit (0 = unknown).
    preview_extent = 0.0F;
    if (const auto& bounds = live_session->economy().bounds) {
        const float width = static_cast<float>(to_double((*bounds)[2]) - to_double((*bounds)[0]));
        const float height = static_cast<float>(to_double((*bounds)[3]) - to_double((*bounds)[1]));
        if (width > 0.0F && height > 0.0F) preview_extent = std::min(width, height);
    }
    preview_distance = preview_start_distance(preview_radius, preview_extent);
    const auto target = presentation::space::render_from_source({static_cast<float>(subject->position[0]),
        static_cast<float>(subject->position[1]), static_cast<float>(subject->position[2])});
    space->live_camera_movie(true, {target.x, target.y, target.z});
    if (live_fog) live_fog->set_movie_hidden(space->live_camera_movie());
    if (hud) hud->set_movie_mode(space->live_camera_movie());
    advance_movie(0.0);
}

void MapMode::State::set_movie_mode(const bool on, const bool immediate) {
    if (!on) {
        const bool stop_bands = immediate || !live_session
            || live_session->phase() != LiveSessionView::Phase::running || live_session->time().paused();
        restore_battlecam([&] { if (space) space->live_camera_movie(false, {}); },
            [&] { if (live_fog) live_fog->set_movie_hidden(false); },
            [&] { if (hud) hud->set_movie_mode(false, stop_bands); },
            movie_subject, movie_key_down, movie_tab_down);
        movie_preview = false;
        preview_key_down = false;
        return;
    }
    if (!space || !live_session || !space_population || live_session->phase() != LiveSessionView::Phase::running || !cinematics_allowed()) return;
    const auto& visible = live_session->visible_units();
    if (visible.empty()) { set_movie_mode(false); return; }
    movie_preview = false;
    auto candidates = movie_candidates();
    if (candidates.empty() || !movie_weights) { set_movie_mode(false); return; }
    const auto chosen = choose_battlecam_subject(std::move(candidates), *movie_weights, movie_subject,
        !space->live_camera_movie(), movie_random);
    const auto subject_it = std::find_if(visible.begin(), visible.end(), [chosen](const auto& unit) { return unit.entity == chosen; });
    if (subject_it == visible.end()) { set_movie_mode(false); return; }
    const auto* subject = &*subject_it;
    movie_subject = subject->entity;
    if (!begin_movie_shot(*subject)) { set_movie_mode(false); return; }
    // Trace only: a new battlecam shot starts here (a cut for the drawn camera).
    ++perf_camera.shot_id;
    perf_camera.shot_cut = true;
    perf_camera.input.reset();
    perf_camera.shot_type = movie_hardpoint ? std::string("hardpoint") : movie_tec ? std::string("tec") : std::string(shot_kind_name(movie_shot.kind));
    const auto target = presentation::space::render_from_source({static_cast<float>(subject->position[0]),
        static_cast<float>(subject->position[1]), static_cast<float>(subject->position[2])});
    space->live_camera_movie(true, {target.x, target.y, target.z});
    if (live_fog) live_fog->set_movie_hidden(space->live_camera_movie());
    advance_movie(0.0);
    if (hud) hud->set_movie_mode(space->live_camera_movie());
    // Movie entry cancels an armed world order without issuing it.
    if (battle) {
        battle->command_click(ui::OrderMode::none, false, *live_session, ui::CommandOrigin::hud_button);
        battle->cancel();
    }
}


void MapMode::State::advance_movie(const double delta) {
    if (!space || !space->live_camera_movie()) return;
    const bool playing = live_session && space_population && live_session->phase() == LiveSessionView::Phase::running && cinematics_allowed();
    const bool paused = playing && live_session->time().paused();
    const auto before = battlecam_advance(playing, paused, false, false);
    if (before == BattlecamAdvance::exit) { set_movie_mode(false, true); return; }
    const auto& visible = live_session->visible_units();
    if (movie_preview && preview_subject_lost(visible, live_session->alive_units(),
        live_session->sensor_visible_units(), movie_subject, live_session->reveal_all())) {
        set_movie_mode(false); return;
    }
    // A zero-time call initializes the shot pose even if the battle is paused.
    if (before == BattlecamAdvance::hold && delta != 0.0) return;
    if (!paused) movie_elapsed += std::max(delta, 0.0);
    if (battlecam_advance(true, paused, !movie_preview && movie_elapsed >= movie_shot.duration, movie_key_down || movie_tab_down)
        == BattlecamAdvance::next) {
        set_movie_mode(true);
        return;
    }
    apply_movie_pose();
}

bool MapMode::State::apply_movie_pose() {
    if (!space || !live_session || !space_population) return false;
    const auto& visible = live_session->visible_units();
    const auto subject = std::find_if(visible.begin(), visible.end(),
        [this](const auto& unit) { return unit.entity == movie_subject; });
    if (subject == visible.end()) return false; // BCAM-08: ordinary shots retain the last pose until expiry.
    const auto pose = movie_pose(*subject);
    if (!pose) return false;
    // Trace only: the subject position this pose was built from (the visible-unit list as of the start of this frame, before this
    // frame's session frame; the drawn camera of this frame is aimed there, not at where the subject is drawn this frame).
    if (PerfTrace::movers() > 0) perf_camera.input = subject->position;
    space->live_camera_movie(true, pose->target);
    space->live_camera_movie_eye(pose->eye);
    space->live_camera_movie_fov(movie_fov());
    return true;
}

void MapMode::State::sync_overview_ui() {
    if (!live_session || !space) return;
    const std::string level = space->live_camera_overview();
    const auto is_level = [](const std::string& value) { return value == "off" || value == "overview" || value == "map"; };
    bool movie = space->live_camera_movie();
    if (hud) hud->set_movie_mode(movie);
    const bool on = level == "overview" || level == "map";
    const bool first = overview_level.empty();
    if (level != overview_level) {
        // V-5g: every level change, by the wheel or the overview key, fades from the last old frame.
        if (is_level(overview_level) && is_level(level) && perf_host != nullptr && perf_host->get_viewport() != nullptr) {
            if (!overview_fade) overview_fade = std::make_unique<OverviewFadeView>(*perf_host);
            overview_fade->request(*perf_host->get_viewport());
        }
        overview_level = level;
    }
    // V-5a: x1 and x2 hide the same set.
    overview_ui = presentation::ui::overview_ui(on || movie);
    if (movie) overview_ui.world_markers = false;
    if (hud) hud->set_overview(!overview_ui.tactical_shell);
    if (battle) {
        battle->set_overview_ui(overview_ui);
        if (first) battle->set_overview_probe([this] {
            std::ostringstream output;
            output << "{\"shell\": ";
            if (hud) output << (hud->shell_shown() ? "true" : "false");
            else output << "null";
            output << ", \"pause_banner\": ";
            if (hud) output << (hud->pause_banner_shown() ? "true" : "false");
            else output << "null";
            output << ", \"minimap_syncs\": " << minimap_syncs << ", \"fade_opacity\": ";
            if (overview_fade && overview_fade->opacity()) output << *overview_fade->opacity();
            else output << "null";
            output << "}";
            return output.str();
        });
    }
    if (overview_fade) overview_fade->frame();
}

std::string MapMode::State::overview_report_json() const {
    std::ostringstream output;
    output << "{\"level\": \"" << overview_level << "\", \"tactical_shell\": " << (overview_ui.tactical_shell ? "true" : "false")
           << ", \"pause_banner\": " << (overview_ui.pause_banner ? "true" : "false")
           << ", \"radar_contents\": " << (overview_ui.radar_contents ? "true" : "false")
           << ", \"unit_brackets\": " << (overview_ui.unit_brackets ? "true" : "false")
           << ", \"world_markers\": " << (overview_ui.world_markers ? "true" : "false")
           // V-5f: the remake draws no scene distance fog in space (the legacy adapters disable it),
           // so the overview's fog switch has nothing to turn off.
           << ", \"distance_fog\": \"not drawn\", \"minimap_syncs\": " << minimap_syncs << ", \"fade\": ";
    if (overview_fade) output << overview_fade->report_json();
    else output << "null";
    output << "}";
    return output.str();
}

void MapMode::State::sync_minimap() {
    if (!hud || !live_session || !battle || !space || hud->minimap() == nullptr) return;
    // V-5c: the radar's contents stop updating while either overview level is on.
    if (!overview_ui.radar_contents) return;
    ++minimap_syncs;
    const auto bounds = space->live_camera_bounds();
    const auto& latest = live_session->battle_frame().latest;
    if (!bounds || !latest) return;
    const LiveSessionView& live = *live_session;
    const auto local_radar = std::find_if(latest->players().begin(), latest->players().end(),
        [&](const auto& player) { return player.player_id == live.local_player(); });
    const bool display_enemies = local_radar != latest->players().end() && local_radar->radar_enabled;
    fill_type_names();
    if (!minimap_height) {
        // MM-09: the mean height of the lobby players' radar-visible units, taken once there are any
        // (until then the outline uses the plane z = 0).
        double sum = 0.0;
        std::size_t count = 0;
        for (const sim::tactical::TacticalInstance& instance : latest->instances()) {
            if (!live.player_colour(instance.owner)) continue;
            const auto name = minimap_type_names.find(instance.type_id);
            if (name == minimap_type_names.end() || !hud->minimap_looks(name->second).visible) continue;
            sum += to_double(instance.fixed_transform.rows[2][3]);
            ++count;
        }
        if (count > 0) minimap_height = sum / static_cast<double>(count);
    }
    TacticalHud::MinimapView view;
    if (battle_audio) {
        for (const auto& warning : battle_audio->radar_warnings()) {
            view.warnings.push_back({warning.position[0], warning.position[1], warning.age});
        }
    }
    view.extents = presentation::ui::minimap_extents(bounds->min_x, bounds->max_x, bounds->min_y, bounds->max_y);
    if (const auto& cells = live.battle_frame().fog) {
        const auto& rules = cells->rules;
        view.cells = presentation::ui::MinimapFogCells{to_double(rules.map_left), to_double(rules.map_top),
            to_double(rules.cell_size), rules.cells_wide, rules.cells_tall,
            {}, std::shared_ptr<const std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>>(cells, &cells->values)};
    }
    const auto owner_colour = [&](const sim::tactical::PlayerId owner) {
        if (const auto colour = live.player_colour(owner)) return data::ui::Rgba8{(*colour)[0], (*colour)[1], (*colour)[2], 255};
        return hud->faction_colour(live.player_faction(owner)).value_or(data::ui::Rgba8{100, 100, 100, 255});
    };
    std::vector<presentation::ui::MinimapPlayerColour> radar_players;
    for (const auto& player : latest->players()) {
        radar_players.push_back({player.player_id, player.team_id, player.neutral, owner_colour(player.player_id)});
    }
    // WHZ-70/72: all hazard flags share the static mask, regardless of radar visibility or fog.
    for (const auto& instance : latest->instances()) {
        const auto name = minimap_type_names.find(instance.type_id);
        if (name == minimap_type_names.end() || !hud->minimap_looks(name->second).hazard) continue;
        auto size = minimap_hazard_sizes.find(instance.type_id);
        if (size == minimap_hazard_sizes.end()) {
            std::array<double, 3> half{};
            std::string path;
            double scale = 1.0;
            if (const auto* tables = live.tables()) {
                const auto read = [&](const auto& types) {
                    for (const auto& type : types) if (type.id == name->second) {
                        path = type.model_path;
                        if (type.scale_factor) scale = to_double(*type.scale_factor);
                        break;
                    }
                };
                read(tables->units);
                read(tables->obstacles);
            }
            if (!path.empty() && filesystem) if (auto model = assets::load_model(*filesystem, path)) {
                const auto frames = units::bind_frames(model.value());
                std::array<double, 3> low{}, high{};
                bool first = true;
                for (const auto& mesh : model.value().meshes) for (unsigned corner = 0; corner < 8; ++corner) {
                    const std::array<double, 3> local{(corner & 1U) ? mesh.bounds_max.x : mesh.bounds_min.x,
                        (corner & 2U) ? mesh.bounds_max.y : mesh.bounds_min.y,
                        (corner & 4U) ? mesh.bounds_max.z : mesh.bounds_min.z};
                    auto point = local;
                    if (frames && mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) < frames.value().size()) {
                        const auto& bone_frame = frames.value()[static_cast<std::size_t>(mesh.bone)];
                        for (std::size_t row = 0; row < 3; ++row) point[row] = to_double(bone_frame.rows[row][3])
                            + to_double(bone_frame.rows[row][0]) * local[0] + to_double(bone_frame.rows[row][1]) * local[1]
                            + to_double(bone_frame.rows[row][2]) * local[2];
                    }
                    // The model's fixed quarter turn precedes the instance's own yaw.
                    point = {-point[1] * scale, point[0] * scale, point[2] * scale};
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        low[axis] = first ? point[axis] : std::min(low[axis], point[axis]);
                        high[axis] = first ? point[axis] : std::max(high[axis], point[axis]);
                    }
                    first = false;
                }
                for (std::size_t axis = 0; axis < 3; ++axis) half[axis] = (high[axis] - low[axis]) * 0.5;
            }
            size = minimap_hazard_sizes.emplace(instance.type_id, half).first;
        }
        std::array<double, 2> extent{};
        for (std::size_t row = 0; row < 2; ++row) for (std::size_t axis = 0; axis < 3; ++axis) {
            extent[row] += std::abs(to_double(instance.fixed_transform.rows[row][axis])) * size->second[axis];
        }
        view.hazards.push_back({to_double(instance.fixed_transform.rows[0][3]), to_double(instance.fixed_transform.rows[1][3]),
            extent[0], extent[1], hud->minimap_looks(name->second).hazard_kind});
    }
    const auto make_blip = [&](const sim::EntityId entity, const sim::tactical::TypeId type,
                               const sim::tactical::PlayerId owner, const bool hostile) {
        presentation::ui::MinimapUnit blip;
        blip.id = entity;
        const auto name = minimap_type_names.find(type);
        if (name != minimap_type_names.end()) blip.type = name->second;
        // MM-07: the owner's lobby colour; a player without one (a map's Neutral or Pirates owner)
        // takes its faction's Factions.xml colour, else Neutral's grey.
        blip.owner_colour = owner_colour(owner);
        blip.hostile = hostile;
        blip.selected = battle->selected(entity);
        return blip;
    };
    std::vector<const LiveSessionView::VisibleUnit*> visible;
    visible.reserve(live.visible_units().size());
    for (const auto& unit : live.visible_units()) visible.push_back(&unit);
    std::sort(visible.begin(), visible.end(), [](const auto* left, const auto* right) { return left->entity < right->entity; });
    // WNO-41: candidates come from copied live snapshots, independently of model visibility.
    for (const auto& instance : latest->instances()) {
        const auto entity = instance.entity_id;
        // MM-15: members contribute to their team's identity instead of submitting another blip.
        if (live.squadron_of().contains(entity) || live.squadron_members().contains(entity)) continue;
        auto blip = make_blip(entity, instance.type_id, instance.owner,
            sim::tactical::players_hostile(latest->players(), instance.owner, live.local_player()));
        blip.radar = live.radar_state(entity);
        blip.radar.display_enemies = display_enemies;
        blip.in_nebula = instance.in_nebula;
        const auto pose = live.unit_frame(entity);
        blip.x = pose ? pose->position[0] : to_double(instance.fixed_transform.rows[0][3]);
        blip.y = pose ? pose->position[1] : to_double(instance.fixed_transform.rows[1][3]);
        blip.yaw_degrees = pose ? pose->yaw_degrees : 0.0;
        const auto* type = live.tables() ? live.tables()->find(blip.type) : nullptr;
        blip.radar.capital_layer = type && type->movement.space_layer == "Capital";
        if (const auto* pad_profile = live.economy().pads.point(instance.type_id)) {
            const auto capture_colour = [&](const sim::tactical::PlayerId owner) {
                auto fallback = owner_colour(owner);
                if (pad_profile->community_property) {
                    const auto player = std::find_if(radar_players.begin(), radar_players.end(),
                        [owner](const auto& p) { return p.player == owner; });
                    // WNO-43: Neutral capture endpoints use faction colour, not community fallback.
                    if (player != radar_players.end() && player->neutral) return fallback;
                    if (player == radar_players.end() || player->team == 0xffffffffU) {
                        const auto unassigned = hud->minimap_looks(blip.type).no_colorization;
                        fallback = unassigned && *unassigned != data::ui::Rgba8{0,0,0,0} ? *unassigned
                            : hud->faction_colour(live.player_faction(owner), true).value_or(fallback);
                    }
                    // Tactical starts use multiplayer lobby bindings, including their recorded replays.
                    fallback = presentation::ui::minimap_community_colour(radar_players, owner,
                        live.local_player(), true, fallback);
                }
                return fallback;
            };
            const auto* pad = live.pad_view(entity);
            const auto target = pad && pad->state.progress.raw() > 0 ? pad->state.target : instance.owner;
            const bool raw_fog = view.cells && !view.cells->revealed(blip.x, blip.y);
            blip.capture = presentation::ui::MinimapCaptureColour{raw_fog, capture_colour(instance.owner),
                capture_colour(target), pad ? to_double(pad->state.progress) : 0.0};
        }
        const auto shown = std::lower_bound(visible.begin(), visible.end(), entity,
            [](const auto* unit, const auto id) { return unit->entity < id; });
        const auto box = space_population && shown != visible.end() && (*shown)->entity == entity
            ? space_population->live_ship_box((*shown)->ship) : std::nullopt;
        // MM-17: current visibility and pick-box availability do not establish model absence.
        if (space_population) if (const auto ship = live.composed_ship(entity)) {
            blip.radar.model_present = space_population->live_ship_model_present(*ship);
        }
        if (hud->minimap_looks(blip.type).draw_to_scale && box) {
            blip.world_half_size = presentation::ui::minimap_world_half_size(box->model_to_world, box->low, box->high);
        }
        view.units.push_back(std::move(blip));
    }
    std::vector<presentation::ui::MinimapSquadronMember> members;
    for (const auto& [squadron, ids] : live.squadron_members()) {
        const auto* container = live.snapshot_index().instance(squadron);
        if (container == nullptr) continue;
        members.clear();
        const LiveSessionView::VisibleUnit* leader = nullptr;
        bool member_radar_visible = false;
        for (const auto id : ids) {
            const auto* instance = live.snapshot_index().instance(id);
            if (instance == nullptr) continue; // dead and docked craft do not belong to the live team centre
            const auto member_type = minimap_type_names.find(instance->type_id);
            if (member_type != minimap_type_names.end()) {
                member_radar_visible = member_radar_visible || hud->minimap_looks(member_type->second).visible;
            }
            const auto shown = std::lower_bound(visible.begin(), visible.end(), id,
                [](const auto* unit, const sim::EntityId member) { return unit->entity < member; });
            const bool seen = shown != visible.end() && (*shown)->entity == id;
            if (members.empty() && seen) leader = *shown;
            const auto pose = live.unit_frame(id);
            members.push_back({pose ? pose->position[0] : to_double(instance->fixed_transform.rows[0][3]),
                pose ? pose->position[1] : to_double(instance->fixed_transform.rows[1][3]),
                pose ? pose->yaw_degrees : 0.0, seen});
        }
        const auto pose = presentation::ui::minimap_squadron_pose(members);
        if (!pose || leader == nullptr) continue;
        auto blip = make_blip(squadron, container->type_id, container->owner, leader->hostile);
        // MM-15: the team's authored type supplies geometry/style. Live craft supply
        // radar permission, including teams whose own type omits the radar flag.
        const auto* type = live.tables() != nullptr ? live.tables()->find(blip.type) : nullptr;
        if (type == nullptr || type->team_type.empty()) continue;
        blip.type = type->team_type;
        blip.team = true; // MM-17: model-free team extent, independent of member spread
        blip.team_radar_visible = member_radar_visible;
        blip.radar.display_enemies = display_enemies;
        blip.x = pose->x;
        blip.y = pose->y;
        blip.yaw_degrees = pose->yaw_degrees;
        // WHZ-25/MM-12: the published team hazard state still controls nebula suppression.
        blip.in_nebula = container->in_nebula;
        view.units.push_back(std::move(blip));
    }
    for (const auto& beacon : live.visible_beacons()) {
        presentation::ui::MinimapUnit blip;
        blip.id = std::numeric_limits<sim::EntityId>::max() / 16U + beacon.player;
        blip.type = live.beacon_type(beacon.type);
        blip.owner_colour = owner_colour(beacon.player);
        blip.x = to_double(beacon.position.x);
        blip.y = to_double(beacon.position.y);
        blip.team = true; // authored icon scale, independent of a model bounding box.
        view.units.push_back(std::move(blip));
    }
    std::sort(view.units.begin(), view.units.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    // WNO-44: retained copies never enter visible_units(), selection or attack cursor queries.
    for (const auto& [entity, stored] : live.radar_memories()) {
        if (!stored.retained_model) continue;
        const auto name = minimap_type_names.find(stored.type);
        if (name == minimap_type_names.end()) continue;
        presentation::ui::MinimapMemory memory;
        memory.id = entity;
        memory.type = name->second;
        memory.owner_colour = owner_colour(stored.owner);
        memory.hostile = sim::tactical::players_hostile(latest->players(), stored.owner, live.local_player());
        memory.capture_point = stored.capture_point;
        memory.previously_revealed = true;
        memory.retained_model = true;
        memory.display_enemies = display_enemies;
        memory.x = stored.position[0];
        memory.y = stored.position[1];
        memory.yaw_degrees = stored.yaw;
        memory.world_half_size = stored.world_half_size;
        view.memories.push_back(std::move(memory));
    }
    // MM-10: the local player's fog cells where the battle has them (#494: the cells the fog in
    // the world draws), else the local team's units with a sensor range reveal.
    for (const auto& revealer : live.snapshot_index().revealers()) {
        view.revealers.push_back({revealer.x, revealer.y, revealer.range});
    }
    view.ground = battle->ground_corners(minimap_height.value_or(0.0));
    // FW-14: `--eawr-live-reveal on` draws no fog plane in the world, so the minimap's fog layer
    // stays consistent with it (MM-10) and draws nothing either, without touching the fog cells
    // or revealers the session actually published (the sim stays presentation-untouched).
    view.fog = !live_options.reveal;
    hud->set_minimap(view);
}

void MapMode::State::open_loading_screen(Node3D& host, const assets::Map& map) {
    loading = options.loading;
    // BLD-12: a setup host waits for Begin; a campaign host releases its
    // retained-child readiness after scene preparation instead.
    if (loading != nullptr) {
        live_options.begin_barrier = options.loading_begin_barrier && live_begin_barrier(static_cast<bool>(options.lan));
        return;
    }
    if (!live_options.begin_barrier.value_or(live_options.real_time) || !filesystem) return;
    auto* screen = memnew(EawrLoadingScreen);
    EawrLoadingScreen::Options screen_options;
    screen_options.content = options.content ? options.content
        : std::make_shared<const BattleContent>(BattleContent{filesystem, catalog, profile, layers});
    screen_options.map_path = options.map_path;
    screen_options.context_name = map.context_name;
    if (live_options.land) screen_options.battle = presentation::ui::LoadingBattle::land;
    if (!screen->build(host, std::move(screen_options))) {
        // The battle still starts, behind the HUD's own Begin (WBF-10), and the report says why.
        loading_failure = screen->failure();
        memdelete(screen);
        return;
    }
    loading = screen;
}

void MapMode::State::load_report(const std::size_t stage, const std::uint64_t done, const std::uint64_t total) {
    // The battle audio's file reads run beside the load; publish what they have finished (a few ms at most).
    if (battle_audio) battle_audio->pump_loading();
    load_progress.report(stage, done, total);
    if (GodotRenderer::texture_census_enabled()) {
        if (done == total) TextureCensusJournal::snapshot(("stage" + std::to_string(stage)).c_str());
        else TextureCensusJournal::periodic("load_periodic");
    }
    if (done == total && MenuTimeline::active()) {
        const std::string label = "battle_stage" + std::to_string(stage);
        MenuTimeline::mark(label.c_str());
    }
    if (done == total && MemoryAttribution::enabled()) {
        const std::string label = "stage" + std::to_string(stage);
        const auto stats = filesystem ? scene::VfsAssetCache(*filesystem).model_stats() : scene::VfsAssetCache::ModelStats{};
        MemoryAttribution::load_mark(label.c_str(), stats.cache_retained_bytes, stats.capacity.total_bytes());
    }
    if (loading != nullptr) loading->report(load_progress);
}

void MapMode::State::begin_battle() {
    // BLD-13: Begin starts the battle and the screen leaves in the same frame.
    if (live_session) live_session->begin();
    if (loading != nullptr) loading->dismiss();
    sync_battle_hud();
}

void MapMode::State::sync_battle_hud() {
    if (!live_session) return;
    const bool waiting = live_session->phase() == LiveSessionView::Phase::loading
        || live_session->phase() == LiveSessionView::Phase::ready;
    // A battle that began some other way (a time-panel press) takes the screen with it.
    if (loading != nullptr && loading->shown() && !waiting) loading->dismiss();
    if (!hud) return;
    const auto* saved_keys = ActiveSettings::get();
    const bool wasd = saved_keys && saved_keys->camera_wasd;
    if (hud_shortcut_wasd != wasd) {
        hud_shortcut_wasd = wasd;
        for (const auto& [order, control] : std::array<std::pair<const char*, const char*>, 3>{{
             {"attack", "IDC_EDIT_ATTACK"}, {"stop", "IDC_EDIT_STOP"}, {"waypoint", "IDC_EDIT_WAYPOINT"}}}) {
            hud->set_order_shortcut(order, std::string(order) + " ("
                + keyboard_key_label(ui::project_key_for(control, wasd)) + ")");
        }
    }
    const presentation::ui::TimeControls& time = live_session->time();
    const bool running = live_session->phase() == LiveSessionView::Phase::running;
    const auto& end = live_session->battle_end();
    const auto lan = live_session->lan_view();
    const bool departure = live_session->multiplayer() && lan.phase == LanPhase::ended && lan.victory.has_value();
    const bool results_shown = departure || (end && end->ended_frame);
    if (battle) {
        hud->set_special_weapon(battle->special_weapon_button(*live_session));
        hud->set_beacon(battle->beacon_enabled(*live_session), battle->beacon_targeting());
        const bool gift_enabled = battle->gift_enabled(*live_session);
        static const std::vector<ui::ObserverOwner> no_recipients;
        hud->set_gift(live_session->controller_policy_enabled(), gift_enabled,
            gift_enabled ? live_session->gift_recipients() : no_recipients);
    }
    hud->set_begin(live_session->phase() == LiveSessionView::Phase::ready && (loading == nullptr || !loading->shown()));
    static const std::string no_child_pause_notice;
    const auto& child_pause_notice = options.shared_pause_notice ? options.shared_pause_notice() : no_child_pause_notice;
    // #5355: a multiplayer battle's fast forward (skirmish LAN or a LAN GC battle child) is the agreed shared 2x
    // (TP-MP06), not the local 4x.
    const auto agreed_pace = live_session->agreed_pacing();
    const bool shared_speed = agreed_pace || live_session->multiplayer();
    hud->set_time_view({time.paused() && (!options.shared_pause_notice || !child_pause_notice.empty()) && (!live_session->multiplayer() || lan_gameplay_enabled(live_session->lan_view())),
                        shared_speed ? live_session->shared_fast_forward_pressed() : time.state() == presentation::ui::TimeState::fast_forward,
                        running && time.pause_enabled(),
                        running && (agreed_pace ? !agreed_pace->paused
                            : live_session->multiplayer() ? lan_gameplay_enabled(lan) && !lan.clock_paused
                            : !options.shared_pause_notice && time.fast_forward_enabled()),
                        live_session->multiplayer(), time.speed_step()});
    const auto pause_view = live_session->lan_view();
    const bool shared_pause = live_session->multiplayer() && pause_view.clock_paused && lan_gameplay_enabled(pause_view);
    const auto& save_status = live_session->save_status();
    if (options.shared_pause_notice) {
        if (child_pause_notice != shown_child_pause_notice) {
            shown_child_pause_notice = child_pause_notice;
            hud->set_pause_notice(child_pause_notice);
        }
    } else hud->set_pause_notice(shared_pause ? "Paused by " + pause_view.pause_name : "");
    if (save_status.failed != shown_save_failures) {
        shown_save_failures = save_status.failed;
        hud->set_retreat_countdown("Save failed: " + save_status.last_problem, {255, 100, 100, 255});
        save_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    if (save_notice_until && std::chrono::steady_clock::now() >= *save_notice_until) {
        hud->set_retreat_countdown({}, {255, 255, 255, 255});
        save_notice_until.reset();
    }
    if (shared_speed) update_speed_notice(running, lan, agreed_pace);
    if (live_session->multiplayer()) {
        if (!lan_overlay) {
            auto* layer = memnew(CanvasLayer); layer->set_layer(40); perf_host->add_child(layer);
            lan_overlay = memnew(Control); layer->add_child(lan_overlay);
            lan_incident = memnew(Control); lan_incident->set_theme(hud->hud()->get_theme());
            lan_incident->set_oversampling_with_scale(CanvasItem::OVERSAMPLING_WITH_SCALE_ENABLED);
            lan_incident->set_size(Vector2(1280, 96)); lan_incident->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            layer->add_child(lan_incident);
            auto* incident_frame = memnew(EawrUiFrame); incident_frame->set_small(true);
            incident_frame->set_position(Vector2(12, 8)); incident_frame->set_size(Vector2(1256, 88));
            lan_incident->add_child(incident_frame);
            auto* incident = memnew(EawrUiLabel); incident->set_name("LanIncidentText"); incident->set_role("L_Text"); incident->set_wrap(true);
            incident->set_position(Vector2(24, 20)); incident->set_size(Vector2(950, 72));
            incident->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); lan_incident->add_child(incident);
            auto* retry = memnew(EawrUiButton); retry->set_name("LanIncidentSave"); retry->set_toggle_mode(true);
            retry->set_fit_to_rect(true);
            retry->set_text(String::utf8(hud->localized_text("TEXT_BUTTON_SAVE", "Save").c_str()));
            retry->set_position(Vector2(1030, 20)); retry->set_size(Vector2(200, 36)); lan_incident->add_child(retry);
            lan_overlay->set_size(Vector2(820, 280));
            lan_overlay->set_oversampling_with_scale(CanvasItem::OVERSAMPLING_WITH_SCALE_ENABLED);
            lan_overlay->set_theme(hud->hud()->get_theme());
            lan_overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            auto* lan_frame = memnew(EawrUiFrame); lan_frame->set_size(Vector2(820, 280));
            lan_frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); lan_overlay->add_child(lan_frame);
            auto* message = memnew(EawrUiLabel); message->set_name("LanStatus");
            message->set_role("L_Text"); message->set_wrap(true);
            message->set_position(Vector2(24, 20)); message->set_size(Vector2(772, 190));
            message->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); lan_overlay->add_child(message);
            auto* leave = memnew(EawrUiButton); leave->set_name("LanLeave");
            leave->set_fit_to_rect(true);
            leave->set_text(String::utf8(hud->localized_text("TEXT_BUTTON_DISCONNECT_QUIT", "Leave").c_str())); leave->set_toggle_mode(true);
            leave->set_position(Vector2(280, 228)); leave->set_size(Vector2(200, 36));
            lan_overlay->add_child(leave);
            auto* rejoin = memnew(EawrUiButton); rejoin->set_name("LanRejoin");
            rejoin->set_fit_to_rect(true);
            rejoin->set_text("Rejoin"); rejoin->set_toggle_mode(true);
            rejoin->set_position(Vector2(590, 228)); rejoin->set_size(Vector2(200, 36));
            lan_overlay->add_child(rejoin);
            auto* save = memnew(EawrUiButton); save->set_name("LanSaveDiagnostic");
            save->set_fit_to_rect(true);
            save->set_text(String::utf8(hud->localized_text("TEXT_BUTTON_SAVE", "Save").c_str()));
            save->set_toggle_mode(true); save->set_position(Vector2(24, 228)); save->set_size(Vector2(200, 36));
            save->set_tooltip_text("Confirm automatically saved diagnostic details, or retry the local save"); lan_overlay->add_child(save);
        }
        auto view = live_session->lan_view();
        if (lan_recovered_notice.advance(view, live_session->presented_tick(), time.paused()) && options.lan) {
            options.lan->acknowledge_recovery(); view = live_session->lan_view();
        }
        const auto message = lan_message(view);
        const bool can_leave = lan_escape_aborts(view);
        auto* leave = static_cast<EawrUiButton*>(lan_overlay->get_node<Node>("LanLeave"));
        leave->set_visible(can_leave);
        if (leave->is_pressed()) {
            leave->set_pressed_no_signal(false);
            if (can_leave) live_session->quit();
        }
        auto* rejoin = static_cast<EawrUiButton*>(lan_overlay->get_node<Node>("LanRejoin"));
        const bool recovered = view.recovery == LanRecovery::recovered;
        rejoin->set_text(recovered ? String::utf8(hud->localized_text("TEXT_BUTTON_OK", "OK").c_str()) : String("Rejoin"));
        rejoin->set_visible(view.rejoin_enabled || recovered);
        if (rejoin->is_pressed()) {
            rejoin->set_pressed_no_signal(false);
            if (options.lan && recovered) options.lan->acknowledge_recovery();
            else if (view.rejoin_enabled && options.lan) (void)options.lan->rejoin();
        }
        auto* save = static_cast<EawrUiButton*>(lan_overlay->get_node<Node>("LanSaveDiagnostic"));
        save->set_visible(view.diagnostic_available);
        if (save->is_pressed()) {
            save->set_pressed_no_signal(false);
            if (view.diagnostic_available && options.lan) (void)options.lan->save_diagnostic_details();
        }
        lan_overlay->set_visible(!message.empty() && !results_shown);
        const auto incident_text = lan_incident_message(view);
        lan_incident->set_visible(!incident_text.empty() && !results_shown && lan_gameplay_enabled(view));
        static_cast<EawrUiLabel*>(lan_incident->get_node<Node>("LanIncidentText"))->set_text(String::utf8(incident_text.c_str()));
        auto* retry = static_cast<EawrUiButton*>(lan_incident->get_node<Node>("LanIncidentSave"));
        retry->set_visible(view.diagnostic_available);
        if (retry->is_pressed()) {
            retry->set_pressed_no_signal(false);
            if (view.diagnostic_available && options.lan) (void)options.lan->save_diagnostic_details();
        }
        const auto size = perf_host->get_viewport()->get_visible_rect().size;
        const auto layout = fit_letterbox_canvas(*lan_overlay, size, Vector2(1280, 720));
        lan_incident->set_scale(Vector2(layout.scale, layout.scale));
        static_cast<EawrUiLabel*>(lan_overlay->get_node<Node>("LanStatus"))->set_text(String::utf8(message.c_str()));

    }
    if (results_shown) {
        auto results = live_session->results();
        results.status_message = live_session->multiplayer() ? lan_message(lan) : std::string{};
        hud->set_results(results);
    }
    hud->set_battle(departure ? lan.victory : end ? std::optional<bool>(end->result == presentation::ui::BattleResult::victory) : std::nullopt,
                    departure || (end && end->ended_frame));
}

// #5355 (TP-MP06): who changed the shared battle speed, for five presentation seconds, and while the agreed rate is not
// the nominal 30, what really runs (lockstep follows the slowest seat and never skips a tick, MP-31).
void MapMode::State::update_speed_notice(const bool running, const LanView& lan,
    const std::optional<platform::TacticalFeed::AgreedPacing>& agreed) {
    namespace ui = presentation::ui;
    if (!speed_notice) {
        if (!perf_host || !hud || !hud->hud()) return;
        auto* layer = memnew(CanvasLayer); layer->set_layer(41); perf_host->add_child(layer);
        auto* label = memnew(EawrUiLabel); label->set_name("SharedSpeedNotice"); label->set_role("L_Text");
        label->set_theme(hud->hud()->get_theme());
        label->set_position(Vector2(24, 104)); label->set_size(Vector2(900, 48)); label->set_wrap(true);
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        layer->add_child(label);
        speed_notice = label;
    }
    const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    std::uint32_t requested = ui::nominal_rate;
    bool advancing = running;
    // A speed event is formatted once, when its identity is new: the GC child's clock event count (lock-free; the
    // owner view is read only for a new one), or the skirmish options change tick.
    if (agreed) {
        requested = agreed->target_rate;
        advancing = advancing && !agreed->paused;
        if (options.shared_speed_notice) {
            std::string line;
            if (const auto revision = options.shared_speed_notice(speed_notice_text.revision(), line); revision != speed_notice_text.revision())
                speed_notice_text.change(revision, std::move(line), now_ms);
        }
    } else {
        requested = ui::speed_step_rates[std::min<std::size_t>(lan.options.tactical_speed, ui::speed_step_rates.size() - 1)];
        advancing = advancing && lan.phase == LanPhase::running && !lan.clock_paused;
        if (lan.options_actor && lan.options_change_tick != speed_notice_text.revision())
            speed_notice_text.change(lan.options_change_tick,
                lan.options_name + " set the battle speed to " + ui::speed_factor_label(requested), now_ms);
    }
    if (!speed_notice_text.refresh(static_cast<std::uint64_t>(live_session->presented_tick()), now_ms, requested, advancing)) return;
    auto* label = static_cast<EawrUiLabel*>(speed_notice);
    label->set_text(String::utf8(speed_notice_text.text().c_str()));
    label->set_visible(!speed_notice_text.text().empty());
}

} // namespace eawr::presentation::godot_backend

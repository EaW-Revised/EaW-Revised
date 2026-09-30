// The tactical HUD over a map (P2-20a, #83): `--eawr-hud tactical` draws the
// space HUD shell for `--eawr-hud-faction <empire|rebel|underworld>` (default
// rebel), laid out by `--eawr-hud-rules <aspect|retail>` (default aspect, D4).
// A live session (#316) draws it by default for its local player's faction;
// `--eawr-hud off` leaves it out.
// Its fonts come from `--eawr-font-cache <dir>`, EAWR_FONT_CACHE or the
// checkout's out/fonts, as for `--eawr-ui-gallery`.

#include "map_mode_internal.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include "eawr/skirmish/start.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <span>

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
    return presentation::ui::load_font_cache(mounted ? mounted.value() : unmounted, directory.generic_string());
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

void MapMode::State::sync_perf_overlay() {
    if (perf == nullptr || !perf->shown()) return;
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

bool MapMode::State::build_hud(Node3D& host, const std::optional<std::string>& context_name) {
    // #558: the overlay's parent; `--eawr-perf-overlay on` shows it from the first frame.
    perf_host = &host;
    if (perf_requested) set_perf_overlay(true);
    if (!hud_options) return true;
    if (live_session && !hud_faction_given) {
        // The live session's local player; a faction without a shell variant keeps the default.
        if (const auto faction = presentation::ui::hud_faction_from(live_session->local_faction())) {
            hud_options->faction = *faction;
        }
    }
    hud = std::make_unique<TacticalHud>(*hud_options);
    // Over the 3D view on its own canvas layer. The host routes world input after
    // the GUI, so the HUD's hit mask keeps its clicks off the world (UI-I1).
    if (!hud->build(*filesystem, catalog ? &*catalog : nullptr, context_name, host)) {
        failure = "--eawr-hud tactical: " + hud->failure();
        hud.reset();
        return false;
    }
    host.set_process_input(true);
    host.set_process_unhandled_input(true);
    // #459 and #453: the time panel's buttons, the pause banner's Resume Game and the end panel's
    // Quit Game drive the live session; scripted hud=<name> gestures find them by name.
    if (live_session) {
        LiveSessionView* live = live_session.get();
        hud->set_time_handlers({[live] { live->press_pause(); }, [live] { live->press_fast_forward(); },
                                [live] { live->resume(); }, [live] { live->quit(); }});
        if (battle) {
            TacticalHud* named = hud.get();
            battle->set_hud_point([named](const std::string& name) { return named->control_point(name); });
        }
        sync_battle_hud();
    }
    // #425: the live battle's selection fills the command bar's unit cards, and a card click
    // changes the selection (presentation only).
    if (battle) {
        if (EawrUnitCards* cards = hud->unit_cards()) {
            BattleInput* input = battle.get();
            battle->set_card_slots(cards->slot_count());
            if (live_session) {
                const LiveSessionView* live = live_session.get();
                TacticalHud* card_hud = hud.get();
                cards->set_click([input, live, card_hud](const std::size_t slot, const bool shift) {
                    input->card_click(slot, shift, *live);
                    card_hud->set_unit_cards(input->card_layout(), input->card_units());
                    card_hud->set_ability_bar(input->ability_bar());
                });
            }
            if (live_session) {
                const LiveSessionView* live = live_session.get();
                cards->set_clock([live]() {
                    // As BattleInput::now: real time when interactive, the presented tick in a driven run.
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
    // #454: an ability button's release becomes the battle input's ability request (a report line
    // until #76); scripted ability=N gestures find the N-th shown button.
    if (battle && live_session) {
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
            [input, live](const double x, const double y) { static_cast<void>(input->minimap_move(x, y, *live)); });
        battle->set_minimap_point([minimap_hud](const double x, const double y) { return minimap_hud->minimap_point(x, y); });
    }
    return true;
}

void MapMode::State::sync_minimap() {
    if (!hud || !live_session || !battle || !space || hud->minimap() == nullptr) return;
    const auto bounds = space->live_camera_bounds();
    const auto& latest = live_session->battle_frame().latest;
    if (!bounds || !latest) return;
    const LiveSessionView& live = *live_session;
    if (minimap_type_names.empty() && live.tables() != nullptr) {
        for (const units::UnitType& type : live.tables()->units) minimap_type_names.emplace(skirmish::type_id(type.id), type.id);
    }
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
    view.extents = presentation::ui::minimap_extents(bounds->min_x, bounds->max_x, bounds->min_y, bounds->max_y);
    for (const LiveSessionView::VisibleUnit& unit : live.visible_units()) {
        presentation::ui::MinimapUnit blip;
        blip.id = unit.entity;
        const auto name = minimap_type_names.find(unit.type);
        if (name != minimap_type_names.end()) blip.type = name->second;
        // MM-07: the owner's lobby colour; a player without one (a map's Neutral or Pirates owner)
        // takes its faction's Factions.xml colour, else Neutral's grey.
        if (const auto colour = live.player_colour(unit.owner)) {
            blip.owner_colour = {(*colour)[0], (*colour)[1], (*colour)[2], 255};
        } else {
            blip.owner_colour = hud->faction_colour(live.player_faction(unit.owner)).value_or(data::ui::Rgba8{100, 100, 100, 255});
        }
        blip.hostile = unit.hostile;
        blip.selected = battle->selected(unit.entity);
        blip.x = unit.position[0];
        blip.y = unit.position[1];
        blip.yaw_degrees = unit.yaw;
        view.units.push_back(std::move(blip));
    }
    // MM-10: the local player's fog cells where the battle has them (#494: the cells the fog in
    // the world draws), else the local team's units with a sensor range reveal.
    if (const auto& cells = live.battle_frame().fog) {
        const auto& rules = cells->rules;
        view.cells = presentation::ui::MinimapFogCells{to_double(rules.map_left), to_double(rules.map_top),
            to_double(rules.cell_size), rules.cells_wide, rules.cells_tall,
            std::shared_ptr<const std::vector<std::uint8_t>>(cells, &cells->values)};
    }
    const auto local_team = live.team_of(live.local_player());
    for (const sim::tactical::TacticalInstance& instance : latest->instances()) {
        if (!instance.reveal_range || !local_team || instance.team != *local_team) continue;
        view.revealers.push_back({to_double(instance.fixed_transform.rows[0][3]), to_double(instance.fixed_transform.rows[1][3]),
                                  to_double(*instance.reveal_range)});
    }
    view.ground = battle->ground_corners(minimap_height.value_or(0.0));
    // FW-14: `--eawr-live-reveal on` draws no fog plane in the world, so the minimap's fog layer
    // stays consistent with it (MM-10) and draws nothing either, without touching the fog cells
    // or revealers the session actually published (the sim stays presentation-untouched).
    view.fog = !live_options.reveal;
    hud->set_minimap(view);
}

void MapMode::State::sync_battle_hud() {
    if (!hud || !live_session) return;
    const presentation::ui::TimeControls& time = live_session->time();
    hud->set_time_view({time.paused(), time.state() == presentation::ui::TimeState::fast_forward, time.pause_enabled(),
                        time.fast_forward_enabled()});
    const auto& end = live_session->battle_end();
    hud->set_battle(end ? std::optional<bool>(end->result == presentation::ui::BattleResult::victory) : std::nullopt,
                    end && end->ended_frame);
}

} // namespace eawr::presentation::godot_backend

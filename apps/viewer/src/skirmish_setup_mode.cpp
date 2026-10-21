#include "eawr/battle/start.hpp"
#include "eawr/core/text.hpp"
#include "skirmish_setup_mode.hpp"
#include "lan_lobby_screen.hpp"
#include "lan_broadcast_source.hpp"
#include "battle_content.hpp"
#include "lan_match.hpp"
#include "lobby_retry.hpp"
#include "galactic_lobby_backend.hpp"
#include "eawr/scene/scene.hpp"
#include "profile_store.hpp"
#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include "startup_trace.hpp"
#include "viewer_path.hpp"
#include "ui/dialog_builder.hpp"
#include "ui/font_provider.hpp"
#include "ui/kit.hpp"
#include "ui/theme_builder.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/player_profile.hpp"
#include "eawr/presentation/ui/skirmish_setup.hpp"
#include "eawr/presentation/godot/renderer.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;
namespace {
// The LAN panel's address field starts on the port a hosted match listens on (src/platform/net/lan_match.hpp).
constexpr std::string_view default_lan_endpoint = "127.0.0.1:47624";
constexpr std::uint16_t default_lan_port = 47624;

String text(std::string_view value) { return String::utf8(value.data(), static_cast<int64_t>(value.size())); }
std::string utf8(const String& value) { return value.utf8().get_data(); }

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

void key(Key code) {
    for (bool pressed : {true, false}) {
        Ref<InputEventKey> event;
        event.instantiate();
        event->set_keycode(code);
        event->set_physical_keycode(code);
        event->set_pressed(pressed);
        Input::get_singleton()->parse_input_event(event);
    }
}

void navigate_popup(EawrUiCombo* combo, int target) {
    const auto focused = combo->get_popup()->get_focused_item();
    if (focused < target) key(KEY_DOWN);
    else if (focused > target) key(KEY_UP);
}
} // namespace

struct SkirmishSetupMode::State final {
    explicit State(Options value) : options(std::move(value)),
        shaders(options.cache_shaders ? std::make_shared<GodotShaderCache>() : nullptr) {}
    ~State() { if (online_id && UtilityFunctions::is_instance_id_valid(online_id) && online_screen->get_parent() == nullptr) memdelete(online_screen); }
    EawrLanLobby* online_screen{};
    int64_t online_id{};
    bool online_mode{};
    Options options;
    Node3D* host{};
    CanvasLayer* layer{};
    ColorRect* background{};
    bool scene_running{};
    Control* root{};
    EawrUiList* list{};
    EawrUiList* game_types{};
    EawrUiButton *official{}, *custom{};
    std::vector<Control*> start_icons;
    TextureRect* preview{};
    EawrUiLabel* status{};
    EawrUiButton* start{};
    EawrUiCombo* connection{};
    EawrUiButton *lan_host{}, *lan_join{}, *lan_cancel{};
    EawrUiCheck* lan_port_mapping{};
    EawrUiEdit* endpoint{};
    EawrUiLabel *lan_status{}, *lan_addresses{};
    std::shared_ptr<LanMatch> lan;
    std::string lan_test, lan_join_endpoint;
    std::optional<std::uint16_t> lan_host_port;
    bool lan_smoke{};
    bool lan_acceptance{};
    std::string gc_acceptance_campaign;
    std::uint64_t gc_acceptance_ticks{};
    std::optional<std::uint64_t> gc_acceptance_seed;  // explicit override; otherwise the host draws one
    std::filesystem::path gc_acceptance_out;
    std::optional<std::chrono::steady_clock::time_point> gc_acceptance_setup_started;
    std::chrono::steady_clock::time_point gc_acceptance_next_log{};
    LobbyRetry gc_acceptance_ready_retry, gc_acceptance_start_retry;
    std::filesystem::path lan_trace;
    std::uint64_t lan_delay{3};
    EawrUiButton *advanced{}, *accept{}, *cancel{}, *defaults_button{}, *staging_cancel{};
    ColorRect *options_panel{}, *lan_review{};
    EawrUiLabel* lan_summary{};
    ScrollContainer* lan_scroll{};
    EawrUiCheck *heroes{}, *starting_units{}, *superweapons{};
    // MNU-22: the credits slider and its value, and the win-condition choice (WSS-25, WSS-31).
    EawrUiSlider* credits_slider{};
    EawrUiLabel* credits_value{};
    EawrUiCombo* win_condition{};
    std::optional<skirmish::CreditsBounds> credits_bounds;
    std::optional<BuiltDialog> staging, options_dialog;
    std::optional<model::ThemeModel> theme_model;
    Ref<Theme> theme;
    std::optional<data::ui::TextDatabase> text_database;
    std::optional<data::ui::TextLookup> text_lookup;
    // Faction names in the data's order (the faction registries), and the captions they show.
    std::vector<std::string> faction_names, faction_captions;
    std::string local_name, platform_name;
    // #2935: the player profile (docs/ui/player-profile.md): the defaults the screen opens with and what Start
    // writes back. Held whole, so a field this screen does not show (credits, timer, game name, land) survives.
    // Test and capture runs do not touch the user's profile unless --eawr-setup-profile names a file.
    ProfileStore profile_store;
    model::PlayerProfile player_profile;
    bool profile_live{};
    bool cancel_requested{}, cancel_routed{};
    battle::MatchOptions defaults;
    std::optional<skirmish::SetupOptionsEdit> editing;
    // The rows' controls: the name combo (Open or AI; the host's own name in row one), faction, team, colour.
    std::array<EawrUiCombo*, skirmish::local_setup_rows> faction{}, team{}, controller{}, colour{};
    std::size_t authored_rows{};
    std::shared_ptr<const vfs::Vfs> filesystem;
    std::shared_ptr<const data::Catalog> catalog;
    std::shared_ptr<const BattleContent> content;
    std::shared_ptr<GodotShaderCache> shaders;
    std::optional<data::ui::DialogCatalog> dialogs;
    std::optional<assets::MegaTextureAtlas> atlas;
    std::unique_ptr<FontProvider> fonts;
    std::unique_ptr<UiTextures> textures;
    // The front end shell's Escape: edge detection for process_back.
    bool escape_was_down{};
    // The advanced dialog's Escape: edge detection, and whether the win-condition list was open before.
    bool options_escape_was_down{}, win_list_was_open{};
    std::vector<skirmish::SetupMap> maps;
    skirmish::SetupMapCache all_maps;
    std::vector<battle::LobbyColour> palette;
    bool custom_maps{};
    skirmish::SetupSelection selection;
    std::optional<skirmish::FixtureOptions> pending;
    int selected{-1};
    std::uint32_t frames{};
    std::string benchmark_map;
    bool benchmark{};
    bool three_players{};
    bool nine_players{};
    std::uint32_t test_players{9};
    std::string three_player_test_map{"data/art/maps/_mp_space_ryloth.ted"};
    bool two_vs_two{};
    bool policy_test{};
    // #3167: report the controls' rectangles, open a row list and the advanced dialog, capture each.
    bool layout_test{};
    bool startup_metrics{};
    bool visible{true}, test{}, captured{}, capture_error_reported{}, bench_started{};
    std::string failure;
    std::string notice;

    // A caption from the text database (UI-T3: a missing key shows as written).
    [[nodiscard]] std::string lookup(std::string_view key);
    [[nodiscard]] String caption(model::SetupText id);
    [[nodiscard]] std::size_t live_rows() const;
    [[nodiscard]] bool row_live(std::size_t row) const { return row < live_rows(); }
    // WSS-14: the controller combo lists Open, Easy, Normal and Hard AI (indices 0 to 3); an occupied AI row is any
    // index above Open. The index picks the AI's difficulty level (WTA-47).
    static constexpr int controller_open = 0, controller_easy = 1, controller_normal = 2, controller_hard = 3;
    [[nodiscard]] static int controller_index(const skirmish::LobbySlot& record) noexcept {
        if (record.slot == 1) return controller_open;
        return record.difficulty == sim::tactical::AiLevel::easy ? controller_easy
            : record.difficulty == sim::tactical::AiLevel::hard ? controller_hard : controller_normal;
    }
    [[nodiscard]] static sim::tactical::AiLevel controller_level(int index) noexcept {
        return index == controller_easy ? sim::tactical::AiLevel::easy
            : index == controller_hard ? sim::tactical::AiLevel::hard : sim::tactical::AiLevel::normal;
    }
    void resolve_factions();
    void fill_map_list();
    void idle_row(std::size_t row);
    void process_map_kind();
    void process_cancel();
    void apply_profile();
    void record_profile();
    void process_lan();
    void refresh_map(int index);
    void filter_maps();
    void validate();
    void report_layout(const BuiltDialog& dialog, std::string_view id) const;
    void report_layouts() const;
    void refresh_options();
    void process_options();
    void process_back(bool was_editing);
    bool load();
    bool build();
};

std::string SkirmishSetupMode::State::lookup(const std::string_view key) {
    return text_lookup ? data::ui::to_utf8(text_lookup->text(key)) : std::string(key);
}

String SkirmishSetupMode::State::caption(const model::SetupText id) {
    return text(lookup(model::skirmish_setup_text_key(id)));
}

std::size_t SkirmishSetupMode::State::live_rows() const {
    if (selected < 0 || static_cast<std::size_t>(selected) >= maps.size()) return 0;
    return skirmish::setup_row_count(maps[static_cast<std::size_t>(selected)]);
}

// The factions the listed maps offer, in the order of the faction registries, each with the caption its
// Text_ID gives (the faction's own name when the data or the database gives none).
void SkirmishSetupMode::State::resolve_factions() {
    std::vector<std::pair<std::pair<std::size_t, std::size_t>, std::string>> found;
    std::set<std::string> seen;
    for (const auto& map : all_maps) {
        for (const auto& side : map.teams) {
            for (const auto& name : side.factions) {
                if (!seen.insert(name).second) continue;
                const auto* definition = catalog->find(name, data::Category::faction);
                const auto order = definition ? std::pair{definition->registry_order, definition->definition_order}
                                              : std::pair{std::size_t(-1), std::size_t(-1)};
                found.push_back({order, name});
            }
        }
    }
    std::sort(found.begin(), found.end());
    faction_names.clear();
    faction_captions.clear();
    for (const auto& entry : found) {
        std::string shown = entry.second;
        if (auto resolved = catalog->resolve(entry.second, data::Category::faction)) {
            if (const auto* id = resolved.value().value("Text_ID")) {
                std::string key = id->value.raw_text;
                key = core::trim(key, core::xml_whitespace);
                if (!key.empty()) shown = lookup(key);
            }
        }
        faction_names.push_back(entry.second);
        faction_captions.push_back(std::move(shown));
    }
    // The local player's name: the OS account stands in for the profile name until a profile page exists.
    for (const char* variable : {"USERNAME", "USER"}) {
        if (!local_name.empty()) break;
        const auto value = OS::get_singleton()->get_environment(variable);
        if (!value.is_empty()) local_name = utf8(value);
    }
    platform_name = local_name;
    if (local_name.empty()) local_name = lookup(model::skirmish_setup_text_key(model::SetupText::host_player));
}

bool SkirmishSetupMode::State::load() {
    const auto base = options.game_root / "GameData" / "Data";
    const auto expansion = options.game_root / "corruption" / "Data";
    std::vector<vfs::LayerRoot> roots;
    if (!std::filesystem::is_directory(base) || !std::filesystem::is_directory(expansion)) {
        failure = "Skirmish setup requires an installation containing GameData/Data and corruption/Data.";
        return false;
    }
    const std::string profile = options.profile.empty() ? (options.mod_root.empty() ? "foc" : "remake") : options.profile;
    if (profile != "foc" && profile != "remake") { failure = "Skirmish setup requires the FoC or mod profile."; return false; }
    if (profile == "remake") {
        if (options.mod_root.empty()) { failure = "The mod profile requires a mod root."; return false; }
        roots = vfs::mod_chain_roots(options.mod_root);
    }
    roots.emplace_back("expansion", expansion);
    roots.emplace_back("base", base);
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) { failure = core::format_diagnostic(chain.error()); return false; }
    std::vector<vfs::MountSpec> specs;
    std::vector<std::string> layers;
    for (auto& manifest : chain.value()) {
        layers.push_back(manifest.mount.layer_id);
        specs.push_back(std::move(manifest.mount));
    }
    auto mounted = vfs::Vfs::mount(specs);
    if (!mounted) { failure = core::format_diagnostic(mounted.error()); return false; }
    filesystem = std::make_shared<const vfs::Vfs>(std::move(mounted).value());
    auto objects = data::load_catalog_strict(*filesystem, profile == "foc" ? data::Profile::foc : data::Profile::remake);
    if (!objects) { failure = core::format_diagnostic(objects.error()); return false; }
    if (!objects.value().health.clean()) {
        UtilityFunctions::printerr(text("EAWR catalog load: " + data::describe_load_health(objects.value().health)));
    }
    catalog = std::make_shared<const data::Catalog>(std::move(objects.value().catalog));
    content = std::make_shared<const BattleContent>(BattleContent{filesystem, catalog, profile, std::move(layers)});
    auto colours = skirmish::read_lobby_colours(*filesystem);
    if (!colours) { failure = colours.error().message; return false; }
    palette = std::move(colours).value();
    auto match_defaults = skirmish::read_match_defaults(*filesystem);
    if (!match_defaults) { failure = match_defaults.error().message; return false; }
    defaults = std::move(match_defaults).value();
    // A mod's constants without the slider bounds keep the slider off rather than the screen closed.
    if (auto bounds = skirmish::read_credits_bounds(*filesystem)) credits_bounds = bounds.value();
    else UtilityFunctions::printerr(text("EAWR setup credits bounds: " + core::format_diagnostic(bounds.error())));
    selection.match = defaults;
    skirmish::SetupMapQuery query;
    query.custom.reset();
    auto found = all_maps.load(filesystem, catalog, query);
    if (!found) { failure = found.error().message; return false; }
    for (const auto& map : all_maps) if (map.metadata.custom == false) maps.push_back(map);
    auto skin = data::ui::load_dialog_catalog(*filesystem);
    if (!skin) { failure = skin.error().message; return false; }
    dialogs.emplace(std::move(skin).value());
    // Captions resolve through the text database (UI-T3); without one they show their keys.
    if (auto texts = data::ui::load_language_text_database(*filesystem, model::skirmish_setup_language)) {
        text_database.emplace(std::move(texts).value());
        text_lookup.emplace(*text_database);
    } else {
        UtilityFunctions::printerr(text("EAWR setup text database: " + core::format_diagnostic(texts.error())));
    }
    resolve_factions();
    auto page = assets::load_mega_texture_atlas(*filesystem, "data/art/textures/" + dialogs->skin.texture_file + ".mtd");
    if (!page) { failure = page.error().message; return false; }
    atlas.emplace(std::move(page).value());
    textures = std::make_unique<UiTextures>(*atlas, *filesystem);
    if (!textures->page_failure().empty()) { failure = textures->page_failure(); return false; }
    std::filesystem::path cache;
    bool profile_named{};
    const auto args = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--eawr-perf-trace") startup_metrics = true;
        if (args[i] == "--eawr-font-cache" && i + 1 < args.size()) cache = ViewerPath{utf8(args[++i])}.native();
        if (args[i] == "--eawr-lan-smoke") lan_smoke = true;
        if (args[i] == "--eawr-lan-acceptance") lan_smoke = lan_acceptance = true;
        if (args[i] == "--eawr-gc-lan-out" && i + 1 < args.size()) gc_acceptance_out = ViewerPath{utf8(args[++i])}.native();
        if (args[i] == "--eawr-gc-lan-acceptance" && i + 1 < args.size()) {
            gc_acceptance_campaign = utf8(args[++i]);
        }
        if (args[i] == "--eawr-galactic-seed" && i + 1 < args.size()) {
            const auto value = utf8(args[++i]);
            std::uint64_t seed{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seed);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                failure = "--eawr-galactic-seed expects a number"; return false;
            }
            gc_acceptance_seed = seed;
        }
        if (args[i] == "--eawr-gc-lan-final-tick" && i + 1 < args.size()) {
            const auto value = utf8(args[++i]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), gc_acceptance_ticks);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || gc_acceptance_ticks == 0 || gc_acceptance_ticks > net::max_session_ticks) {
                failure = "Invalid GC acceptance final tick"; return false;
            }
        }
        if (args[i] == "--eawr-lan-trace" && i + 1 < args.size()) lan_trace = ViewerPath{utf8(args[++i])}.native();
        if (args[i] == "--eawr-lan-input-delay" && i + 1 < args.size()) {
            const auto value = utf8(args[++i]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), lan_delay);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || lan_delay < 1 || lan_delay > 14) {
                failure = "--eawr-lan-input-delay expects 1 to 14"; return false;
            }
        }
        if (args[i] == "--eawr-lan-host" && i + 1 < args.size()) {
            const auto value = utf8(args[++i]);
            unsigned port{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port == 0 || port > 65535) {
                failure = "--eawr-lan-host expects a port from 1 to 65535"; return false;
            }
            lan_host_port = static_cast<std::uint16_t>(port);
        }
        if (args[i] == "--eawr-lan-join" && i + 1 < args.size()) lan_join_endpoint = utf8(args[++i]);
        if (args[i] == "--eawr-setup-test") test = true;
        if (args[i] == "--eawr-setup-nine-player-test") nine_players = true;
        if (args[i] == "--eawr-setup-eight-player-test") { nine_players = true; test_players = 8; }
        if (args[i] == "--eawr-setup-profile" && i + 1 < args.size()) {
            profile_store = ProfileStore{ViewerPath{utf8(args[++i])}.native()};
            profile_named = true;
        }
        if (args[i] == "--eawr-lan-panel-test" && i + 1 < args.size()) {
            lan_test = utf8(args[++i]);
            lan = make_fake_lan_match(lan_test);
        }
        if (args[i] == "--eawr-setup-three-player-test") three_players = true;
        if (args[i] == "--eawr-setup-ffa-map-test" && i + 1 < args.size())
            three_player_test_map = utf8(args[++i]);
        if (args[i] == "--eawr-setup-2v2-test") two_vs_two = true;
        if (args[i] == "--eawr-setup-policy-test") policy_test = true;
        if (args[i] == "--eawr-setup-layout-test") layout_test = true;
        if (args[i] == "--eawr-load-bench-map" && i + 1 < args.size()) {
            benchmark_map = utf8(args[++i]);
            benchmark = true;
        }
    }
    if ((!gc_acceptance_campaign.empty() || gc_acceptance_ticks || !gc_acceptance_out.empty())
        && (!lan_acceptance || gc_acceptance_campaign.empty() || !gc_acceptance_ticks || gc_acceptance_out.empty())) {
        failure = "GC acceptance needs --eawr-lan-acceptance, an output directory and final tick"; return false;
    }
    // The profile belongs to an interactive run: fixtures, captures, reports and LAN smoke runs keep the defaults.
    profile_live = profile_named || (!test && !two_vs_two && !three_players && !policy_test && !layout_test && !benchmark
        && options.report_path.empty() && options.capture_path.empty() && lan_test.empty() && !lan_smoke
        && !lan_host_port && lan_join_endpoint.empty());
    if (profile_live) {
        auto loaded = profile_store.load();
        for (const auto& note : loaded.notes) UtilityFunctions::print("EAWR setup profile note: ", text(note));
        player_profile = std::move(loaded.profile);
        local_name = model::effective_player_name(player_profile, platform_name);
    }
    if (cache.empty()) {
        const auto environment = OS::get_singleton()->get_environment("EAWR_FONT_CACHE");
        cache = environment.is_empty()
            ? (ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://"))}.native()
               / ".." / ".." / ".." / "out" / "fonts").lexically_normal()
            : ViewerPath{utf8(environment)}.native();
    }
    const vfs::MountSpec font_spec{.layer_id = "font-cache", .data_root = cache,
        .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto mounted_fonts = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&font_spec, 1));
    const vfs::Vfs empty;
    fonts = std::make_unique<FontProvider>(model::load_font_cache(mounted_fonts ? mounted_fonts.value() : empty, ViewerPath::utf8(cache)));
    return true;
}

namespace {

[[nodiscard]] Control* gadget_control(const BuiltDialog& dialog, const std::string_view id) {
    for (const auto& item : dialog.gadgets) {
        if (item.id == id) return item.control;
    }
    return nullptr;
}

[[nodiscard]] const BuiltGadget* gadget_of(const BuiltDialog& dialog, const std::string_view id) {
    for (const auto& item : dialog.gadgets) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

template <typename Type>
[[nodiscard]] Type* gadget_as(const BuiltDialog& dialog, const std::string_view id) {
    return Object::cast_to<Type>(gadget_control(dialog, id));
}

} // namespace

void SkirmishSetupMode::State::fill_map_list() {
    list->clear();
    for (const auto& map : maps) {
        // An unavailable map stays listed with the database's "Unavailable" and its own reason as the tooltip.
        list->add_item(map.unavailable.empty() ? text(map.name)
                                               : text(map.name) + String(" (") + caption(model::SetupText::unavailable) + String(")"));
        list->set_item_tooltip(list->get_item_count() - 1, text(map.unavailable.empty() ? map.path : map.unavailable));
    }
}

// A row the map does not offer or nobody occupies: shown, blank and disabled as the retail rows beyond
// the map's capacity are.
void SkirmishSetupMode::State::idle_row(const std::size_t row) {
    for (auto* combo : {faction[row], team[row], controller[row], colour[row]}) {
        combo->set_visible(row < authored_rows || row_live(row));
        combo->set_disabled(true);
    }
}

bool SkirmishSetupMode::State::build() {
    layer = memnew(CanvasLayer);
    layer->set_layer(10);
    host->add_child(layer);
    background = memnew(ColorRect);
    background->set_color(Color(0.018F, 0.027F, 0.055F));
    background->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    layer->add_child(background);
    root = memnew(Control);
    root->set_size(Vector2(1280, 720));
    // Scaled to the window by its transform: the text is rasterised at the scale it is drawn at (#3176).
    root->set_oversampling_with_scale(CanvasItem::OVERSAMPLING_WITH_SCALE_ENABLED);
    background->add_child(root);
    model::ThemeSources sources;
    sources.catalog = &*dialogs;
    sources.atlas = &atlas->directory;
    sources.standalone = model::vfs_standalone_textures(*filesystem);
    sources.fonts = &fonts->cache();
    const auto& families = fonts->system_families();
    sources.system = [&families](std::string_view face) { return model::match_system_face(face, families).has_value(); };
    // UI-L3 and UI-L4, retail rules: the staging dialog is stretched and centred as the retail capture is
    // (R-SETUP-01), so the catalogue's rectangles stand for the screen's own placement.
    theme_model.emplace(model::build_theme_model(sources, model::reference_space({1280, 720}, model::LayoutRules::retail)));
    theme = build_theme(*theme_model, *textures, *fonts);
    root->set_theme(theme);

    const auto* staging_resource = dialogs->script.find(std::string(model::skirmish_staging_dialog));
    const auto* options_resource = dialogs->script.find(std::string(model::skirmish_options_dialog));
    if (staging_resource == nullptr || options_resource == nullptr) {
        failure = "Skirmish setup requires the dialogs " + std::string(model::skirmish_staging_dialog) + " and "
            + std::string(model::skirmish_options_dialog) + " in the dialog resources.";
        return false;
    }
    data::ui::TextLookup* lookup_ptr = text_lookup ? &*text_lookup : nullptr;
    // Every combo keeps its resource rectangle (the retail columns' split); its text clips (#3167).
    staging.emplace(build_dialog(*staging_resource, *theme_model, lookup_ptr, model::Placement::centre, true));
    options_dialog.emplace(build_dialog(*options_resource, *theme_model, lookup_ptr, model::Placement::centre, true));
    // The skin's tooltips, through the database like the captions.
    const auto tooltips = [&](const data::ui::Dialog& resource, BuiltDialog& built) {
        for (std::size_t i = 0; i < resource.controls.size() && i < built.gadgets.size(); ++i) {
            const auto* tip = dialogs->tooltip(resource.controls[i]);
            if (tip != nullptr && built.gadgets[i].control != nullptr)
                built.gadgets[i].control->set_tooltip_text(text(lookup(tip->text_id)));
        }
    };
    tooltips(*staging_resource, *staging);
    tooltips(*options_resource, *options_dialog);
    // A group box draws its border only: the builder's frame has an interior fill, so it goes behind the
    // controls it surrounds (the resource lists it last).
    for (auto* built : {&*staging, &*options_dialog}) {
        for (const auto& item : built->gadgets) {
            if (item.kind == "group" && item.control != nullptr) built->frame->move_child(item.control, 0);
        }
    }
    // A group box is its rim alone: the dialog's own shade shows inside it, as in the retail advanced dialog.
    for (const auto& item : options_dialog->gadgets) {
        if (item.kind != "group") continue;
        if (auto* rim = Object::cast_to<EawrUiFrame>(item.control)) rim->set_rim_only(true);
    }

    const auto missing = [&](const BuiltDialog& built, std::initializer_list<std::string_view> ids) {
        for (const auto id : ids) {
            if (gadget_control(built, id) == nullptr) {
                failure = "Skirmish setup requires the control " + std::string(id) + " in its dialog.";
                return true;
            }
        }
        return false;
    };
    if (missing(*staging, {"IDC_GAME_TYPE_LIST", "IDC_STAGING_MAP_LIST", "IDC_SELECT_OFFICIAL_MAPS",
            "IDC_SELECT_CUSTOM_MAPS", "IDC_SELECT_OPTIONS", "IDC_CANCEL_BUTTON", "IDC_SKIRMISH_START_GAME_BUTTON",
            "IDC_PLAYER_NAME_7", "IDC_PLAYER_FACTION_7", "IDC_PLAYER_COLOR_7", "IDC_PLAYER_TEAM_7"})
        || missing(*options_dialog, {"IDC_ALLOW_HEROES", "IDC_ALLOW_STARTING_UNIT_PURCHASE", "IDC_ALLOW_SUPER_WEAPONS",
            "IDC_DEFAULTS_BUTTON", "IDC_BUTTON_ACCEPT", "IDC_CANCEL_BUTTON"})) return false;
    root->add_child(staging->frame);

    // Controls the retail dialog has and the screen finds by their resource names.
    game_types = gadget_as<EawrUiList>(*staging, "IDC_GAME_TYPE_LIST");
    list = gadget_as<EawrUiList>(*staging, "IDC_STAGING_MAP_LIST");
    official = gadget_as<EawrUiButton>(*staging, "IDC_SELECT_OFFICIAL_MAPS");
    custom = gadget_as<EawrUiButton>(*staging, "IDC_SELECT_CUSTOM_MAPS");
    advanced = gadget_as<EawrUiButton>(*staging, "IDC_SELECT_OPTIONS");
    staging_cancel = gadget_as<EawrUiButton>(*staging, "IDC_CANCEL_BUTTON");
    start = gadget_as<EawrUiButton>(*staging, "IDC_SKIRMISH_START_GAME_BUTTON");
    // Polled each frame (a click presses and releases within one input flush).
    for (auto* button : {official, custom, advanced, staging_cancel}) button->set_toggle_mode(true);
    start->set_focus_mode(Control::FOCUS_ALL);
    official->set_pressed_no_signal(true);

    // The game-type list shares the map list's column and stops where the map list starts (the resource's
    // rectangle runs a few units into it).
    game_types->set_size(Vector2(list->get_size().x,
        std::min(game_types->get_size().y, list->get_position().y - game_types->get_position().y)));
    // The game types: land and land control stay listed and disabled, with the reason as their tooltip.
    for (const auto& row : model::setup_game_types()) {
        game_types->add_item(caption(row.text));
        const int index = game_types->get_item_count() - 1;
        if (!row.available) {
            game_types->set_item_disabled(index, true);
            game_types->set_item_tooltip(index, caption(model::SetupText::unavailable) + String(": ")
                + text(model::setup_notice(model::SetupNotice::land_not_built)));
        }
        if (row.type == model::setup_default_game_type()) game_types->select(index);
    }

    fill_map_list();

    // The preview fills the dialog's image slot; the builder leaves the slot's rectangle.
    const auto* image = gadget_of(*staging, "IDC_IMAGE");
    const model::PixelRect slot = image != nullptr ? image->rect : model::PixelRect{};
    preview = memnew(TextureRect);
    preview->set_name("EawrSetupPreview");
    preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    preview->set_stretch_mode(TextureRect::STRETCH_SCALE);
    preview->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    // R-SETUP-01: keep the preview on the canonical root transform. Nesting it
    // below the fractional dialog origin changes texture rasterization by a
    // pixel, despite equal global rectangles. The loaded slot still anchors it
    // for every mod, and the root supplies viewport scaling and visibility.
    const Vector2 slot_position(static_cast<float>(slot.x), static_cast<float>(slot.y));
    preview->set_position(slot_position + Vector2(2, 2));
    preview->set_size(Vector2(std::max(0.0F, static_cast<float>(slot.width) - 4.0F),
        std::max(0.0F, static_cast<float>(slot.height) - 4.0F)));
    preview->set_visible(image != nullptr);
    auto* preview_frame = memnew(EawrUiFrame);
    preview_frame->set_name("EawrSetupPreviewFrame");
    preview_frame->set_small(true);
    preview_frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    preview_frame->set_position(slot_position);
    preview_frame->set_size(Vector2(static_cast<float>(slot.width), static_cast<float>(slot.height)));
    preview_frame->set_visible(image != nullptr);
    root->add_child(preview_frame);
    // The image goes above the frame's interior fill and keeps its border.
    root->add_child(preview);

    // The status line sits in the free band under the team column; the retail dialog has no such control
    // (its start checks open a message box, which this screen does not have yet).
    status = memnew(EawrUiLabel);
    status->set_name("EawrSetupStatus");
    status->set_role("L_Text");
    status->set_wrap(true);
    status->set_clip_contents(true);
    status->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    {
        const auto authored = [&](const std::string_view id) {
            for (const auto& control : staging_resource->controls) {
                if (control.id_name == id)
                    return model::RcRect{control.rect.x, control.rect.y, control.rect.width, control.rect.height};
            }
            return model::RcRect{};
        };
        const std::int32_t buttons_top = std::min({authored("IDC_CANCEL_BUTTON").y,
            authored("IDC_SELECT_OPTIONS").y, authored("IDC_SKIRMISH_START_GAME_BUTTON").y});
        std::int32_t footer_top = buttons_top;
        if (auto* hint = gadget_control(*staging, "IDC_HINTS_SMALL")) {
            footer_top = std::min(footer_top, authored("IDC_HINTS_SMALL").y);
            // An authored hint's height may include the footer. Stop before its buttons.
            hint->set_size(Vector2(hint->get_size().x, std::max(0.0F,
                std::min(hint->get_size().y, static_cast<float>(buttons_top - authored("IDC_HINTS_SMALL").y)
                    * static_cast<float>(staging->layout.scale_y)))));
        }
        const auto resource_band = model::staging_status_band(authored("IDC_PLAYER_NAME_7"),
            authored("IDC_PLAYER_NAME_6"), authored("IDC_PLAYER_TEAM_7"), footer_top);
        const auto band = model::place_gadget(staging->layout, resource_band);
        status->set_position(Vector2(static_cast<float>(band.x - staging->layout.frame.x),
            static_cast<float>(band.y - staging->layout.frame.y)));
        status->set_size(Vector2(static_cast<float>(band.width), static_cast<float>(band.height)));
        status->set_visible(resource_band.width > 0 && resource_band.height > 0);
        staging->frame->add_child(status);
    }

    while (authored_rows < skirmish::local_setup_rows
        && gadget_control(*staging, "IDC_PLAYER_NAME_" + std::to_string(authored_rows)) != nullptr) ++authored_rows;
    // R-SETUP-01: extend missing controls from the last authored row's geometry and skin.
    // refresh_map fits extra live rows above the existing status band.
    for (const std::string column : {"IDC_PLAYER_NAME_", "IDC_PLAYER_FACTION_", "IDC_PLAYER_COLOR_", "IDC_PLAYER_TEAM_"}) {
        for (std::size_t row = 2; row < skirmish::local_setup_rows; ++row) {
            const std::string id = column + std::to_string(row);
            if (gadget_control(*staging, id) != nullptr) continue;
            const auto* previous = gadget_of(*staging, column + std::to_string(row - 1));
            const auto* before = gadget_of(*staging, column + std::to_string(row - 2));
            if (previous == nullptr || before == nullptr || previous->kind != "combo") {
                failure = "Skirmish setup cannot extend the player row " + id;
                return false;
            }
            BuiltGadget added = *previous;
            added.id = id;
            added.rect.y += previous->rect.y - before->rect.y;
            auto* combo = memnew(EawrUiCombo);
            combo->set_name(text(id));
            combo->set_theme_type_variation(text(added.variation));
            combo->set_fit_to_longest_item(false);
            combo->set_clip_text(true);
            combo->set_retail_list(true);
            combo->set_position(previous->control->get_position()
                + Vector2(0, static_cast<float>(added.rect.y - previous->rect.y)));
            combo->set_size(previous->control->get_size());
            staging->frame->add_child(combo);
            added.control = combo;
            staging->gadgets.push_back(std::move(added));
        }
    }
    // The player rows: name combo (Open or AI; the local player's name in row one), faction, colour, team.
    for (std::size_t slot_index = 0; slot_index < skirmish::local_setup_rows; ++slot_index) {
        const std::string suffix = std::to_string(slot_index);
        controller[slot_index] = gadget_as<EawrUiCombo>(*staging, "IDC_PLAYER_NAME_" + suffix);
        faction[slot_index] = gadget_as<EawrUiCombo>(*staging, "IDC_PLAYER_FACTION_" + suffix);
        colour[slot_index] = gadget_as<EawrUiCombo>(*staging, "IDC_PLAYER_COLOR_" + suffix);
        team[slot_index] = gadget_as<EawrUiCombo>(*staging, "IDC_PLAYER_TEAM_" + suffix);
        for (auto* combo : {faction[slot_index], colour[slot_index], team[slot_index], controller[slot_index]})
            combo->set_focus_mode(Control::FOCUS_ALL);
        for (const auto& shown : faction_captions) faction[slot_index]->add_item(text(shown));
        faction[slot_index]->select(slot_index == 0 || faction_names.size() < 2 ? 0 : 1);
        // The retail rows show the colour alone, as a bar centred in the box's text area (about a quarter
        // of the box's width and height in the retail capture), so it scales with the box.
        const Vector2 box = colour[slot_index]->get_size();
        const int bar_width = std::max(1, static_cast<int>(std::lround(box.x * 0.27F)));
        const int bar_height = std::max(1, static_cast<int>(std::lround(box.y * 0.25F)));
        colour[slot_index]->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
        for (std::size_t i = 0; i < palette.size(); ++i) {
            const auto& rgb = palette[i].rgb;
            auto swatch = Image::create_empty(bar_width, bar_height, false, Image::FORMAT_RGBA8);
            swatch->fill(Color(rgb[0] / 255.0F, rgb[1] / 255.0F, rgb[2] / 255.0F));
            colour[slot_index]->add_icon_item(ImageTexture::create_from_image(swatch), String(), static_cast<int>(i));
        }
        colour[slot_index]->select(static_cast<int>(std::min(slot_index, palette.size() - 1)));
        if (slot_index == 0) {
            controller[slot_index]->add_item(text(local_name));
            controller[slot_index]->set_disabled(true);
        } else {
            controller[slot_index]->add_item(caption(model::SetupText::open_player));
            controller[slot_index]->add_item(caption(model::SetupText::ai_easy_player));
            controller[slot_index]->add_item(caption(model::SetupText::ai_player));
            controller[slot_index]->add_item(caption(model::SetupText::ai_hard_player));
            controller[slot_index]->select(slot_index == 1 ? controller_normal : controller_open);
        }
    }

    // The LAN panel is this project's own: below the dialog, outside the retail layout.
    const auto place = [&](Control* control, float x, float y, float w, float h) {
        root->add_child(control);
        control->set_position(Vector2(x, y));
        control->set_size(Vector2(w, h));
        return control;
    };
    const auto label = [&](const String& value, float x, float y, float w, float h = 28.0F) {
        auto* item = memnew(EawrUiLabel);
        item->set_text(value);
        item->set_role("L_Text");
        item->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        place(item, x, y, w, h);
        return item;
    };
    connection = memnew(EawrUiCombo);
    connection->add_item(caption(model::SetupText::single_player));
    connection->add_item(caption(model::SetupText::lan));
    place(connection, 58, 578, 180, 28);
    lan_host = memnew(EawrUiButton); lan_host->set_text(caption(model::SetupText::lan_host)); lan_host->set_toggle_mode(true);
    place(lan_host, 250, 578, 130, 28);
    endpoint = memnew(EawrUiEdit); endpoint->set_text(text(default_lan_endpoint));
    place(endpoint, 396, 578, 250, 28);
    lan_join = memnew(EawrUiButton); lan_join->set_text(caption(model::SetupText::lan_join)); lan_join->set_toggle_mode(true);
    place(lan_join, 662, 578, 130, 28);
    lan_cancel = memnew(EawrUiButton); lan_cancel->set_text(caption(model::SetupText::lan_cancel)); lan_cancel->set_toggle_mode(true);
    place(lan_cancel, 808, 578, 160, 28);
    lan_port_mapping = memnew(EawrUiCheck); lan_port_mapping->set_text(text(model::setup_notice(model::SetupNotice::port_mapping)));
    lan_port_mapping->set_tooltip_text(text(model::setup_notice(model::SetupNotice::port_mapping_tooltip)));
    place(lan_port_mapping, 980, 578, 238, 28);
    lan_status = label(String(), 58, 616, 1160, 44); lan_status->set_wrap(true);
    lan_addresses = label(String(), 58, 666, 1160, 26);
    lan_review = memnew(ColorRect); lan_review->set_color(Color(0.025F, 0.045F, 0.07F, 1.0F));
    place(lan_review, 48, 172, 1184, 338);
    lan_summary = label(String(), 66, 184, 1140, 314); lan_summary->set_wrap(true);
    lan_scroll = memnew(ScrollContainer); place(lan_scroll, 66, 184, 1140, 314);
    root->remove_child(lan_summary); lan_scroll->add_child(lan_summary); lan_summary->set_position(Vector2());
    lan_summary->set_custom_minimum_size(Vector2(1120, 314));
    lan_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
    lan_review->hide(); lan_scroll->hide();
    if (!lan_test.empty()) connection->select(1);

    // The advanced options: the catalogue's dialog over the staging, which retail leaves undimmed behind its
    // translucent frame; the transparent panel only takes the pointer. Edits go to a copy (R-SETUP-05).
    options_panel = memnew(ColorRect);
    options_panel->set_color(Color(0.0F, 0.0F, 0.0F, 0.0F));
    place(options_panel, 0, 0, 1280, 720);
    options_panel->add_child(options_dialog->frame);
    heroes = gadget_as<EawrUiCheck>(*options_dialog, "IDC_ALLOW_HEROES");
    starting_units = gadget_as<EawrUiCheck>(*options_dialog, "IDC_ALLOW_STARTING_UNIT_PURCHASE");
    superweapons = gadget_as<EawrUiCheck>(*options_dialog, "IDC_ALLOW_SUPER_WEAPONS");
    accept = gadget_as<EawrUiButton>(*options_dialog, "IDC_BUTTON_ACCEPT");
    cancel = gadget_as<EawrUiButton>(*options_dialog, "IDC_CANCEL_BUTTON");
    defaults_button = gadget_as<EawrUiButton>(*options_dialog, "IDC_DEFAULTS_BUTTON");
    for (auto* check : {heroes, starting_units, superweapons}) check->set_toggle_mode(true);
    for (auto* button : {defaults_button, accept, cancel}) button->set_toggle_mode(true);
    // WSS-27: the pre-built base is hidden in space.
    if (auto* control = gadget_control(*options_dialog, "IDC_PRE_BUILT_BASE")) control->set_visible(false);
    // WSS-31: the win condition offers the two space choices, captioned from the database.
    win_condition = gadget_as<EawrUiCombo>(*options_dialog, "IDC_WIN_CONDITION_COMBO");
    if (win_condition != nullptr) {
        win_condition->set_focus_mode(Control::FOCUS_ALL);
        for (const auto& choice : model::setup_win_conditions()) win_condition->add_item(text(lookup(choice.text_key)));
        win_condition->select(-1);
    }
    // WSS-25: the credits display clamps into the data's bounds without rounding. Its value is
    // not consumed by a battle yet, so it shows the data's default, disabled, with that reason.
    credits_slider = gadget_as<EawrUiSlider>(*options_dialog, "IDC_SKIRMISH_CREDITS_SLIDER");
    credits_value = gadget_as<EawrUiLabel>(*options_dialog, "IDC_SKIRMISH_CREDITS_VALUE");
    const bool credits_shown = credits_slider != nullptr && credits_bounds.has_value();
    for (const std::string_view id : {"IDC_SKIRMISH_CREDITS_TEXT", "IDC_SKIRMISH_CREDITS_SLIDER", "IDC_SKIRMISH_CREDITS_VALUE"}) {
        if (auto* control = gadget_control(*options_dialog, id)) control->set_visible(credits_shown);
    }
    if (credits_shown) {
        const String reason = text(std::string(model::setup_notice(model::SetupNotice::credits_not_applied)));
        credits_slider->set_min(static_cast<double>(credits_bounds->minimum));
        credits_slider->set_max(static_cast<double>(credits_bounds->maximum));
        // This disabled slider displays an authored value, rather than a slider edit (WSS-25).
        // A step here would make Godot round an off-grid mod default before drawing it.
        credits_slider->set_step(0.0);
        credits_slider->set_forced_states(KitState::disabled, KitState::disabled);
        credits_slider->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        credits_slider->set_tooltip_text(reason);
        // A slider has no disabled state of its own: a transparent control over it takes the pointer and
        // carries the reason.
        auto* cover = memnew(Control);
        cover->set_name("EawrSetupCreditsReason");
        cover->set_position(credits_slider->get_position());
        cover->set_size(credits_slider->get_size());
        cover->set_mouse_filter(Control::MOUSE_FILTER_STOP);
        cover->set_tooltip_text(reason);
        options_dialog->frame->add_child(cover);
    }
    options_panel->hide();

    int initial = 0;
    for (std::size_t i = 0; i < maps.size(); ++i) if (maps[i].path == skirmish::m2_fixture().map) initial = static_cast<int>(i);
    if (benchmark) {
        for (std::size_t i = 0; i < maps.size(); ++i)
            if (maps[i].path == benchmark_map) initial = static_cast<int>(i);
    }
    list->select(initial);
    list->ensure_current_is_visible();
    refresh_map(initial);
    apply_profile();
    return true;
}

// The remembered space choices as the screen's defaults (WSS-02, PRF-03): the map, and the local faction,
// colour and team. Names the data lacks are ignored, so a mod that dropped them falls back to the screen's own.
void SkirmishSetupMode::State::apply_profile() {
    if (!profile_live) return;
    const auto& choice = player_profile.skirmish.space;
    const auto kind = player_profile.skirmish.game_type;
    const auto rows = model::setup_game_types();
    if (kind >= 0 && static_cast<std::size_t>(kind) < rows.size() && rows[static_cast<std::size_t>(kind)].available)
        game_types->select(kind);
    if (!choice.map.empty()) {
        const auto found = std::find_if(all_maps.begin(), all_maps.end(),
            [&](const skirmish::SetupMap& map) { return map.path == choice.map && map.unavailable.empty(); });
        if (found != all_maps.end()) {
            const bool custom_map = found->metadata.custom == true;
            if (custom_map != custom_maps) {
                custom_maps = custom_map;
                official->set_pressed_no_signal(!custom_maps);
                custom->set_pressed_no_signal(custom_maps);
                selection.map = found->path;
                filter_maps();
            } else {
                for (std::size_t i = 0; i < maps.size(); ++i) {
                    if (maps[i].path != found->path) continue;
                    list->select(static_cast<int>(i));
                    list->ensure_current_is_visible();
                    refresh_map(static_cast<int>(i));
                    break;
                }
            }
        }
    }
    if (selected < 0) return;
    const auto& map = maps[static_cast<std::size_t>(selected)];
    // The team first: which factions the row may take depends on it.
    for (int i = 0; i < team[0]->get_item_count(); ++i)
        if (team[0]->get_item_id(i) == choice.team) team[0]->select(i);
    if (!choice.faction.empty() && team[0]->get_selected() >= 0) {
        const auto side = std::find_if(map.teams.begin(), map.teams.end(),
            [&](const auto& entry) { return static_cast<int>(entry.team) == team[0]->get_selected_id(); });
        const auto name = std::find(faction_names.begin(), faction_names.end(), choice.faction);
        if (name != faction_names.end() && side != map.teams.end()
            && std::find(side->factions.begin(), side->factions.end(), choice.faction) != side->factions.end())
            faction[0]->select(static_cast<int>(name - faction_names.begin()));
    }
    if (choice.colour >= 0 && static_cast<std::size_t>(choice.colour) < palette.size()) colour[0]->select(choice.colour);
    validate();
}

// Start (not Cancel) writes the choices back: the whole loaded record, with this screen's fields changed.
void SkirmishSetupMode::State::record_profile() {
    if (!profile_live || selected < 0) return;
    const auto host_slot = std::find_if(selection.slots.begin(), selection.slots.end(),
        [](const auto& entry) { return entry.slot == 1; });
    if (host_slot == selection.slots.end()) return;
    auto choices = player_profile.skirmish;
    choices.space.map = maps[static_cast<std::size_t>(selected)].path;
    choices.space.faction = host_slot->faction;
    choices.space.colour = static_cast<int>(host_slot->colour_index.value_or(0));
    choices.space.team = static_cast<int>(host_slot->team);
    const auto picked = game_types->get_selected_items();
    choices.game_type = picked.is_empty() ? model::game_type_default : static_cast<int>(picked[0]);
    model::record_skirmish_choices(player_profile, choices);
    std::string error;
    if (!profile_store.save(player_profile, error))
        UtilityFunctions::printerr(text("EAWR setup profile not saved: " + error));
}

void SkirmishSetupMode::State::filter_maps() {
    const auto previous = selection.map;
    maps.clear();
    for (const auto& map : all_maps) {
        if (map.metadata.custom != custom_maps) continue;
        maps.push_back(map);
    }
    fill_map_list();
    selected = -1;
    selection.map.clear();
    preview->set_texture(Ref<ImageTexture>());
    for (auto* icon : start_icons) { icon->hide(); icon->queue_free(); }
    start_icons.clear();
    if (maps.empty()) {
        for (auto* value : team) value->clear();
        for (std::size_t row = 0; row < skirmish::local_setup_rows; ++row) idle_row(row);
        start->set_disabled(true);
        status->set_text(String());
        return;
    }
    int initial = 0;
    for (std::size_t i = 0; i < maps.size(); ++i) if (maps[i].path == previous) initial = static_cast<int>(i);
    list->select(initial);
    list->ensure_current_is_visible();
    refresh_map(initial);
}

void SkirmishSetupMode::State::refresh_map(const int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= maps.size()) return;
    selected = index;
    const auto& map = maps[static_cast<std::size_t>(index)];
    skirmish::repair_setup_selection(selection, map);
    notice.clear();
    const auto rows = skirmish::setup_row_count(map);
    // Keep the status/footer space. Only maps needing generated controls change row pitch;
    // switching back restores every authored rectangle and hides the generated idle row.
    for (const std::string column : {"IDC_PLAYER_NAME_", "IDC_PLAYER_FACTION_", "IDC_PLAYER_COLOR_", "IDC_PLAYER_TEAM_"}) {
        const auto* first = gadget_of(*staging, column + "0");
        const auto* last = gadget_of(*staging, column + std::to_string(authored_rows - 1));
        for (std::size_t slot = 0; slot < skirmish::local_setup_rows; ++slot) {
            const auto* gadget = gadget_of(*staging, column + std::to_string(slot));
            const float y = rows > authored_rows && slot < rows
                ? static_cast<float>(first->rect.y) + static_cast<float>(last->rect.y - first->rect.y)
                    * static_cast<float>(slot) / static_cast<float>(rows - 1)
                : static_cast<float>(gadget->rect.y);
            gadget->control->set_position(Vector2(gadget->control->get_position().x,
                y - static_cast<float>(staging->layout.frame.y)));
        }
    }
    const std::string team_word = lookup(model::skirmish_setup_text_key(model::SetupText::team_number));
    for (std::size_t slot = 0; slot < skirmish::local_setup_rows; ++slot) {
        team[slot]->clear();
        if (slot >= rows) {
            // The retail rows past a map's capacity stay on screen, blank and dim.
            faction[slot]->select(-1);
            colour[slot]->select(-1);
            controller[slot]->select(0);
            idle_row(slot);
            continue;
        }
        for (auto* widget : {static_cast<Control*>(faction[slot]), static_cast<Control*>(team[slot]),
            static_cast<Control*>(controller[slot]), static_cast<Control*>(colour[slot])}) widget->set_visible(true);
        for (const auto& side : map.teams)
            team[slot]->add_item(text(model::setup_team_caption(team_word, side.team)), static_cast<int>(side.team));
        const auto record = std::find_if(selection.slots.begin(), selection.slots.end(),
            [&](const auto& entry) { return entry.slot == slot + 1; });
        if (slot != 0) controller[slot]->select(record == selection.slots.end() ? controller_open : controller_index(*record));
        const auto chosen = record == selection.slots.end() ? (slot == 0 ? 0U : 1U) : record->team;
        for (int i = 0; i < team[slot]->get_item_count(); ++i)
            if (team[slot]->get_item_id(i) == static_cast<int>(chosen)) team[slot]->select(i);
        if (record != selection.slots.end()) {
            colour[slot]->select(static_cast<int>(record->colour_index.value_or(record->slot - 1)));
            for (std::size_t i = 0; i < faction_names.size(); ++i)
                if (faction_names[i] == record->faction) faction[slot]->select(static_cast<int>(i));
        } else {
            // An open row keeps its defaults until somebody takes it (WSS-14, WSS-15).
            faction[slot]->select(slot == 0 || faction_names.size() < 2 ? 0 : 1);
            colour[slot]->select(static_cast<int>(std::min(slot, palette.size() - 1)));
        }
    }
    Ref<Image> image;
    if (!map.preview_path.empty()) {
        if (auto texture = assets::load_texture(*filesystem, map.preview_path)) image = texture_image(texture.value(), failure);
    } else if (!map.embedded_preview.empty()) {
        if (auto texture = assets::load_texture(map.embedded_preview, assets::Source{})) image = texture_image(texture.value(), failure);
    }
    if (image.is_null()) {
        if (auto texture = assets::load_texture(*filesystem, "data/art/textures/generic_space.dds")) image = texture_image(texture.value(), failure);
    }
    preview->set_texture(image.is_valid() ? ImageTexture::create_from_image(image) : Ref<ImageTexture>());
    for (auto* icon : start_icons) { icon->hide(); icon->queue_free(); }
    start_icons.clear();
    if (preview->is_visible() && map.metadata.new_markers == true && map.metadata.start_positions) {
        auto decoded = assets::load_map(*filesystem, map.path);
        if (decoded && decoded.value().declared_extents) {
            const auto& extents = *decoded.value().declared_extents;
            if (extents.first > 0 && extents.second > 0) {
                // WSS-10: centred space coordinates; preview Y points down.
                const auto origin = preview->get_position();
                const auto size = preview->get_size();
                for (std::size_t i = 0; i < map.metadata.start_positions->size(); ++i) {
                    const auto& point = map.metadata.start_positions->at(i);
                    const float x = std::clamp(0.5F + point.x / extents.first, 0.0F, 1.0F);
                    const float y = std::clamp(0.5F - point.y / extents.second, 0.0F, 1.0F);
                    auto* backing = memnew(ColorRect);
                    backing->set_color(Color(0.01F, 0.01F, 0.02F, 0.85F));
                    backing->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                    // Keep numbers readable at the preview edges (project inset).
                    backing->set_position(origin + Vector2(size.x * (0.1F + x * 0.8F) - 10,
                        size.y * (0.1F + y * 0.8F) - 10));
                    backing->set_size(Vector2(20, 20));
                    root->add_child(backing);
                    auto* number = memnew(EawrUiLabel);
                    number->set_role("L_Text");
                    number->set_text(text(std::to_string(i + 1)));
                    number->set_alignment(HORIZONTAL_ALIGNMENT_CENTER);
                    number->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                    number->set_size(Vector2(20, 20));
                    backing->add_child(number);
                    start_icons.push_back(backing);
                }
            }
        }
    }
    validate();
}

void SkirmishSetupMode::State::validate() {
    if (selected < 0) { start->set_disabled(true); return; }
    const auto& map = maps[static_cast<std::size_t>(selected)];
    std::vector<skirmish::LobbySlot> occupied;
    std::vector<std::uint32_t> edited_colours;
    const String unavailable = caption(model::SetupText::unavailable);
    for (std::size_t slot = 0; slot < skirmish::setup_row_count(map); ++slot) {
        const bool active = slot == 0 || controller[slot]->get_selected() >= controller_easy;
        faction[slot]->set_disabled(!active);
        team[slot]->set_disabled(!active);
        colour[slot]->set_disabled(!active);
        if (!active) {
            // R-SETUP-01: Open rows have no faction or team caption. Occupying
            // the row again takes the ordinary opponent defaults below.
            faction[slot]->select(-1);
            team[slot]->select(-1);
            continue;
        }
        const auto previous = std::find_if(selection.slots.begin(), selection.slots.end(),
            [&](const auto& entry) { return entry.slot == slot + 1; });
        if (previous == selection.slots.end() && slot != 0 && !occupied.empty()) {
            // WSS-15: a newly occupied row starts opposite the host when valid.
            // WSS-01: the host faction's index with its low bit toggled, or the first faction when that is out of range.
            const auto host_index = static_cast<std::size_t>(std::find(faction_names.begin(), faction_names.end(),
                occupied.front().faction) - faction_names.begin());
            const std::size_t opponent = host_index < faction_names.size() ? (host_index ^ 1U) : 0U;
            faction[slot]->select(opponent < faction_names.size() ? static_cast<int>(opponent) : 0);
            for (int i = 0; i < team[slot]->get_item_count(); ++i) {
                if (team[slot]->get_item_id(i) != static_cast<int>(occupied.front().team)) { team[slot]->select(i); break; }
            }
        }
        skirmish::LobbySlot record;
        record.slot = static_cast<std::uint32_t>(slot + 1);
        record.human = slot == 0;
        // WTA-47: the AI row's level; the LAN peer's row is a human seat.
        if (slot != 0 && !(connection->get_selected() == 1 && slot == 1)) record.difficulty = controller_level(controller[slot]->get_selected());
        record.team = team[slot]->get_selected() >= 0 ? static_cast<std::uint32_t>(team[slot]->get_selected_id()) : sim::tactical::max_players;
        const int chosen_faction = faction[slot]->get_selected();
        if (chosen_faction >= 0 && static_cast<std::size_t>(chosen_faction) < faction_names.size())
            record.faction = faction_names[static_cast<std::size_t>(chosen_faction)];
        record.colour_index = static_cast<std::uint32_t>(colour[slot]->get_selected_id());
        if (previous == selection.slots.end() || previous->colour_index.value_or(previous->slot - 1) != record.colour_index)
            edited_colours.push_back(record.slot);
        if (previous == selection.slots.end() || previous->team != record.team || previous->faction != record.faction) notice.clear();
        const auto side = std::find_if(map.teams.begin(), map.teams.end(), [&](const auto& t) { return t.team == record.team; });
        for (std::size_t i = 0; i < faction_names.size(); ++i) {
            const bool allowed = side != map.teams.end()
                && std::find(side->factions.begin(), side->factions.end(), faction_names[i]) != side->factions.end();
            faction[slot]->set_item_disabled(static_cast<int>(i), !allowed);
            faction[slot]->set_item_tooltip(static_cast<int>(i), allowed ? String() : unavailable);
        }
        occupied.push_back(std::move(record));
    }
    if (occupied.size() != selection.slots.size()) notice.clear();
    selection.slots = std::move(occupied);
    for (const auto row : edited_colours) {
        const auto choice = static_cast<std::uint32_t>(colour[row - 1]->get_selected_id());
        const auto repaired = skirmish::select_setup_colour(selection, row, choice, palette.size());
        if (!repaired) { start->set_disabled(true); status->set_text(text(repaired.error().message)); return; }
    }
    for (const auto& record : selection.slots) colour[record.slot - 1]->select(static_cast<int>(*record.colour_index));
    for (std::size_t i = 0; i < start_icons.size(); ++i) {
        auto* icon = static_cast<ColorRect*>(start_icons[i]);
        icon->set_color(Color(0.01F, 0.01F, 0.02F, 0.85F));
        for (const auto& record : selection.slots) {
            if (record.team != i || *record.colour_index >= palette.size()) continue;
            const auto& rgb = palette[*record.colour_index].rgb;
            icon->set_color(Color(rgb[0] / 510.0F, rgb[1] / 510.0F, rgb[2] / 510.0F, 0.95F));
            break;
        }
    }
    const auto options_result = skirmish::setup_options(selection, maps);
    start->set_disabled(!options_result || editing.has_value());
    std::vector<model::SetupRow> rows;
    for (const auto& record : selection.slots) rows.push_back({record.team, record.faction});
    const auto blocker = model::setup_blocker_text(model::setup_blocker(rows));
    // R-SETUP-01: the three start checks show the retail messages; any other refusal keeps its own diagnostic.
    if (options_result) status->set_text(text(notice.empty() ? map.name : notice));
    else if (blocker) status->set_text(caption(*blocker));
    else status->set_text(text(options_result.error().message));
}

void SkirmishSetupMode::State::process_lan() {
    if (lan_acceptance && !gc_acceptance_campaign.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (!gc_acceptance_setup_started) gc_acceptance_setup_started = now;
        const auto fail_setup = [&](const std::string& reason) {
            Dictionary receipt;
            receipt["mode"] = "galactic"; receipt["phase"] = "lobby"; receipt["failure"] = text(reason);
            receipt["campaign"] = text(gc_acceptance_campaign);
            if (gc_acceptance_seed) receipt["seed"] = static_cast<int64_t>(*gc_acceptance_seed);
            receipt["elapsed_ms"] = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *gc_acceptance_setup_started).count());
            std::error_code error;
            std::filesystem::create_directories(gc_acceptance_out, error);
            if (!error) {
                const auto json = utf8(JSON::stringify(receipt));
                std::ofstream output(gc_acceptance_out / "report.json");
                output << json << '\n';
            }
            UtilityFunctions::printerr(text("EAWR GC acceptance setup failed: " + reason));
            host->get_tree()->quit(3);
        };
        if (!failure.empty()) { fail_setup(failure); return; }
        if (!lan) {
            connection->select(1);
            lan = make_lan_match(ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "lan-desync", lan_delay);
            lan->set_diagnostic_logger([](std::string_view line) {
                UtilityFunctions::print(String::utf8(line.data(), static_cast<int64_t>(line.size())));
            });
            lan->configure_factions(scene::faction_order(*catalog));
            lan->configure_lobby(lan_host_port ? "Acceptance host" : "Acceptance join", 0, maps,
                [](const skirmish::FixtureOptions&) {
                    return core::Result<sim::tactical::TacticalSetup>::failure(net::lobby_diagnostic(
                        net::LobbyReason::invalid_choice, "GC acceptance cannot prepare a skirmish"));
                });
            auto backend = std::make_shared<GalacticLobbyBackend>(GalacticLobbyLoader([content = content] {
                return load_galactic_lobby_content(*content->filesystem, *content->catalog, content);
            }));
            backend->acceptance_final_tick(gc_acceptance_ticks);
            lan->configure_galactic(backend);
            if (lan_host_port) {
                lan->host(*lan_host_port);
                const auto& campaigns = backend->campaigns();
                const auto selected_campaign = std::find_if(campaigns.begin(), campaigns.end(), [&](const auto& row) {
                    return gc_acceptance_campaign == "auto" || row.name == gc_acceptance_campaign;
                });
                if (selected_campaign == campaigns.end()) { failure = "GC acceptance campaign is not in the multiplayer list"; return; }
                const std::uint64_t seed = gc_acceptance_seed.value_or(net::draw_galactic_session_seed());
                UtilityFunctions::print("EAWR GC acceptance seed=", static_cast<int64_t>(seed));
                gc_acceptance_seed = seed;
                auto game = net::default_galactic_lobby_options(*selected_campaign, "Normal_Default", seed, lan_delay);
                if (!game || !lan->set_galactic_game(game.value())) { failure = "GC acceptance room could not open"; return; }
            } else lan->join(lan_join_endpoint);
        }
        lan->pump_setup({});
        const auto room = lan->lobby_state();
        const auto view = lan->view();
        const bool log = now >= gc_acceptance_next_log;
        if (log) {
            gc_acceptance_next_log = now + std::chrono::seconds(5);
            std::ostringstream state;
            state << "EAWR GC acceptance lobby role=" << (lan_host_port ? "host" : "join")
                  << " transport_phase=" << static_cast<int>(view.phase) << " connected=" << view.connected
                  << " message=" << view.message;
            if (room) {
                state << " revision=" << room->snapshot.revision << " seats=" << room->snapshot.seats.size()
                      << " room_phase=" << static_cast<int>(room->snapshot.phase) << " local_ready=" << room->local_ready
                      << " identity_pending=" << room->identity_pending << " ready_enabled=" << room->ready_enabled
                      << " start_enabled=" << room->start_enabled << " ready_reason=" << room->ready_reason
                      << " start_reason=" << room->start_reason;
                for (const auto& seat : room->snapshot.seats)
                    state << " [peer=" << seat.peer << " player=" << seat.player << " ready=" << seat.ready << ']';
                if (room->notice) state << " notice=" << room->notice->message;
            } else state << " room=none";
            UtilityFunctions::print(text(state.str()));
        }
        if (room && room->snapshot.seats.size() == 2) {
            const auto local = std::find_if(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& row) {
                return row.peer == (lan_host_port ? 0U : 1U);
            });
            if (local != room->snapshot.seats.end() && !local->ready && room->ready_enabled
                && gc_acceptance_ready_retry.attempt(room->snapshot.revision, now)) {
                const bool accepted = lan->lobby_ready(true);
                if (log) UtilityFunctions::print("EAWR GC acceptance ready request accepted=", accepted);
            }
            if (lan_host_port && room->start_enabled
                && gc_acceptance_start_retry.attempt(room->snapshot.revision, now)) lan->start({});
        }
        lan_status->set_text(text(lan->view().message));
        if (view.phase == LanPhase::ended) { fail_setup(view.message); return; }
        if (now - *gc_acceptance_setup_started >= std::chrono::seconds(300)) {
            fail_setup("Lobby did not launch within 300 seconds; see acceptance lobby diagnostics.");
        }
        return;
    }
    if (frames == 0 && (lan_host_port || !lan_join_endpoint.empty())) {
        connection->select(1);
        lan = make_lan_match(ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "lan-desync", lan_delay, lan_smoke, lan_trace);
        lan->set_diagnostic_logger([](std::string_view line) {
            UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
        });
        lan->configure_factions(scene::faction_order(*catalog));
        if (lan_host_port) lan->host(*lan_host_port); else lan->join(lan_join_endpoint);
        if (lan_smoke && lan_host_port) {
            for (std::size_t i = 0; i < maps.size(); ++i) if (maps[i].path.ends_with("_mp_space_coruscant.ted")) {
                list->select(static_cast<int>(i)); refresh_map(static_cast<int>(i)); break;
            }
            controller[2]->select(controller_normal);
            selection.slots.push_back({3, "Rebel", 0, false, {}});
            faction[2]->select(0); team[2]->select(0);
            if (lan_acceptance) {
                controller[3]->select(controller_normal);
                selection.slots.push_back({4, "Empire", 1, false, {}});
                faction[3]->select(1); team[3]->select(1);
            }
            validate();
        }
    }
    const bool enabled = connection->get_selected() == 1;
    for (Control* control : {static_cast<Control*>(lan_host), static_cast<Control*>(endpoint),
         static_cast<Control*>(lan_join), static_cast<Control*>(lan_cancel),
         static_cast<Control*>(lan_status), static_cast<Control*>(lan_addresses), static_cast<Control*>(lan_port_mapping)}) control->set_visible(enabled);
    lan_review->set_visible(enabled && lan && lan->view().read_only);
    lan_scroll->set_visible(lan_review->is_visible());
    start->set_text(caption(model::SetupText::start));
    if (!enabled) {
        if (lan) {
            lan->cancel(); lan.reset();
            controller[1]->select(controller_normal);
        }
        controller[0]->set_item_text(0, text(local_name));
        controller[1]->set_item_text(controller_easy, caption(model::SetupText::ai_easy_player));
        official->set_disabled(false); custom->set_disabled(false); advanced->set_disabled(false);
        list->set_mouse_filter(Control::MOUSE_FILTER_STOP); list->set_focus_mode(Control::FOCUS_ALL);
        for (std::size_t row = 1; row < skirmish::local_setup_rows; ++row) controller[row]->set_disabled(!row_live(row));
        return;
    }
    controller[1]->select(1);
    if (!lan) {
        lan = make_lan_match(ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "lan-desync");
        lan->set_diagnostic_logger([](std::string_view line) {
            UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
        });
        lan->configure_factions(scene::faction_order(*catalog));
    }
    if (lan_host->is_pressed()) {
        lan_host->set_pressed_no_signal(false); lan->set_port_mapping(lan_port_mapping->is_pressed()); lan->host(default_lan_port);
    }
    if (lan_join->is_pressed()) {
        lan_join->set_pressed_no_signal(false); editing.reset(); options_panel->hide();
        lan->join(utf8(endpoint->get_text()));
    }
    if (lan_cancel->is_pressed()) { lan_cancel->set_pressed_no_signal(false); lan->cancel(); }
    const auto selected_options = skirmish::setup_options(selection, maps);
    if (selected_options) lan->pump_setup(selected_options.value());
    const auto view = lan->view();
    start->set_text(caption(view.read_only ? model::SetupText::accept : model::SetupText::start));
    start->set_disabled(view.read_only ? !view.accept_enabled : (!selected_options || editing.has_value() || !view.start_enabled));
    lan_cancel->set_text(caption(model::SetupText::lan_cancel));
    lan_summary->set_text(text(view.setup_summary.empty() ? lan_message(view) : view.setup_summary));
    const std::string review_text = view.setup_summary.empty() ? lan_message(view) : view.setup_summary;
    std::size_t lines = 0, length = 0;
    for (const char character : review_text) {
        if (character == '\n') { lines += 1 + length / 100; length = 0; } else ++length;
    }
    lines += 1 + length / 100;
    lan_summary->set_custom_minimum_size(Vector2(1120, static_cast<float>(std::max<std::size_t>(14, lines)) * 24.0F));
    lan_review->set_visible(view.read_only); lan_scroll->set_visible(view.read_only);
    if (view.read_only) { root->move_child(lan_review, -1); root->move_child(lan_scroll, -1); }
    list->set_mouse_filter(view.read_only ? Control::MOUSE_FILTER_IGNORE : Control::MOUSE_FILTER_STOP);
    list->set_focus_mode(view.read_only ? Control::FOCUS_NONE : Control::FOCUS_ALL);
    official->set_disabled(view.read_only); custom->set_disabled(view.read_only); advanced->set_disabled(view.read_only);
    for (std::size_t row = 0; row < skirmish::local_setup_rows; ++row) {
        if (view.read_only) { faction[row]->set_disabled(true); team[row]->set_disabled(true); colour[row]->set_disabled(true); }
        controller[row]->set_disabled(view.read_only || row == 0 || row == 1 || !row_live(row));
    }
    // The two human seats show their players' names: the local one and the peer's (the match's own data).
    controller[0]->set_item_text(0, text(view.read_only ? view.peer_name : local_name));
    controller[1]->set_item_text(1, text(view.read_only ? local_name : view.peer_name));
    lan_status->set_text(text(lan_message(view)));
    String addresses = caption(model::SetupText::host_address);
    const auto ips = IP::get_singleton()->get_local_addresses();
    for (int64_t i = 0; i < ips.size(); ++i) {
        addresses += String(i == 0 ? " " : ", ") + ips[i] + String(":") + String::num_int64(view.port);
    }
    lan_addresses->set_text(addresses);
    if (auto offered = lan->offered_setup()) {
        // The offered lobby is presentation data; the agreed setup is revalidated by LAN-3.
        if (offered->map && *offered->map != selection.map) {
            for (std::size_t i = 0; i < maps.size(); ++i) if (maps[i].path == *offered->map) { list->select(static_cast<int>(i)); refresh_map(static_cast<int>(i)); break; }
        }
        if (offered->slots) {
            selection.slots = *offered->slots;
            selection.match = offered->match;
            for (std::size_t row = 0; row < skirmish::local_setup_rows; ++row) {
                const auto slot = std::find_if(offered->slots->begin(), offered->slots->end(),
                    [&](const auto& value) { return value.slot == row + 1; });
                controller[row]->select(row == 0 ? controller_open : slot == offered->slots->end() ? controller_open
                    : row == 1 ? 1 : controller_index(*slot));
                if (slot == offered->slots->end()) continue;
                colour[row]->select(static_cast<int>(slot->colour_index.value_or(slot->slot - 1)));
                for (std::size_t i = 0; i < faction_names.size(); ++i)
                    if (faction_names[i] == slot->faction) faction[row]->select(static_cast<int>(i));
                for (int i = 0; i < team[row]->get_item_count(); ++i)
                    if (team[row]->get_item_id(i) == static_cast<int>(slot->team)) team[row]->select(i);
            }
        }
    }
    if (lan_smoke && view.accept_enabled) lan->accept();
    if (lan_smoke && lan_host_port && frames >= 34 && view.start_enabled && selected_options)
        lan->start(selected_options.value());
    if (auto launch = lan->take_start()) pending = std::move(launch);
}

// The layout contract's report (#3167): for each named control, the rectangle its resource gives it
// (placed by the dialog layout) and the one it has on the 1280 by 720 reference screen, so a test can see a
// control that grew or moved past its authored rectangle.
void SkirmishSetupMode::State::report_layout(const BuiltDialog& dialog, const std::string_view id) const {
    const auto* gadget = gadget_of(dialog, id);
    if (gadget == nullptr || gadget->control == nullptr) return;
    const float scale = root->get_scale().x > 0.0F ? root->get_scale().x : 1.0F;
    const Vector2 origin = root->get_global_position();
    const Rect2 actual = gadget->control->get_global_rect();
    const auto numbers = [](const double a, const double b, const double c, const double d) {
        return String::num(a, 2) + String(",") + String::num(b, 2) + String(",") + String::num(c, 2) + String(",")
            + String::num(d, 2);
    };
    UtilityFunctions::print("EAWR setup layout ", text(std::string(id)),
        " authored=", numbers(gadget->rect.x, gadget->rect.y, gadget->rect.width, gadget->rect.height),
        " actual=", numbers((actual.position.x - origin.x) / scale, (actual.position.y - origin.y) / scale,
                            actual.size.x, actual.size.y),
        " visible=", gadget->control->is_visible_in_tree() ? 1 : 0);
}

void SkirmishSetupMode::State::report_layouts() const {
    for (std::size_t row = 0; row < skirmish::local_setup_rows; ++row) {
        const std::string suffix = std::to_string(row);
        for (const char* column : {"IDC_PLAYER_NAME_", "IDC_PLAYER_FACTION_", "IDC_PLAYER_COLOR_", "IDC_PLAYER_TEAM_"})
            report_layout(*staging, column + suffix);
    }
    for (const std::string_view id : {"IDC_SKIRMISH_CREDITS_SLIDER", "IDC_SKIRMISH_CREDITS_VALUE",
             "IDC_WIN_CONDITION_COMBO"}) report_layout(*options_dialog, id);
}

void SkirmishSetupMode::State::refresh_options() {
    if (!editing) return;
    heroes->set_pressed_no_signal(editing->value.allow_heroes);
    starting_units->set_pressed_no_signal(editing->value.free_starting_units);
    superweapons->set_pressed_no_signal(editing->value.allow_superweapons);
    if (win_condition != nullptr) {
        const auto index = model::setup_win_condition_index(editing->value.space_win_condition);
        win_condition->select(index ? static_cast<int>(*index) : -1);
    }
    if (credits_slider != nullptr && credits_bounds) {
        const auto shown = model::setup_credits_clamp({credits_bounds->minimum, credits_bounds->maximum},
            editing->value.credits.trunc_to_integer());
        credits_slider->set_value_no_signal(static_cast<double>(shown));
        if (credits_value != nullptr) credits_value->set_text(text(std::to_string(shown)));
    }
}

// The official and custom buttons choose the list; exactly one of them shows pressed.
void SkirmishSetupMode::State::process_map_kind() {
    if (custom->is_pressed() && !custom_maps) custom_maps = true;
    else if (official->is_pressed() && custom_maps) custom_maps = false;
    else {
        official->set_pressed_no_signal(!custom_maps);
        custom->set_pressed_no_signal(custom_maps);
        return;
    }
    official->set_pressed_no_signal(!custom_maps);
    custom->set_pressed_no_signal(custom_maps);
    filter_maps();
}

// MNU-21: Cancel leaves the staging. A shell that owns the screen routes it (route_cancel) and takes
// the request; a stand-alone screen has nowhere to go back to and quits, as closing its window does.
void SkirmishSetupMode::State::process_cancel() {
    if (!staging_cancel->is_pressed()) return;
    staging_cancel->set_pressed_no_signal(false);
    if (editing) return;
    if (cancel_routed) cancel_requested = true;
    else host->get_tree()->quit(0);
}

void SkirmishSetupMode::State::process_options() {
    if (advanced->is_pressed()) {
        advanced->set_pressed_no_signal(false);
        editing = skirmish::begin_setup_options(selection, defaults);
        refresh_options();
        root->move_child(options_panel, -1);
        options_panel->show();
    }
    // Escape cancels the copy, except the press that closes the win-condition list: that list closed
    // before this frame, so it counts as open when it was open on the frame before.
    const bool escape_down = Input::get_singleton()->is_physical_key_pressed(KEY_ESCAPE);
    const bool win_list_open = win_condition != nullptr && win_condition->get_popup()->is_visible();
    const bool escape_cancels = escape_down && !options_escape_was_down && !win_list_was_open && !win_list_open;
    options_escape_was_down = escape_down;
    win_list_was_open = win_list_open;
    if (!editing) return;
    if (heroes->is_pressed() != editing->value.allow_heroes
        || starting_units->is_pressed() != editing->value.free_starting_units
        || superweapons->is_pressed() != editing->value.allow_superweapons) editing->custom = true;
    editing->value.allow_heroes = heroes->is_pressed();
    editing->value.free_starting_units = starting_units->is_pressed();
    editing->value.allow_superweapons = superweapons->is_pressed();
    // WSS-31: a picked win condition goes into the copy and marks it custom, as a check does.
    if (win_condition != nullptr && win_condition->is_visible_in_tree()) {
        const int picked = win_condition->get_selected();
        const auto choices = model::setup_win_conditions();
        const auto current = model::setup_win_condition_index(editing->value.space_win_condition);
        if (picked >= 0 && static_cast<std::size_t>(picked) < choices.size()
            && (!current || static_cast<int>(*current) != picked)) {
            editing->value.space_win_condition = std::string(choices[static_cast<std::size_t>(picked)].name);
            editing->custom = true;
        }
    }
    if (defaults_button->is_pressed()) {
        defaults_button->set_pressed_no_signal(false);
        skirmish::reset_setup_options(*editing, defaults);
        refresh_options();
    }
    if (accept->is_pressed()) {
        accept->set_pressed_no_signal(false);
        skirmish::accept_setup_options(selection, *editing);
        editing.reset();
        options_panel->hide();
    } else if (cancel->is_pressed() || escape_cancels) {
        cancel->set_pressed_no_signal(false);
        editing.reset();
        options_panel->hide();
    }
}

// The shell's Back is the staging's Cancel (process_cancel); Escape does the same, unless it is closing
// the options panel or a drop-down list.
void SkirmishSetupMode::State::process_back(const bool was_editing) {
    const bool down = Input::get_singleton()->is_physical_key_pressed(KEY_ESCAPE);
    bool popup_open = connection->get_popup()->is_visible()
        || (win_condition != nullptr && win_condition->get_popup()->is_visible());
    for (std::size_t row = 0; row < skirmish::local_setup_rows; ++row) {
        for (auto* combo : {faction[row], team[row], controller[row], colour[row]})
            popup_open = popup_open || combo->get_popup()->is_visible();
    }
    if (cancel_routed && down && !escape_was_down && !was_editing && !editing && !popup_open) cancel_requested = true;
    escape_was_down = down;
}

SkirmishSetupMode::SkirmishSetupMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
SkirmishSetupMode::~SkirmishSetupMode() = default;
const std::string& SkirmishSetupMode::failure() const { return state_->failure; }
bool SkirmishSetupMode::ready(Node3D& host) {
    state_->host = &host;
    if (!state_->load()) { UtilityFunctions::printerr(text(state_->failure)); return false; }
    if (state_->test) host.get_viewport()->set_embedding_subwindows(true);
    if (!state_->build()) { UtilityFunctions::printerr(text(state_->failure)); return false; }
    // Probe for the GPU report: how many of the screen's own captions the installed text database lacks.
    std::int64_t missing_captions = 0;
    const auto keys = model::skirmish_setup_text_keys();
    for (const auto& entry : keys) {
        if (!state_->text_database || state_->text_database->find(entry.key) == nullptr) ++missing_captions;
    }
    UtilityFunctions::print("EAWR setup captions: ", static_cast<int64_t>(keys.size()), " keys; missing: ", missing_captions);
    UtilityFunctions::print("EAWR skirmish setup ready");
    return true;
}

void SkirmishSetupMode::process() {
    auto& state = *state_;
    if (!state.visible) return;
    if (state.online_mode) {
        state.lan = state.online_screen->lan_match();
        if (auto selected = state.online_screen->take_start()) { state.lan = state.online_screen->lan_match(); state.pending = std::move(selected); }
        else if (!state.online_screen->shown()) state.cancel_requested = true;
        return;
    }
    const auto size = state.host->get_viewport()->get_visible_rect().size;
    static_cast<void>(fit_letterbox_canvas(*state.root, size, Vector2(1280, 720)));
    state.process_map_kind();
    const auto selected = state.list->get_selected_items();
    if (!selected.is_empty() && selected[0] != state.selected) state.refresh_map(selected[0]);
    const bool was_editing = state.editing.has_value();
    state.process_options();
    state.validate();
    state.process_lan();
    state.process_cancel();
    state.process_back(was_editing);
    ++state.frames;
    if (!state.captured && state.frames >= (state.layout_test || state.nine_players ? 40U : state.two_vs_two && state.test ? 94U : state.policy_test ? 114U : state.three_players ? 98U : state.test ? 78U : 30U) && !state.options.capture_path.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(state.options.capture_path.parent_path(), directory_error);
        auto image = state.host->get_viewport()->get_texture()->get_image();
        state.captured = !directory_error && image.is_valid()
            && image->save_png(text(ViewerPath::utf8(state.options.capture_path))) == OK;
        if (!state.captured && !state.capture_error_reported) {
            state.capture_error_reported = true;
            UtilityFunctions::printerr("EAWR setup capture failed: ", text(ViewerPath::utf8(state.options.capture_path)));
        }
    }
    if (!state.lan_test.empty() && state.frames >= 34 && state.captured) {
        UtilityFunctions::print("EAWR LAN panel: ", text(state.lan_test), " | ", text(lan_message(state.lan->view())));
        state.host->get_tree()->quit(0);
        return;
    }
    // GPU contract hook: real GUI pointer/key events, then the ordinary Start handler.
    if (state.test) {
        if (state.nine_players) {
            const auto ninth = static_cast<std::size_t>(state.test_players - 1);
            if (state.frames == 4) {
                const auto map = std::find_if(state.maps.begin(), state.maps.end(), [&](const auto& candidate) {
                    return skirmish::setup_row_count(candidate) == state.test_players && candidate.unavailable.empty();
                });
                if (map == state.maps.end()) {
                    UtilityFunctions::printerr("EAWR nine-player setup: no authored nine-seat map");
                    state.host->get_tree()->quit(1);
                    return;
                }
                state.list->select(static_cast<int>(map - state.maps.begin()));
            } else if (state.frames == 10) {
                for (std::size_t row = 1; row < ninth; ++row) state.controller[row]->select(State::controller_normal);
            } else if (state.frames == 12 || state.frames == 20 || state.frames == 28) {
                click(state.controller[ninth]->get_global_rect().get_center());
            } else if ((state.frames >= 13 && state.frames <= 15) || (state.frames >= 29 && state.frames <= 31)) {
                navigate_popup(state.controller[ninth], State::controller_normal);
            } else if (state.frames == 22 || state.frames == 23) {
                navigate_popup(state.controller[ninth], State::controller_open);
            } else if (state.frames == 16 || state.frames == 24 || state.frames == 32) key(KEY_ENTER);
            else if (state.frames == 18 || state.frames == 26 || state.frames == 34) {
                UtilityFunctions::print("EAWR setup last row: live=", state.row_live(ninth) ? 1 : 0,
                    " controller=", state.controller[ninth]->get_selected(),
                    " occupied=", static_cast<int64_t>(state.selection.slots.size()),
                    " start=", state.start->is_disabled() ? 0 : 1);
                if (state.frames == 34) {
                    state.report_layouts();
                    const auto rect = state.status->get_global_rect();
                    UtilityFunctions::print("EAWR setup status rect: ", rect.position.x, ",", rect.position.y,
                        ",", rect.size.x, ",", rect.size.y);
                }
            } else if (state.frames == 36 || state.frames == 38) {
                const auto image = state.host->get_viewport()->get_texture()->get_image();
                const auto path = state.options.capture_path.parent_path()
                    / (state.frames == 36 ? "status-visible.png" : "status-hidden.png");
                if (image.is_valid()) image->save_png(text(ViewerPath::utf8(path)));
                state.status->set_visible(state.frames == 38);
            } else if (state.frames == 44) click(state.start->get_global_rect().get_center());
            return;
        } else if (state.layout_test) {
            // #3167: the staging's rectangles against the resource's; a row's drop-down list; the advanced
            // dialog with its slider and win-condition list. Each state is captured beside the staging image.
            const auto sibling = [&](const char* suffix) {
                auto base = ViewerPath::utf8(state.options.capture_path);
                if (base.ends_with(".png")) base.resize(base.size() - 4);
                return text(base + "." + suffix + ".png");
            };
            const auto save = [&](const char* suffix) {
                auto image = state.host->get_viewport()->get_texture()->get_image();
                if (image.is_valid() && !state.options.capture_path.empty()) image->save_png(sibling(suffix));
            };
            const auto report_row = [&](const std::size_t row) {
                UtilityFunctions::print("EAWR setup slot ", static_cast<int64_t>(row),
                    ": active=", row == 0 || state.controller[row]->get_selected() >= State::controller_easy ? 1 : 0,
                    " faction=", state.faction[row]->get_text(), " team=", state.team[row]->get_text());
            };
            if (state.frames == 41) {
                state.report_layouts();
                for (std::size_t row = 0; row < skirmish::setup_row_count(state.maps[static_cast<std::size_t>(state.selected)]); ++row)
                    report_row(row);
            }
            else if (state.frames == 44) click(state.controller[1]->get_global_rect().get_center());
            else if (state.frames == 52) {
                save("popup");
                const auto* popup = state.controller[1]->get_popup();
                UtilityFunctions::print("EAWR setup popup: visible=", popup->is_visible() ? 1 : 0,
                    " rect=", String::num(popup->get_position().x, 1), ",", String::num(popup->get_position().y, 1),
                    ",", String::num(popup->get_size().x, 1), ",", String::num(popup->get_size().y, 1),
                    " combo=", String::num(state.controller[1]->get_global_position().x, 1), ",",
                    String::num(state.controller[1]->get_global_position().y, 1), ",",
                    String::num(state.controller[1]->get_size().x, 1), ",",
                    String::num(state.controller[1]->get_size().y, 1));
                const Ref<StyleBoxFlat> panel = popup->get_theme_stylebox("panel");
                if (panel.is_valid()) UtilityFunctions::print("EAWR setup popup alpha: ", panel->get_bg_color().a);
                UtilityFunctions::print("EAWR setup popup scale: ", popup->get_content_scale_factor(),
                    " combo=", state.controller[1]->get_global_transform_with_canvas().get_scale().x);
            } else if (state.frames == 54 || state.frames == 55) navigate_popup(state.controller[1], 0);
            else if (state.frames == 56) key(KEY_ENTER);
            else if (state.frames == 57) report_row(1);
            else if (state.frames == 58) click(state.controller[1]->get_global_rect().get_center());
            else if (state.frames >= 59 && state.frames <= 61) {
                if (state.frames == 60) report_row(1);
                navigate_popup(state.controller[1], State::controller_normal);
            } else if (state.frames == 62) key(KEY_ENTER);
            else if (state.frames == 64) click(state.advanced->get_global_rect().get_center());
            else if (state.frames == 70) {
                report_row(1);
                save("options");
                state.report_layouts();
                if (state.credits_slider != nullptr) {
                    UtilityFunctions::print("EAWR setup credits: min=", state.credits_slider->get_min(),
                        " max=", state.credits_slider->get_max(), " step=", state.credits_slider->get_step(),
                        " value=", state.credits_slider->get_value(),
                        " caption=", state.credits_value->get_text(),
                        " enabled=", state.credits_slider->get_mouse_filter() == Control::MOUSE_FILTER_IGNORE ? 0 : 1);
                }
                if (state.win_condition != nullptr) {
                    UtilityFunctions::print("EAWR setup win condition: items=", state.win_condition->get_item_count(),
                        " selected=", state.win_condition->get_selected());
                }
            } else if (state.frames == 72 && state.win_condition != nullptr)
                click(state.win_condition->get_global_rect().get_center());
            else if (state.frames == 80) {
                save("wincondition");
                if (state.win_condition != nullptr) {
                    const auto* popup = state.win_condition->get_popup();
                    UtilityFunctions::print("EAWR setup win list: visible=", popup->is_visible() ? 1 : 0,
                        " width=", String::num(popup->get_size().x, 1),
                        " combo=", String::num(state.win_condition->get_size().x, 1));
                }
            } else if (state.frames == 84 && state.win_condition != nullptr) state.win_condition->get_popup()->hide();
            else if (state.frames == 90) state.host->get_tree()->quit(0);
            return;
        }
        if (state.two_vs_two) {
            // Exercise the same controls as a local player; no fixture injection.
            if (state.frames >= 32 && state.frames <= 48) {
                state.list->grab_focus();
                for (std::size_t i = 0; i < state.maps.size(); ++i) {
                    if (state.maps[i].path.ends_with("_mp_space_coruscant.ted") && state.selected != static_cast<int>(i))
                        key(state.selected < static_cast<int>(i) ? KEY_DOWN : KEY_UP);
                }
            } else if (state.frames == 50) click(state.controller[2]->get_global_rect().get_center());
            else if (state.frames >= 52 && state.frames <= 54) navigate_popup(state.controller[2], State::controller_normal);
            else if (state.frames == 56 || state.frames == 64 || state.frames == 72 || state.frames == 80) key(KEY_ENTER);
            else if (state.frames == 58) click(state.team[2]->get_global_rect().get_center());
            else if (state.frames >= 60 && state.frames <= 62) navigate_popup(state.team[2], 0);
            else if (state.frames == 66) click(state.faction[2]->get_global_rect().get_center());
            else if (state.frames >= 68 && state.frames <= 70) navigate_popup(state.faction[2], 0);
            else if (state.frames == 74) click(state.controller[3]->get_global_rect().get_center());
            else if (state.frames >= 76 && state.frames <= 78) navigate_popup(state.controller[3], State::controller_normal);
            else if (state.frames == 96) click(state.start->get_global_rect().get_center());
            return;
        }
        if (state.policy_test && state.frames >= 80) {
            if (state.frames == 99 && !state.options.capture_path.empty()) {
                auto image = state.host->get_viewport()->get_texture()->get_image();
                const auto destination = state.options.capture_path.parent_path()
                    / (state.options.capture_path.stem().string() + ".options.png");
                if (image.is_valid()) image->save_png(text(ViewerPath::utf8(destination)));
            }
            if (state.frames == 80 || state.frames == 92) click(state.advanced->get_global_rect().get_center());
            else if (state.frames == 82 || state.frames == 94 || state.frames == 104) click(state.heroes->get_global_rect().get_center());
            else if (state.frames == 84 || state.frames == 96 || state.frames == 106) click(state.starting_units->get_global_rect().get_center());
            else if (state.frames == 86 || state.frames == 98 || state.frames == 108) click(state.superweapons->get_global_rect().get_center());
            else if (state.frames == 88) click(state.cancel->get_global_rect().get_center());
            else if (state.frames == 100) click(state.defaults_button->get_global_rect().get_center());
            else if (state.frames == 110) click(state.accept->get_global_rect().get_center());
            else if (state.frames == 90 || state.frames == 102 || state.frames == 112) {
                const auto current = state.editing ? state.editing->value : *state.selection.match;
                UtilityFunctions::print("EAWR setup policy frame ", state.frames, ": ", current.allow_heroes,
                    ",", current.free_starting_units, ",", current.allow_superweapons, "; custom: ",
                    state.editing ? state.editing->custom : state.selection.custom_options);
            } else if (state.frames == 116) click(state.start->get_global_rect().get_center());
            return;
        }
        if (state.benchmark) {
            // A requested setup capture must finish before the bench leaves this screen.
            if (!state.bench_started && state.frames >= 60 && (state.options.capture_path.empty() || state.captured)) {
                state.bench_started = true;
                click(state.start->get_global_rect().get_center());
            }
        } else if (state.frames == (state.three_players ? 82U : 62U)) {
            click(state.colour[0]->get_global_rect().get_center());
        } else if (state.frames >= (state.three_players ? 84U : 64U)
            && state.frames <= (state.three_players ? 88U : 68U)) navigate_popup(state.colour[0], 4);
        else if (state.frames >= (state.three_players ? 92U : 72U)
            && state.frames <= (state.three_players ? 95U : 75U)) navigate_popup(state.colour[1], 3);
        else if (state.frames == (state.three_players ? 89U : 69U)
            || state.frames == (state.three_players ? 96U : 76U)) key(KEY_ENTER);
        else if (state.frames == (state.three_players ? 90U : 70U)) {
            click(state.colour[1]->get_global_rect().get_center());
        } else if (state.frames == 8) {
            click(state.custom->get_global_rect().get_center());
        } else if (state.frames == 20) {
            click(state.official->get_global_rect().get_center());
        } else if (state.frames == 16) {
            UtilityFunctions::print("EAWR setup custom map list: ", state.custom_maps,
                "; empty: ", state.maps.empty(), "; start disabled: ", state.start->is_disabled());
        } else if (state.frames == 28) {
            UtilityFunctions::print("EAWR setup official map list: ", !state.custom_maps,
                "; maps: ", static_cast<int64_t>(state.maps.size()),
                "; preview starts: ", static_cast<int64_t>(state.start_icons.size()));
        } else if (state.frames >= 32 && state.frames <= 48) {
            state.list->grab_focus();
            for (std::size_t i = 0; i < state.maps.size(); ++i) if (state.three_players
                ? state.maps[i].path == state.three_player_test_map : state.maps[i].path.ends_with("_mp_space_kessel.ted")) {
                const auto target = static_cast<int>(i);
                const auto distance = state.selected < target ? target - state.selected : state.selected - target;
                for (int step = 0; step < std::min(distance, 3); ++step)
                    key(state.selected < target ? KEY_DOWN : KEY_UP);
            }
        } else if (state.frames == 50) {
            click(state.faction[0]->get_global_rect().get_center());
        } else if (state.frames == 52 || state.frames == 53) key(KEY_DOWN);
        else if (state.frames == 54) key(KEY_ENTER);
        else if (!state.three_players && state.frames == 80) click(state.start->get_global_rect().get_center());
        else if (state.three_players) {
            // Two-player maps hide the third row; its absent popup must not
            // send navigation keys into the still-focused map list.
            if (!state.row_live(2) && state.frames != 98) return;
            if (state.frames == 62) click(state.controller[2]->get_global_rect().get_center());
            else if (state.frames == 64 || state.frames == 65) navigate_popup(state.controller[2], State::controller_normal);
            else if (state.frames == 66 || state.frames == 74 || state.frames == 80) key(KEY_ENTER);
            else if (state.frames == 68) click(state.team[2]->get_global_rect().get_center());
            else if (state.frames >= 70 && state.frames <= 72) navigate_popup(state.team[2], 2);
            else if (state.frames == 78 || state.frames == 79) navigate_popup(state.faction[2], 1);
            else if (state.frames == 76) click(state.faction[2]->get_global_rect().get_center());
            else if (state.frames == 98) {
                if (state.selected >= 0 && state.selected < static_cast<int>(state.maps.size()))
                    UtilityFunctions::print("EAWR FFA setup map: ", text(state.maps[static_cast<std::size_t>(state.selected)].path));
                UtilityFunctions::print("EAWR FFA setup third row: ", state.row_live(2));
                // The map's authored teams (a row past the map's capacity lists none).
                const auto authored_teams = state.selected >= 0 && state.selected < static_cast<int>(state.maps.size())
                    ? state.maps[static_cast<std::size_t>(state.selected)].teams.size() : std::size_t{};
                // With the third row live the count is the GUI's own (its team combo); otherwise the authored teams.
                UtilityFunctions::print("EAWR FFA setup teams: ", static_cast<int64_t>(
                    state.row_live(2) ? static_cast<std::size_t>(state.team[2]->get_item_count()) : authored_teams));
                // Stock capacity does not imply another team start. Exercise the real
                // selectors, save their roster, and finish this test without a battle.
                if (authored_teams < 3) state.host->get_tree()->quit();
            } else if (state.frames == 100) click(state.start->get_global_rect().get_center());
        }
    }
}

void SkirmishSetupMode::request_start() {
    if (state_->editing) return;
    if (state_->connection->get_selected() == 1 && state_->lan && state_->lan->view().accept_enabled) {
        state_->lan->accept(); return;
    }
    const auto pressed = StartupTrace::Clock::now();
    state_->validate();
    auto options = skirmish::setup_options(state_->selection, state_->maps);
    if (options && state_->connection->get_selected() == 1) {
        if (state_->lan && state_->lan->view().start_enabled) {
            state_->record_profile();
            state_->lan->start(options.value());
        }
        return;
    }
    if (options) {
        if (!state_->pending && (!state_->options.report_path.empty() || state_->startup_metrics))
            startup_trace.begin(true, pressed);
        state_->record_profile();
        state_->pending = std::move(options).value();
    }
}
std::optional<skirmish::FixtureOptions> SkirmishSetupMode::take_start() { return std::exchange(state_->pending, std::nullopt); }
bool SkirmishSetupMode::start_pending() const { return state_->pending.has_value(); }
std::shared_ptr<const BattleContent> SkirmishSetupMode::content() const { return state_->content; }
const skirmish::SetupMap* SkirmishSetupMode::map(const std::string& path) const {
    for (const auto& map : state_->all_maps) if (map.path == path) return &map;
    return nullptr;
}
std::shared_ptr<GodotShaderCache> SkirmishSetupMode::shaders() const { return state_->shaders; }
std::shared_ptr<LanMatch> SkirmishSetupMode::lan_match() const { return state_->lan; }
const std::filesystem::path& SkirmishSetupMode::gc_acceptance_directory() const { return state_->gc_acceptance_out; }
BaseButton* SkirmishSetupMode::start_button() const { return state_->start; }
BaseButton* SkirmishSetupMode::lan_host_button() const { return state_->lan_host; }
void SkirmishSetupMode::hide() { state_->visible = false; state_->layer->hide(); if (state_->online_mode && state_->online_screen) state_->online_screen->dismiss(); }
void SkirmishSetupMode::route_cancel() { state_->cancel_routed = true; }
bool SkirmishSetupMode::take_cancel() { return std::exchange(state_->cancel_requested, false); }
void SkirmishSetupMode::show(std::string message) {
    if (state_->online_mode && state_->online_screen) { state_->escape_was_down = true; state_->visible = true; state_->online_screen->show(*state_->host); return; }
    if (state_->lan_smoke) {
        UtilityFunctions::print("EAWR LAN smoke returned: ", text(message));
        state_->host->get_tree()->quit(message.starts_with("Battle finished") ? 0 : 2);
        return;
    }
    // A held Escape that closed the previous screen must not count as a press here.
    state_->escape_was_down = true;
    state_->visible = true;
    state_->layer->show();
    state_->notice = std::move(message);
    state_->validate();
    state_->test = state_->benchmark;
    state_->frames = 0;
    state_->bench_started = false;
    state_->capture_error_reported = false;
    if (!state_->options.capture_path.empty()) {
        state_->options.capture_path = state_->options.capture_path.parent_path()
            / (state_->options.capture_path.stem().string() + ".returned.png");
        state_->captured = false;
    }
    UtilityFunctions::print("EAWR skirmish setup returned");
}


SkirmishSetupMode::UiResources SkirmishSetupMode::ui_resources() const {
    const auto& state = *state_;
    UiResources resources;
    resources.filesystem = state.filesystem.get();
    resources.catalog = state.catalog.get();
    resources.dialogs = state.dialogs ? &*state.dialogs : nullptr;
    resources.theme_model = state.theme_model ? &*state.theme_model : nullptr;
    resources.theme = state.theme;
    resources.fonts = state.fonts.get();
    return resources;
}


void SkirmishSetupMode::enter_online(model::NetworkSettings settings) {
    auto& state = *state_;
    if (state.online_screen && UtilityFunctions::is_instance_id_valid(state.online_id)) {
        state.online_screen->dismiss(); memdelete(state.online_screen);
    }
    state.online_screen = memnew(EawrLanLobby); state.online_id = state.online_screen->get_instance_id();
    EawrLanLobby::Options options; options.content = state.content;
    options.game_root = state.options.game_root; options.mod_root = state.options.mod_root; options.profile = state.options.profile;
    options.online_settings = std::move(settings);
    options.lan_source_factory = [](std::string& failure) {
        BroadcastLobbySource::Options discovery;
        discovery.menu_chat_limits = true;
        return BroadcastLobbySource::open(std::move(discovery), failure);
    };
    options.player_name = model::effective_network_name(state.player_profile, state.platform_name);
    options.player_colour = static_cast<std::uint32_t>(std::max(0, state.player_profile.lobby_colour));
    if (!state.online_screen->build(std::move(options), &state.all_maps)) {
        state.notice = "The online lobby could not be opened."; memdelete(state.online_screen); state.online_screen = nullptr; state.online_id = 0;
        state.cancel_requested = true; return;
    }
    state.layer->hide(); state.online_mode = true; state.visible = true; state.cancel_requested = false;
    state.online_screen->show(*state.host, model::LobbySourceKind::online);
}
EawrLanLobby* SkirmishSetupMode::online_lobby() const { return state_->online_mode ? state_->online_screen : nullptr; }

void SkirmishSetupMode::enter(const bool lan) {
    auto& state = *state_;
    state.online_mode = false;
    state.visible = true;
    state.layer->show();
    state.connection->select(lan ? 1 : 0);
    state.cancel_requested = false;
    // A held Escape that closed the previous screen must not count as a press here.
    state.escape_was_down = true;
    state.validate();
}

void SkirmishSetupMode::leave() {
    auto& state = *state_;
    if (state.online_mode && state.online_screen) { state.online_screen->dismiss(); state.online_screen->lan_match()->cancel(); }
    state.online_mode = false;
    if (state.lan) { state.lan->cancel(); state.lan.reset(); }
    state.visible = false;
    state.layer->hide();
}

bool SkirmishSetupMode::visible() const { return state_->visible; }

// MAT-14: a clear backing lets the demo battle behind the staging show; the LAN connection stays opaque.
void SkirmishSetupMode::scene_behind(const bool running) {
    auto& state = *state_;
    state.scene_running = running;
    if (state.background == nullptr) return;
    state.background->set_color(scene_visible() ? Color(0.018F, 0.027F, 0.055F, 0.0F) : Color(0.018F, 0.027F, 0.055F));
}

bool SkirmishSetupMode::scene_visible() const { return state_->scene_running && !lan_selected(); }

void SkirmishSetupMode::set_content_visible(const bool visible) {
    if (state_->root != nullptr) state_->root->set_visible(visible);
}

bool SkirmishSetupMode::lan_selected() const { return state_->connection->get_selected() == 1; }

// The shell binds its Back (and Escape) to the staging's Cancel: no second button. The caption stays the
// catalogue's own.
void SkirmishSetupMode::enable_back(const std::string&) { route_cancel(); }

BaseButton* SkirmishSetupMode::back_button() const { return state_->staging_cancel; }

bool SkirmishSetupMode::take_back() { return take_cancel(); }

} // namespace eawr::presentation::godot_backend

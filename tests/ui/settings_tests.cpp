// #2927: the player settings record (docs/ui/ui-settings.md): FoC's defaults, the text form's
// round trip, tolerance of damaged files, clamping, and the table of what each retail
// control does today. The screen and the GPU case are the viewer's (test_options_screen.py).

#include "eawr/presentation/ui/settings.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/presentation/ui/selection_config.hpp"
#include "eawr/presentation/ui/text_file.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"
#include "eawr/presentation/ui/performance_deviations.hpp"
#include "ui_test_support.hpp"
#include "../../apps/viewer/src/battle_selection_settings.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

struct TestActiveSettings {
    static inline std::optional<ui::PlayerSettings> value;
    static const ui::PlayerSettings* get() { return value ? &*value : nullptr; }
    static void set(ui::PlayerSettings settings) { value = std::move(settings); }
};

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

void test_default_sfx_catalog_unchanged() {
    auto filesystem = test::ui::foc_corpus("SH-03 default SFX");
    if (!filesystem) return;
    data::LoadOptions options;
    options.parse_unregistered_xml = false;
    auto loaded = data::load_catalog(*filesystem, data::Profile::foc, options);
    expect(static_cast<bool>(loaded), "SH-03 default installed catalog loads headlessly");
    if (!loaded) return;
    const auto& catalog = loaded.value().catalog;
    expect(std::none_of(catalog.registry_roots().begin(), catalog.registry_roots().end(),
        [](const auto& root) { return root.registry_path == "data/xml/sfxeventfilesadditions.xml"; }),
        "SH-03 default game has no layer additions");
    const auto signature = [](const data::XmlNode& node) {
        std::string result;
        for (const auto& field : node.children)
            result += std::to_string(field.name.size()) + ":" + field.name
                + std::to_string(field.raw_text.size()) + ":" + field.raw_text;
        return result;
    };
    const auto name_of = [](const data::XmlNode& node) {
        for (const auto& attribute : node.attributes)
            if (core::to_lower(attribute.name) == "name") return core::to_lower(attribute.value);
        return std::string{};
    };
    // Independently reproduce main's winning-primary-registry registration order,
    // including repeats. Compare its complete event fields against the new catalog.
    std::vector<std::pair<std::string, std::string>> primary_events;
    auto registry = data::load_document(*filesystem, "data/xml/sfxeventfiles.xml");
    expect(static_cast<bool>(registry), "SH-03 winning primary registry reads");
    if (!registry) return;
    for (const auto& file : registry.value().root.children) {
        if (core::to_lower(file.name) != "file") continue;
        std::string path = file.raw_text;
        const auto first = path.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        path = path.substr(first, path.find_last_not_of(" \t\r\n") - first + 1);
        std::replace(path.begin(), path.end(), '\\', '/');
        if (!core::to_lower(path).starts_with("data/")) path = "data/xml/" + path;
        auto document = data::load_document(*filesystem, path);
        if (!document) continue;
        for (const auto& event : document.value().root.children)
            primary_events.emplace_back(name_of(event), signature(event));
    }
    std::vector<const data::Definition*> definitions;
    for (const auto& definition : catalog.definitions())
        if (definition.category == data::Category::sfx && definition.active) definitions.push_back(&definition);
    std::stable_sort(definitions.begin(), definitions.end(), [](const auto* a, const auto* b) {
        return std::pair(a->registry_order, a->definition_order) < std::pair(b->registry_order, b->definition_order);
    });
    std::vector<std::pair<std::string, std::string>> catalog_events;
    for (const auto* definition : definitions)
        catalog_events.emplace_back(core::to_lower(definition->id), signature(definition->root));
    expect(primary_events == catalog_events, "SH-03 default SFX order, repeats, names and every event field match main");
    std::map<std::string, std::string> unique_events;
    for (const auto& entry : catalog_events) unique_events.insert_or_assign(entry.first, entry.second);
    std::string bytes;
    for (const auto& [name, fields] : unique_events) bytes += name + "\n" + fields + "\n";
    const auto digest = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    std::cout << "SH-03 main-primary definitions=" << primary_events.size() << " head definitions=" << catalog_events.size()
        << " registered events=" << unique_events.size() << " fields_sha256=" << digest << "\n";
}

void test_selection_layer_config() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-selection-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    {
        std::ofstream stream(root / "XML" / "SelectionControls.xml");
        stream << R"xml(<Selection_Controls>
<Selection_Advisor_Enabled>
 Yes
</Selection_Advisor_Enabled>
<Selection_Action Name=" SELECT_OWN_ALL "><Default_Key>
 Ctrl+Y
</Default_Key>
<Include_Heroes> No </Include_Heroes>
<Faction_SFXEvent>
 ReBeL, SyntheticCue
</Faction_SFXEvent></Selection_Action>
</Selection_Controls>)xml";
    }
    const auto builtin = root / "builtin";
    std::filesystem::create_directories(builtin / "XML");
    {
        std::ofstream stream(builtin / "XML" / "SelectionControls.xml");
        stream << R"xml(<Selection_Controls><Selection_Action Name="SELECT_OWN_ALL">
<Default_Faction_SFXEvent>Empire, EmpireCue</Default_Faction_SFXEvent>
<Default_Faction_SFXEvent>Rebel, RebelCue</Default_Faction_SFXEvent>
<Advisor_SFXEvent>RHD_Choose_Reinforcements, RebelCue</Advisor_SFXEvent>
<Advisor_SFXEvent>EHD_Choose_Reinforcements, EmpireCue</Advisor_SFXEvent>
</Selection_Action></Selection_Controls>)xml";
    }
    const auto stock = root / "stock";
    std::filesystem::create_directories(stock / "XML");
    {
        std::ofstream stream(stock / "XML" / "SelectionControls.xml");
        stream << R"xml(<Selection_Controls><Selection_Action Name="SELECT_OWN_ALL">
<Faction_SFXEvent>Empire, OldEmpireCue</Faction_SFXEvent>
</Selection_Action></Selection_Controls>)xml";
    }
    {
        const std::array layers{vfs::MountSpec{"mod", root, "data", {}},
            vfs::MountSpec{"builtin:revised", builtin, "data", {}},
            vfs::MountSpec{"base", stock, "data", {}}};
        auto layered = vfs::Vfs::mount(layers);
        expect(static_cast<bool>(layered), "selection layers mount");
        if (layered) {
            const auto config = ui::load_selection_config(layered.value());
            const auto& action = config.actions[0];
            expect(!action.include_heroes && config.actions[1].include_heroes,
                "Include_Heroes is read per action and defaults to Yes");
            data::EffectiveObject faction;
            faction.object_id = "Empire";
            data::EffectiveValue advisor;
            advisor.value.name = "Reinforcements_Selection_SFXEvent";
            advisor.value.raw_text = "RHD_Choose_Reinforcements";
            faction.values.push_back(std::move(advisor));
            const auto rebel_advisor = ui::selection_faction_advisors(faction);
            const std::vector<std::string> empire_advisor{"EHD_Choose_Reinforcements"};
            const std::vector<std::string> none;
            expect(ui::selection_advisor_event(action, "Empire", rebel_advisor) == "RebelCue",
                "Empire record with Rebel advisor selects Rebel cue");
            expect(ui::selection_advisor_event(action, "Empire", empire_advisor) == "EmpireCue",
                "FoC advisor mapping preserves Empire cue");
            expect(ui::selection_advisor_event(action, "Rebel", rebel_advisor) == "SyntheticCue",
                "upper faction override wins over advisor mapping");
            expect(ui::selection_advisor_event(action, "Empire", none) == "EmpireCue",
                "unmapped advisor falls back to faction default");
            const std::vector<std::string> custom_advisor{"custom_voice", "RHD_Choose_Reinforcements"};
            expect(ui::selection_advisor_event(action, "Empire", custom_advisor) == "EmpireCue",
                "an unmapped primary advisor does not borrow a later unrelated voice");
            expect(ui::selection_advisor_event(action, "Unknown", none).empty(), "unknown faction stays silent");
        }
    }
    {
        const auto empty_revised = root / "empty-revised";
        const auto advisor_mod = root / "advisor-mod";
        std::filesystem::create_directories(empty_revised / "XML");
        std::filesystem::create_directories(advisor_mod / "XML");
        {
            std::ofstream stream(advisor_mod / "XML" / "SelectionControls.xml");
            stream << R"xml(<Selection_Controls><Selection_Action Name="SELECT_OWN_ALL">
<Advisor_SFXEvent>RHD_Choose_Reinforcements, RebelCue</Advisor_SFXEvent>
</Selection_Action></Selection_Controls>)xml";
        }
        const std::array layers{vfs::MountSpec{"mod", advisor_mod, "data", {}},
            vfs::MountSpec{"builtin:revised", empty_revised, "data", {}},
            vfs::MountSpec{"base", stock, "data", {}}};
        auto layered = vfs::Vfs::mount(layers);
        expect(static_cast<bool>(layered), "selection layers mount with incomplete Revised");
        if (layered) {
            const std::vector<std::string> advisors{"RHD_Choose_Reinforcements"};
            expect(ui::selection_advisor_event(ui::load_selection_config(layered.value()).actions[0], "Empire", advisors) == "RebelCue",
                "missing Revised config still blocks lower faction override");
        }
    }
    {
        const std::array stock_only{vfs::MountSpec{"base", stock, "data", {}}};
        auto stock_files = vfs::Vfs::mount(stock_only);
        expect(static_cast<bool>(stock_files), "stock selection config mounts without Revised");
        if (stock_files) {
            const std::vector<std::string> advisors{"RHD_Choose_Reinforcements"};
            expect(ui::selection_advisor_event(ui::load_selection_config(stock_files.value()).actions[0], "Empire", advisors) == "OldEmpireCue",
                "faction override remains effective without Revised");
        }
    }
    const std::array mounts{vfs::MountSpec{"mod", root, "data", {}}};
    auto filesystem = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "SH-03 config fixture mounts");
    if (filesystem) {
        ui::PlayerSettings settings;
        ui::PlayerSettings fallback;
        TestActiveSettings::value.reset();
        presentation::godot_backend::prepare_battle_selection_settings<TestActiveSettings>(fallback, filesystem.value());
        expect(!TestActiveSettings::get(), "evidence preparation leaves active settings absent");
        expect(ui::selection_action(fallback, 'Y', false, true, false) == 0,
            "evidence preparation resolves configured selection hotkey");
        const auto settings_file = root / "player-settings.cfg";
        for (const int cap : {0, 144}) {
            {
                std::ofstream stream(settings_file);
                stream << "eawr-settings 1\nvideo.vsync=0\nvideo.frame_rate_limit=" << cap << "\n";
            }
            const auto text = ui::read_text_file(settings_file);
            expect(text.has_value(), "player settings fixture is readable");
            if (!text) continue;
            auto player = ui::parse_settings(*text).settings;
            expect(!player.video.vsync && player.video.frame_rate_limit == cap, "saved pacing fixture loads");
            TestActiveSettings::set(player);
            presentation::godot_backend::prepare_battle_selection_settings<TestActiveSettings>(fallback, filesystem.value());
            expect(TestActiveSettings::get()->video == player.video, "player video settings survive preparation");
        }
        {
            std::ofstream stream(settings_file);
            stream << "damaged\n";
        }
        const auto damaged_text = ui::read_text_file(settings_file);
        expect(damaged_text.has_value() && ui::parse_settings(*damaged_text).defaulted, "damaged file defaults");
        TestActiveSettings::set(damaged_text ? ui::parse_settings(*damaged_text).settings : ui::PlayerSettings{});
        const auto defaults_video = TestActiveSettings::get()->video;
        presentation::godot_backend::prepare_battle_selection_settings<TestActiveSettings>(fallback, filesystem.value());
        expect(TestActiveSettings::get()->video == defaults_video, "damaged settings defaults survive preparation");
        std::filesystem::remove(settings_file);
        expect(!ui::read_text_file(settings_file), "missing file uses defaults");
        TestActiveSettings::set(ui::PlayerSettings{});
        presentation::godot_backend::prepare_battle_selection_settings<TestActiveSettings>(fallback, filesystem.value());
        expect(TestActiveSettings::get()->video == defaults_video, "missing settings defaults survive preparation");
        TestActiveSettings::value.reset();
        ui::apply_selection_config(settings, filesystem.value());
        expect(settings.selection_hotkeys && ui::selection_key(settings, 0) == "Ctrl+Y"
            && ui::selection_key(settings, 1) == "Q", "SH-03 partial action XML keeps optional defaults");
        expect(settings.selection_config.actions[0].faction_events.at("rebel") == "SyntheticCue"
            && settings.selection_config.actions[1].faction_events.empty(), "SH-03 faction mapping is optional");
        const auto untouched = ui::serialize_settings(settings);
        expect(untouched.find("keyboard.selection_hotkeys=") == std::string::npos,
            "SH-03 an unrelated Options save must not freeze an XML default");
        auto reloaded = ui::parse_settings(untouched).settings;
        ui::apply_selection_config(reloaded, filesystem.value());
        expect(reloaded.selection_hotkeys && !reloaded.selection_hotkeys_authored,
            "SH-03 a new layer enable default survives an untouched settings save");
        vfs::Vfs no_layer;
        auto removed_layer = ui::parse_settings(ui::serialize_settings(reloaded)).settings;
        ui::apply_selection_config(removed_layer, no_layer);
        expect(!removed_layer.selection_hotkeys && !removed_layer.selection_hotkeys_authored,
            "SH-03 removing the layer restores the project default after an unrelated save");
        auto saved = ui::parse_settings("eawr-settings 1\nkeyboard.selection_hotkeys=0\nkey.SELECT_OWN_ALL=Ctrl+U\n").settings;
        ui::apply_selection_config(saved, filesystem.value());
        expect(!saved.selection_hotkeys && ui::selection_key(saved, 0) == "Ctrl+U", "SH-03 player overrides XML enable and key");
        saved.keys.clear();
        expect(ui::selection_key(saved, 0) == "Ctrl+Y", "SH-03 reset restores XML default");
    }
    testing::remove_temp_directory(root);
    vfs::Vfs absent;
    const auto defaults = ui::load_selection_config(absent);
    expect(!defaults.enabled && defaults.actions[0].default_key == "Ctrl+Q"
        && defaults.actions[1].default_key == "Q", "SH-03 absent config preserves merged defaults");
}

void test_revised_settings() {
    const auto old = ui::parse_settings("eawr-settings 1\n").settings;
    expect(old.revised_layer_enabled && !old.revised_layer_path, "Revised defaults on without an override");
    auto edited = old;
    edited.revised_layer_enabled = false;
    edited.revised_layer_path = "C:/example/Revised";
    const auto restored = ui::parse_settings(ui::serialize_settings(edited)).settings;
    expect(!restored.revised_layer_enabled && restored.revised_layer_path == edited.revised_layer_path, "Revised settings round trip");
}

void test_selection_hotkeys() {
    ui::PlayerSettings settings;
    expect(!settings.selection_hotkeys && !ui::selection_action(settings, 'Q', false, false, false), "SH-01 defaults off");
    settings.selection_hotkeys = true;
    settings.selection_hotkeys_authored = true;
    expect(!ui::selection_action(settings, 'Q', false, false, false, false, true)
        && !ui::selection_action(settings, 'Q', false, true, false, false, true), "SH-01 free-camera chords remain camera-owned");
    expect(ui::selection_action(settings, 'Q', false, false, false) == 1, "Q selects screen");
    expect(ui::selection_action(settings, 'Q', false, true, false) == 0, "Ctrl+Q selects all");
    expect(!ui::selection_action(settings, 'Q', true, false, false)
        && !ui::selection_action(settings, 'Q', false, false, true)
        && !ui::selection_action(settings, 'Q', false, false, false, true), "exact modifiers protect existing keys");
    for (const auto chord : ui::selection_key_choices) {
        const bool ctrl = chord.starts_with("Ctrl+");
        expect(!ui::ability_hotkey(chord.back(), {false, ctrl, false}), "offered chords do not shadow abilities");
    }
    expect(!ui::set_selection_key(settings, 0, "E") && !ui::set_selection_key(settings, 0, "Ctrl+E")
        && !ui::set_selection_key(settings, 0, "Shift+Q"), "reserved chords rejected");
    expect(!ui::set_selection_key(settings, 0, "Q"), "duplicate binding rejected");
    expect(ui::selection_key(settings, 0) == "Ctrl+Q", "failed edit preserves binding");
    expect(ui::set_selection_key(settings, 0, "Ctrl+Y"), "all binding editable");
    expect(ui::set_selection_key(settings, 1, "Ctrl+U"), "screen binding editable");
    settings = ui::parse_settings(ui::serialize_settings(settings)).settings;
    expect(settings.selection_hotkeys && ui::selection_action(settings, 'Y', false, true, false) == 0
        && ui::selection_action(settings, 'U', false, true, false) == 1
        && !ui::selection_action(settings, 'Q', false, false, false), "bindings persist and old keys stop firing");
    settings.keys["SELECT_OWN_ALL"] = "Ctrl+U";
    expect(!ui::selection_action(settings, 'U', false, true, false), "hand-edited duplicates fail closed");
}

void test_performance_deviations() {
    std::set<std::string_view> ids, keys;
    std::ostringstream table;
    table << "\n| Rule | Setting key | Name | Scope | Default | Retail behaviour | Our behaviour |\n"
             "|---|---|---|---|---|---|---|\n";
    for (const ui::PerformanceDeviation& row : ui::performance_deviations) {
        expect(!row.rule_id.empty() && ids.insert(row.rule_id).second, "performance rules have unique nonempty IDs");
        expect(row.setting_key.empty() || keys.insert(row.setting_key).second, "performance setting keys are unique");
        expect(!row.default_value.empty(), "performance defaults are explicit");
        expect(row.scope == ui::PerformanceDeviationScope::presentation || row.scope == ui::PerformanceDeviationScope::simulation,
               "performance scope is explicit");
        for (const auto cell : {row.rule_id, row.setting_key, row.name, row.default_value, row.retail_behaviour, row.our_behaviour}) {
            expect(cell.find_first_of("|\r\n") == std::string_view::npos, "registry cells stay single-line table values");
        }
        table << "| " << row.rule_id << " | " << row.setting_key << " | " << row.name << " | "
              << (row.scope == ui::PerformanceDeviationScope::presentation ? "presentation" : "simulation") << " | "
              << row.default_value << " | " << row.retail_behaviour << " | " << row.our_behaviour << " |\n";
    }
    // CTest's build directory is inside its source tree. Avoid cached __FILE__ paths from another build slot.
    std::filesystem::path root = std::filesystem::current_path();
    const auto relative = std::filesystem::path("docs") / "behaviour" / "performance-deviations.md";
    while (!std::filesystem::is_regular_file(root / relative) && root != root.parent_path()) root = root.parent_path();
    std::ifstream stream(root / relative);
    expect(stream.is_open(), "performance registry documentation is present in the test source tree");
    std::ostringstream contents; contents << stream.rdbuf();
    const std::string document = contents.str();
    constexpr std::string_view begin = "<!-- performance-deviations:begin -->", end = "<!-- performance-deviations:end -->";
    const auto first = document.find(begin), last = document.find(end);
    expect(first != std::string::npos && last != std::string::npos && last > first,
           "performance documentation has a checked registry table");
    if (first != std::string::npos && last != std::string::npos && last > first) {
        expect(document.substr(first + begin.size(), last - first - begin.size()) == table.str(),
               "performance docs exactly match registry IDs, setting keys, defaults and descriptions");
    }
}

bool has_note(const ui::SettingsLoad& load, const std::string& part) {
    return std::any_of(load.notes.begin(), load.notes.end(),
                       [&](const std::string& note) { return note.find(part) != std::string::npos; });
}


void test_online_settings() {
    ui::NetworkSettings network;
    expect(!ui::validate_online_settings(network), "empty project service can be saved");
    expect(ui::validate_online_settings(network, true)->code == "EAWR-NET-1745", "Online requires a configured service");
    network.online_host = "service.example"; network.online_pin = std::string(64, 'a');
    expect(!ui::validate_online_settings(network, true), "explicit service pin validates");
    ui::PlayerSettings stored; stored.network = network; stored.network.online_relay_only = true;
    expect(ui::parse_settings(ui::serialize_settings(stored)).settings == stored, "service and relay-only choice round trip");
    network.online_host = "https://secret.example:123";
    auto error = ui::validate_online_settings(network, true);
    expect(error && error->code == "EAWR-NET-1746" && error->message.find("secret") == std::string::npos, "host validation is curated and private");
    for (const char* invalid : {"service:27020", ":::1", "-bad.example", "bad..example"}) {
        network = stored.network; network.online_host = invalid;
        expect(ui::validate_online_settings(network)->code == "EAWR-NET-1746", "malformed host syntax is refused before network I/O");
    }
    network = stored.network; network.online_host = "::1";
    expect(!ui::validate_online_settings(network, true), "raw IPv6 service address validates");
    network = stored.network; network.online_port = 0;
    expect(ui::validate_online_settings(network)->code == "EAWR-NET-1747", "zero service port is refused, not clamped");
    network = stored.network; network.online_pin = "secret-pin";
    expect(ui::validate_online_settings(network)->code == "EAWR-NET-1749", "invalid pin cannot silently select another trust mode");
    network = stored.network; network.online_trust = "ca-file"; network.online_ca_file = "certificates/community.pem";
    expect(!ui::validate_online_settings(network, true), "explicit CA file is accepted");
    network.online_ca_file.clear();
    expect(ui::validate_online_settings(network)->code == "EAWR-NET-1750", "CA mode needs its trust file");
    network = stored.network; network.online_trust = "system";
    expect(!ui::validate_online_settings(network, true), "system trust accepts a DNS host");
    for (const char* literal : {"127.0.0.1", "::1"}) {
        network.online_host = literal;
        expect(ui::validate_online_settings(network, true)->code == "EAWR-NET-1754", "system trust refuses IP literals with a curated setting message");
    }
    network.online_host = "service.example"; stored.network = network;
    expect(ui::parse_settings(ui::serialize_settings(stored)).settings == stored, "system trust choice round trips");
    const auto malformed = ui::parse_settings("eawr-settings 1\nnetwork.online_host=secret.example\nnetwork.online_port=bad\nnetwork.online_pin=private\n");
    expect(has_note(malformed, "EAWR-NET-1747") && !has_note(malformed, "secret.example"), "damaged service remains invalid with a private typed note");
    stored.network.online_host = "service.example\nnetwork.allow_upnp=0";
    expect(ui::parse_settings(ui::serialize_settings(stored)).settings.network.allow_upnp, "service text cannot inject settings lines");
}

void test_animated_loading_background_settings() {
    expect(!ui::PlayerSettings{}.gameplay.animated_loading_background, "loading animation defaults off");
    expect(!ui::parse_settings("eawr-settings 1\n").settings.gameplay.animated_loading_background,
           "older settings default loading animation off");
    for (const bool enabled : {false, true}) {
        ui::PlayerSettings value;
        value.gameplay.animated_loading_background = enabled;
        expect(ui::parse_settings(ui::serialize_settings(value)).settings == value, "loading animation preference round trips");
    }
}

void test_reinforcement_area_settings() {
    const ui::GameplaySettings defaults;
    expect(defaults.reinforcement_areas_only_while_placing, "placement-only is the fresh default");
    for (const bool only_placing : {false, true}) {
        ui::PlayerSettings value;
        value.gameplay.reinforcement_areas_only_while_placing = only_placing;
        expect(ui::parse_settings(ui::serialize_settings(value)).settings == value, "reinforcement preference round trips");
        for (const bool data_drag : {false, true}) {
            for (const bool pane : {false, true}) {
                for (const bool placing : {false, true}) {
                    const bool expected = placing || (pane && !only_placing && !data_drag);
                    expect(ui::reinforcement_areas_visible(value.gameplay, data_drag, pane, placing) == expected,
                           "placement preference preserves data rule and all panel/placement transitions");
                }
            }
        }
    }
    expect(ui::parse_settings("eawr-settings 1\n").settings.gameplay.reinforcement_areas_only_while_placing,
           "old settings files get the new default");
    expect(ui::parse_settings("eawr-settings 1\ngameplay.reinforcement_areas_only_while_placing=0\n")
               .settings.gameplay.reinforcement_areas_only_while_placing == false, "retail preference loads");
}

void test_performance_thread_settings() {
    const ui::PlayerSettings defaults;
    expect(defaults.video.particle_threads == 0 && defaults.video.simulation_threads == 0,
           "thread settings default to automatic");
    for (const int threads : {0, 1, 2, 4, 6, 8}) {
        ui::PlayerSettings value; value.video.particle_threads = threads;
        expect(ui::parse_settings(ui::serialize_settings(value)).settings.video.particle_threads == threads,
               "particle thread settings round trip");
    }
    for (const int threads : {0, 2, 4, 6, 8, 12, 16}) {
        ui::PlayerSettings value; value.video.simulation_threads = threads;
        expect(ui::parse_settings(ui::serialize_settings(value)).settings.video.simulation_threads == threads,
               "simulation thread settings round trip");
    }
    ui::PlayerSettings invalid; invalid.video.particle_threads = 64; invalid.video.simulation_threads = 7;
    const auto repaired = ui::parse_settings(ui::serialize_settings(invalid)).settings;
    expect(repaired.video.particle_threads == 0 && repaired.video.simulation_threads == 0,
           "unsupported choices fall back to automatic");
    expect(!ui::resolve_thread_override(std::nullopt, 0), "automatic preserves existing rule");
    expect(ui::resolve_thread_override(std::nullopt, 4) == 4, "saved count overrides automatic");
    expect(ui::resolve_thread_override(std::size_t{8}, 4) == 8, "launch or profile overrides saved count");

    expect(defaults.video.depth_prepass, "depth pre-pass defaults on");
    ui::PlayerSettings depth; depth.video.depth_prepass = false;
    expect(!ui::parse_settings(ui::serialize_settings(depth)).settings.video.depth_prepass, "depth pre-pass round trips");

    const auto extremes = ui::parse_settings("eawr-settings 1\nvideo.particle_threads=65\nvideo.simulation_threads=-1\n").settings;
    expect(extremes.video.particle_threads == 0 && extremes.video.simulation_threads == 0,
           "out-of-range settings clamp and resolve to offered values");

    const auto old = ui::parse_settings("eawr-settings 1\n").settings;
    expect(old.video.depth_prepass, "old settings preserve the enabled depth pre-pass default");
    expect(old.video.particle_threads == 0 && old.video.simulation_threads == 0,
           "old settings retain automatic thread rules");
}

void test_render_thread_settings() {
    expect(ui::PlayerSettings{}.video.render_thread == ui::RenderThreadSetting::separate,
           "Separate is the fresh default");
    for (const auto mode : {ui::RenderThreadSetting::separate, ui::RenderThreadSetting::safe}) {
        ui::PlayerSettings value; value.video.render_thread = mode;
        const auto text = ui::serialize_settings(value);
        expect(ui::parse_settings(text).settings.video.render_thread == mode, "render thread round trips");
        expect(ui::stored_render_thread(text) == mode, "launcher reader agrees with serialized UI setting");
    }
    for (const std::string value : {"-1", "0", "1", "2", "2147483647", "2147483648", "+1", "bad", "",
                                   "1\nvideo.render_thread=bad", "0\nvideo.render_thread=2",
                                   "1\nvideo.render_thread=-2", "0\nvideo.render_thread=2147483648",
                                   "1\nvideo.render_thread=+1"}) {
        const auto text = "# comment\n eawr-settings 1 \r\n video.render_thread = " + value + "\n";
        const auto full = ui::parse_settings(text).settings.video.render_thread;
        expect(ui::stored_render_thread(text).value_or(ui::RenderThreadSetting::separate) == full,
               "launcher and UI use identical whitespace, invalid-value and clamping rules");
    }
    expect(!ui::stored_render_thread("video.render_thread=1"), "no header means project default");
    expect(!ui::stored_render_thread("eawr-settings 999\nvideo.render_thread=1"), "unsupported version means project default");
    expect(!ui::stored_render_thread("eawr-settings 1\n"), "absent setting leaves project default alone");
    expect(!ui::stored_render_thread(""), "missing file leaves project default alone");
    const std::string bom = "\xEF\xBB\xBF" "eawr-settings 1\nvideo.render_thread=1\n";
    expect(ui::parse_settings(bom).defaulted && !ui::stored_render_thread(bom),
           "launcher and UI consistently reject a BOM rather than reading different preferences");
}

void test_defaults() {
    const ui::PlayerSettings defaults;
    // ST-A1: the owner-requested quieter master keeps the retail channel balance.
    expect(defaults.audio.master_percent == 37 && defaults.audio.sfx_percent == 75
               && defaults.audio.speech_percent == 75 && defaults.audio.music_percent == 75
               && defaults.audio.subtitles,
           "ST-A1: master defaults to 37 percent, channels 75 percent, subtitles on");
    // ST-V1: retail video defaults, with the owner-requested ST-V5 vsync-on default.
    expect(defaults.video.fullscreen_width == 800 && defaults.video.fullscreen_height == 600
               && defaults.video.window_width == 800 && defaults.video.window_height == 600
               && !defaults.video.windowed && defaults.video.vsync && defaults.video.hardware_cursor
               && defaults.video.gamma_permille == 1000 && defaults.video.detail_level == 2,
           "ST-V1: the video defaults");
    expect(std::all_of(defaults.video.detail.begin(), defaults.video.detail.end(),
                       [](const int value) { return value == ui::detail_automatic; }),
           "ST-V4: every advanced detail value follows the basic level");
    expect(defaults.video.render_profile == ui::RenderProfileSetting::automatic,
           "ST-P1: the render profile is the run's own choice");
    // ST-G1: both game speeds at the middle step, scroll speed 1.0 at stop 3, absolute scrolling on,
    // ability autofire on, everything else off.
    const ui::GameplaySettings& play = defaults.gameplay;
    expect(play.tactical_speed == 2 && play.strategic_speed == 2 && play.scroll_stop == 3
               && play.scroll_speed() == 1.0 && play.absolute_scrolling && play.abilities_autofire
               && !play.delay_tooltips && !play.right_button_scroll && !play.both_buttons_rotate
               && !play.alternate_mouse && !play.simplified_faction_colours && !play.ask_auto_resolve,
           "ST-G1: the gameplay defaults");
    // ST-N1: the Defaults button's values.
    const ui::NetworkSettings& net = defaults.network;
    expect(net.port == 1234 && net.uses_default_port() && net.allow_nat && net.allow_firewall && net.allow_upnp
               && net.quickmatch_default == 1 && net.connection_speed == 0,
           "ST-N1: the network defaults");
    expect(defaults.keys.empty(), "ST-K1: no key overrides");
    expect(ui::volume_gain(75) == 0.75 && ui::volume_gain(0) == 0.0 && ui::volume_gain(100) == 1.0
               && ui::volume_gain(-5) == 0.0 && ui::volume_gain(500) == 1.0,
           "a percent slider is a 0..1 gain, bounded");
    expect(ui::GameplaySettings{.scroll_stop = 0}.scroll_speed() == 0.0
               && ui::GameplaySettings{.scroll_stop = 9}.scroll_speed() == 3.0,
           "ST-G2: scroll speed spans 0..3 over ten stops");
}

void test_camera_keys() {
    ui::PlayerSettings settings;
    expect(!settings.camera_wasd, "ST-K3: retail arrows are the fresh default");
    for (const bool wasd : {false, true}) {
        settings.camera_wasd = wasd;
        settings.video.vsync = false;
        settings.audio.master_percent = 81;
        settings.keys["GUARD"] = "Ctrl+G";
        const auto loaded = ui::parse_settings(ui::serialize_settings(settings));
        expect(loaded.notes.empty() && loaded.settings == settings,
               "ST-K3: camera preset round trips with all stored profile choices");
    }
    const auto old = ui::parse_settings("eawr-settings 1\nvideo.vsync=0\naudio.master=81\n");
    expect(!old.settings.camera_wasd && !old.settings.video.vsync && old.settings.audio.master_percent == 81,
           "ST-K3: missing preset adds only its default");
    expect(ui::project_key_for("IDC_EDIT_ATTACK", true) == "X"
        && ui::project_key_for("IDC_EDIT_STOP", true) == "V"
        && ui::project_key_for("IDC_EDIT_WAYPOINT", true) == "U", "ST-K3: keyboard help uses moved keys");
    for (const auto& move : ui::camera_key_moves) {
        expect(ui::camera_command_key(move.retail, false) == move.retail
            && ui::camera_command_key(move.retail, true) == '\0'
            && ui::camera_command_key(move.wasd, true) == move.retail,
            "ST-K3: a bare physical chord belongs to exactly one camera or command action");
    }
    expect(ui::camera_command_key('Q', true) == 'Q' && ui::camera_command_key('Z', true) == 'Z',
           "ST-K3: layout letters are not additional physical pan positions");
}

void test_frame_pacing() {
    ui::VideoSettings video;
    video.vsync = false;
    expect(video.frame_rate_limit == 120, "ST-P2: fresh render cap is 120 FPS");
    expect(ui::frame_rate_cap(video, 144, false, true) == 120, "battle uses selected cap");
    expect(ui::frame_rate_cap(video, 144, true, true) == 60, "demo and menu use 60 FPS");
    expect(ui::frame_rate_cap(video, 144, false, false) == 30, "unfocused battle uses 30 FPS");
    video.frame_rate_limit = 30;
    expect(ui::frame_rate_cap(video, 144, true, true) == 30, "lower selected cap wins in menus");
    video.frame_rate_limit = 0;
    expect(ui::frame_rate_cap(video, 144, false, true) == 0, "Off allows uncapped focused battle");
    expect(ui::frame_rate_cap(video, 144, true, false) == 30, "Off still limits background menus");
    video.vsync = true;
    expect(ui::frame_rate_cap(video, 144, false, true) == 144, "vsync limits an otherwise uncapped battle");
    video.frame_rate_limit = 120;
    expect(ui::frame_rate_cap(video, 60, false, true) == 60, "vsync and explicit limit combine lower wins");
    video.vsync = false;
    video.frame_rate_limit = -1;
    expect(ui::frame_rate_cap(video, 165, false, true) == 165, "refresh selection follows the current display");
    expect(ui::frame_rate_cap(video, -1, false, true) == 60, "unknown refresh safely uses 60 FPS");
    video.frame_rate_limit = 1;
    expect(ui::frame_rate_cap(video, 144, false, true) == 30, "render floor allows the 30 Hz tick clock");
    for (int fps : {0, -1, 30, 60, 90, 120, 144, 165, 240, 360}) {
        ui::PlayerSettings stored;
        stored.video.frame_rate_limit = fps;
        const auto loaded = ui::parse_settings(ui::serialize_settings(stored));
        expect(loaded.notes.empty() && loaded.settings.video.frame_rate_limit == fps,
               "stored FPS choices survive loading without default replacement");
    }
    const auto old = ui::parse_settings("eawr-settings 1\nvideo.vsync=0\naudio.master=81\n");
    expect(!old.settings.video.vsync && old.settings.audio.master_percent == 81
           && old.settings.video.frame_rate_limit == 120, "missing FPS field adds the default and keeps stored settings");
}

void test_round_trip() {
    ui::PlayerSettings changed;
    changed.audio = {10, 20, 30, 40, false};
    changed.video.fullscreen_width = 1920;
    changed.video.fullscreen_height = 1080;
    changed.video.window_width = 1280;
    changed.video.window_height = 720;
    changed.video.windowed = true;
    changed.video.vsync = true;
    changed.video.detail_level = 3;
    changed.video.texture_mip_drop = 2;
    changed.video.gamma_permille = 1250;
    changed.video.widescreen = true;
    changed.video.hardware_cursor = false;
    changed.video.detail[4] = 2;
    changed.video.render_profile = ui::RenderProfileSetting::retail;
    changed.gameplay.tactical_speed = 4;
    changed.gameplay.strategic_speed = 0;
    changed.gameplay.scroll_stop = 9;
    changed.gameplay.absolute_scrolling = false;
    changed.gameplay.abilities_autofire = false;
    changed.gameplay.delay_tooltips = true;
    changed.network.port = 4000;
    changed.network.allow_upnp = false;
    changed.network.quickmatch_default = 2;
    changed.network.lobby_locale = 3;
    changed.keys["STOP"] = "X";

    const ui::SettingsLoad loaded = ui::parse_settings(ui::serialize_settings(changed));
    expect(!loaded.defaulted && loaded.notes.empty(), "a written file loads clean");
    expect(loaded.settings == changed, "the record round-trips exactly");
    expect(ui::serialize_settings(loaded.settings) == ui::serialize_settings(changed),
           "the text form is stable across a load");
    expect(ui::parse_settings(ui::serialize_settings(ui::PlayerSettings{})).settings == ui::PlayerSettings{},
           "the defaults round-trip");
}

void test_garbage() {
    // ST-S1: nothing a file holds can fail the load; a bad file is the defaults.
    for (const std::string_view text : {std::string_view{}, std::string_view{"\n\n"}, std::string_view{"hello"},
                                        std::string_view{"eawr-settings"}, std::string_view{"eawr-settings x"},
                                        std::string_view{"\xFF\xFE\x01 junk"}}) {
        const ui::SettingsLoad load = ui::parse_settings(text);
        expect(load.defaulted && load.settings == ui::PlayerSettings{} && !load.notes.empty(),
               "ST-S1: a file that is not a settings file loads as the defaults");
    }
    const ui::SettingsLoad other = ui::parse_settings("eawr-settings 99\naudio.master=5\n");
    expect(other.defaulted && other.settings.audio.master_percent == 37 && has_note(other, "version 99"),
           "ST-S2: another version loads as the defaults, with a note");

    const ui::SettingsLoad mixed = ui::parse_settings(
        "eawr-settings 1\r\n"
        "# a comment\r\n"
        "audio.master=abc\r\n"          // not a number: ignored, default kept
        "audio.sfx=12x\r\n"             // trailing text: ignored
        "audio.music=250\r\n"           // out of range: clamped
        "video.windowed=1\r\n"
        "video.fullscreen_width=-4\r\n" // below the minimum: clamped
        "gameplay.tactical_speed=\r\n"  // empty value
        "no equals sign\r\n"
        "nonsense.key=1\r\n"
        "video.detail.99=1\r\n"
        "video.detail.2=77\r\n"         // clamped to the sanity bound
        "video.render_profile=fancy\r\n"
        "key.STOP=X\r\n"
        "key.bad name=Y\r\n"
        "audio.speech=60\r\n");
    expect(!mixed.defaulted, "a damaged body is not a default file");
    expect(mixed.settings.audio.master_percent == 37, "a malformed number keeps the default");
    expect(mixed.settings.audio.sfx_percent == 75, "trailing text is a malformed number");
    expect(mixed.settings.audio.music_percent == 100, "an out-of-range volume is clamped");
    expect(mixed.settings.audio.speech_percent == 60, "valid lines after damage still load");
    expect(mixed.settings.video.windowed, "CRLF line ends load");
    expect(mixed.settings.video.fullscreen_width == ui::display_size_minimum, "a size below the minimum is clamped");
    expect(mixed.settings.gameplay.tactical_speed == 2, "an empty value keeps the default");
    expect(mixed.settings.video.detail[2] == 16, "a detail value is held to the sanity bound");
    expect(mixed.settings.video.render_profile == ui::RenderProfileSetting::automatic,
           "an unknown profile name keeps the default");
    expect(mixed.settings.keys.size() == 1 && mixed.settings.keys.at("STOP") == "X", "only a valid key line loads");
    expect(has_note(mixed, "unknown setting") && has_note(mixed, "bad value") && has_note(mixed, "clamped"),
           "every repair is noted");
}

void test_clamping() {
    ui::PlayerSettings wild;
    wild.audio = {-10, 1000, 101, -1, true};
    wild.video.window_width = 9'999'999;
    wild.video.window_height = 0;
    wild.video.detail_level = 9;
    wild.video.texture_mip_drop = 99;
    wild.video.detail[0] = -50;
    wild.gameplay.tactical_speed = 7;
    wild.gameplay.strategic_speed = -2;
    wild.gameplay.scroll_stop = 99;
    wild.network.port = 70000;
    wild.network.quickmatch_default = 5;
    wild.network.lobby_locale = -9;
    wild.keys["ok"] = "A";
    wild.keys["bad name"] = "A";
    wild.keys["EMPTY"] = "";
    const std::vector<std::string> repaired = ui::clamp_settings(wild);
    expect(repaired.size() >= 10, "every out-of-range field is reported");
    expect(wild.audio.master_percent == 0 && wild.audio.sfx_percent == 100 && wild.audio.speech_percent == 100
               && wild.audio.music_percent == 0,
           "volumes are held to 0..100");
    expect(wild.video.window_width == ui::display_size_maximum && wild.video.window_height == ui::display_size_minimum,
           "sizes are held to the display bounds");
    expect(wild.video.detail_level == 3 && wild.video.detail[0] == -1, "detail values are held");
    expect(wild.video.texture_mip_drop == 4, "GM-04: mip detail is held to the supported drop range");
    expect(wild.gameplay.tactical_speed == 4 && wild.gameplay.strategic_speed == 0 && wild.gameplay.scroll_stop == 9,
           "ST-G1: speeds and scroll stops are held to their steps");
    expect(wild.network.port == 65535 && wild.network.quickmatch_default == 2 && wild.network.lobby_locale == -1,
           "network values are held");
    expect(wild.keys.size() == 1 && wild.keys.count("ok") == 1, "invalid key bindings are dropped");
    expect(ui::clamp_settings(wild).empty(), "a clamped record is stable");
}

void test_option_status() {
    // ST-O1: every audio slider, the resolution list, windowed, vsync, the anti-aliasing slider
    // and the tactical speed are honoured; the rest says why not.
    for (const char* control : {"IDC_SLIDER_MASTER_VOLUME", "IDC_SLIDER_SOUND_EFFECTS_VOLUME", "IDC_SLIDER_DIALOG_VOLUME",
                                "IDC_SLIDER_MUSIC_VOLUME", "IDC_RESOLUTION_LIST", "IDC_WINDOWED_MODE",
                                "IDC_VSYNC_ENABLED", "IDC_ANTI_ALIAS_SLIDER", "IDC_GAME_SPEED_SLIDER"}) {
        const ui::OptionStatus* status = ui::find_option_status(control);
        expect(status != nullptr && status->honoured && status->reason.empty(),
               std::string("ST-O1: ") + control + " is honoured");
    }
    std::set<std::string_view> seen;
    for (const ui::OptionStatus& status : ui::option_statuses()) {
        expect(seen.insert(status.control).second, "one status per control: " + std::string(status.control));
        expect(status.honoured == status.reason.empty(),
               "a refused control says why and an honoured one does not: " + std::string(status.control));
    }
    const ui::OptionStatus* gamma = ui::find_option_status("IDC_GAMMA_SLIDER");
    expect(gamma != nullptr && !gamma->honoured, "an unbuilt option is refused");
    const ui::OptionStatus* key = ui::find_option_status("IDC_EDIT_SELECT_ALL");
    expect(key != nullptr && !key->honoured, "ST-K1: key edit boxes are read-only");
    expect(ui::find_option_status("IDC_OK") == nullptr && ui::find_option_status("IDC_STATIC") == nullptr,
           "buttons and labels have no status");
    expect(ui::project_key_for("IDC_EDIT_SELECT_ALL") == "Ctrl+A" && ui::project_key_for("IDC_EDIT_GROUP_7_SELECT") == "7"
               && ui::project_key_for("IDC_EDIT_STOP") == "S" && ui::project_key_for("IDC_EDIT_SPRINT").empty(),
           "ST-K2: the keyboard page shows the project's keys, blank where there is none");
}

void test_resolution_choices() {
    // ST-V2: the common sizes that fit, the screen's own, and the extras, each once, ascending.
    const ui::DisplaySize extras[] = {{1280, 720}, {1366, 700}, {100, 100}, {99999, 99999}};
    const std::vector<ui::DisplaySize> sizes = ui::resolution_choices({1920, 1080}, extras);
    expect(!sizes.empty() && std::is_sorted(sizes.begin(), sizes.end()), "ST-V2: the list is ascending");
    expect(std::adjacent_find(sizes.begin(), sizes.end()) == sizes.end(), "ST-V2: no size twice");
    expect(std::find(sizes.begin(), sizes.end(), ui::DisplaySize{1920, 1080}) != sizes.end()
               && std::find(sizes.begin(), sizes.end(), ui::DisplaySize{1280, 720}) != sizes.end()
               && std::find(sizes.begin(), sizes.end(), ui::DisplaySize{1366, 700}) != sizes.end(),
           "ST-V2: the screen's size and a usable extra are listed");
    expect(std::none_of(sizes.begin(), sizes.end(),
                        [](const ui::DisplaySize& size) { return size.width > 1920 || size.height > 1080
                                                              || size.width < ui::display_size_minimum; }),
           "ST-V2: nothing larger than the screen, nothing below the minimum, unusable extras dropped");
    expect(ui::resolution_choices({0, 0}, {}).empty(), "no screen and no extras is an empty list");
}

void test_session_values() {
    // The start-up apply and a battle read the saved record through one mapping: each volume
    // slider reaches its own bus and the tactical speed reaches the speed step.
    const ui::SessionValues defaults = ui::session_values(ui::PlayerSettings{});
    expect(defaults.master_gain == 0.37 && defaults.sfx_gain == 0.75 && defaults.speech_gain == 0.75
               && defaults.music_gain == 0.75 && defaults.speed_step == 2,
           "the defaults reach a battle as master 0.37, channels 0.75 and speed step 2");
    const ui::SettingsLoad existing = ui::parse_settings(
        "eawr-settings 1\naudio.master=75\naudio.sfx=80\naudio.speech=65\naudio.music=90\nvideo.vsync=0\n");
    expect(!existing.defaulted && existing.settings.audio.master_percent == 75
               && existing.settings.audio.sfx_percent == 80 && existing.settings.audio.speech_percent == 65
               && existing.settings.audio.music_percent == 90 && !existing.settings.video.vsync,
           "stored sliders and disabled vsync keep their values, including former defaults");
    const ui::SettingsLoad saved = ui::parse_settings(
        "eawr-settings 1\naudio.master=30\naudio.sfx=40\naudio.speech=55\naudio.music=100\n"
        "gameplay.tactical_speed=4\n");
    expect(!saved.defaulted, "the saved fixture is a settings file");
    const ui::SessionValues values = ui::session_values(saved.settings);
    expect(values.master_gain == 0.30 && values.sfx_gain == 0.40 && values.speech_gain == 0.55
               && values.music_gain == 1.0 && values.speed_step == 4,
           "saved volumes and speed reach their own bus and the speed step");
    const ui::SessionValues round_trip = ui::session_values(ui::parse_settings(ui::serialize_settings(saved.settings)).settings);
    expect(round_trip.master_gain == values.master_gain && round_trip.speed_step == values.speed_step,
           "the mapping survives a save and a load");
}

} // namespace

int main() {
    test_default_sfx_catalog_unchanged();
    test_selection_layer_config();
    test_revised_settings();
    test_selection_hotkeys();
    test_performance_deviations();
    test_online_settings();
    test_performance_thread_settings();
    test_render_thread_settings();
    test_animated_loading_background_settings();
    test_reinforcement_area_settings();
    test_defaults();
    expect(ui::hover_owner_names_enabled(ui::GameplaySettings{}, true), "hover names default on in multiplayer");
    expect(!ui::hover_owner_names_enabled(ui::GameplaySettings{}, false), "hover names default off in solo");
    ui::PlayerSettings hover_settings;
    hover_settings.gameplay.hover_owner_names = false;
    expect(!ui::parse_settings(ui::serialize_settings(hover_settings)).settings.gameplay.hover_owner_names,
        "hover names disabled choice persists");
    hover_settings.gameplay.hover_owner_names = 1;
    expect(ui::hover_owner_names_enabled(ui::parse_settings(ui::serialize_settings(hover_settings)).settings.gameplay, false),
        "hover names explicit on persists in solo");
    hover_settings.gameplay.hover_owner_names = -1;
    expect(ui::parse_settings(ui::serialize_settings(hover_settings)).settings.gameplay.hover_owner_names == -1,
        "hover names mode default persists");
    test_camera_keys();
    test_frame_pacing();
    test_round_trip();
    test_garbage();
    test_clamping();
    test_option_status();
    test_resolution_choices();
    test_session_values();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " settings check(s) failed\n";
        return 1;
    }
    std::cout << "ui settings passed\n";
    return 0;
}

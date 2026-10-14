// The skirmish battle loading screen's model (docs/behaviour/loading-screen.md, rules BLD-01 to
// BLD-14, cases BLC-01 to BLC-08): what the dialog shows for a map, the weather icon and preview
// names, the history wrapping and the load progress. A synthetic Planets file pins the rules; with
// EAWR_EAW_GAME_ROOT set, the stock Coruscant map's screen is read from the installed game.

#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/loading_screen.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

std::vector<std::byte> text_file(const std::vector<std::pair<std::string, std::u16string>>& records) {
    std::vector<std::byte> out;
    const auto u32 = [&](const std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
    };
    u32(static_cast<std::uint32_t>(records.size()));
    for (const auto& [key, value] : records) {
        u32(data::ui::text_key_crc(key));
        u32(static_cast<std::uint32_t>(value.size()));
        u32(static_cast<std::uint32_t>(key.size()));
    }
    for (const auto& record : records) {
        for (const char16_t c : record.second) {
            out.push_back(static_cast<std::byte>(c & 0xFF));
            out.push_back(static_cast<std::byte>(c >> 8));
        }
    }
    for (const auto& record : records) {
        for (const char c : record.first) out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

void names() {
    expect(ui::loading_weather_icon("i_pa_weather_sun.tga") == "load_weather_sun.tga",
           "BLC-01: the encyclopedia icon's i_pa becomes load");
    expect(ui::loading_weather_icon("i_pa_weather_none.tga") == "load_weather_none.tga", "BLC-01: the none icon follows");
    expect(ui::loading_weather_icon("sun.tga") == "sun.tga", "BLC-01: a name without the prefix stays as written");
    expect(ui::loading_weather_icon("") .empty(), "BLC-01: no name, no icon");
    expect(ui::loading_map_icon_path("data/art/maps/_mp_space_coruscant.ted") == "data/art/textures/_mp_space_coruscant.dds",
           "BLC-02: the preview is the map's name with .dds under the textures");
    expect(ui::loading_map_icon_path("Data\\Art\\Maps\\_MP_Space_Hoth.TED") == "data/art/textures/_MP_Space_Hoth.dds",
           "BLC-02: backslashes and a capital extension are read as well");
    expect(ui::loading_map_icon_path("data/art/maps/").empty() && ui::loading_map_icon_path("").empty(),
           "BLC-02: a path without a file name has no preview");
}

void wrapping() {
    const auto measure = [](const std::string_view text) { return static_cast<double>(text.size()); };
    const auto lines = ui::loading_wrap_lines("alpha beta gamma delta", 10.0, measure);
    expect(lines.size() == 3 && lines[0] == "alpha beta" && lines[1] == "gamma" && lines[2] == "delta",
           "BLC-03: a line breaks before the first word that does not fit");
    const auto exact = ui::loading_wrap_lines("one two", 7.0, measure);
    expect(exact.size() == 1 && exact[0] == "one two", "BLC-03: a line may fill the width exactly");
    const auto wide = ui::loading_wrap_lines("tiny enormousword tiny", 6.0, measure);
    expect(wide.size() == 3 && wide[1] == "enormousword", "BLC-03: a word wider than the line stands alone");
    const auto paragraphs = ui::loading_wrap_lines("one\ntwo\r\n\nthree", 20.0, measure);
    expect(paragraphs.size() == 4 && paragraphs[1] == "two" && paragraphs[2].empty() && paragraphs[3] == "three",
           "BLC-03: a newline ends a line, a blank line stays");
    expect(ui::loading_wrap_lines("", 10.0, measure) == std::vector<std::string>{std::string()},
           "BLC-03: an empty text is one empty line");
}

void progress() {
    double total = 0.0;
    for (const auto& stage : ui::battle_load_stages) total += stage.weight;
    expect(total > 0.999 && total < 1.001, "BLC-04: the stage weights add up to one");
    ui::LoadProgress progress;
    expect(progress.fraction() == 0.0 && !progress.finished(), "BLC-04: the bar starts empty");
    progress.report(ui::load_stage_session, 0, 4);
    const double after_map = progress.fraction();
    expect(after_map > 0.049 && after_map < 0.051, "BLC-04: earlier stages count as done");
    progress.report(ui::load_stage_session, 2, 4);
    const double half = progress.fraction();
    expect(half > after_map && half < 0.2, "BLC-04: a stage's items fill its share");
    progress.report(ui::load_stage_session, 1, 4);
    expect(progress.fraction() == half, "BLC-04: the bar never moves back");
    progress.report(ui::load_stage_interface, 7, 7);
    progress.report(ui::load_stage_scene, 5, 5);
    expect(progress.fraction() < 1.0 && !progress.finished(), "BLC-04: finished stages do not fill the bar");
    progress.report(99, 1, 1);
    progress.report(ui::load_stage_scene, 0, 0);
    expect(progress.fraction() < 1.0, "BLC-04: an unknown stage is ignored and an empty stage is done");
    progress.complete();
    expect(progress.fraction() == 1.0 && progress.finished() && progress.stage_name() == "scene",
           "BLC-04: only complete() fills the bar");
    progress.report(ui::load_stage_map, 0, 1);
    expect(progress.fraction() == 1.0, "BLC-04: a late report changes nothing");
}

void screen_content() {
    test::ui::TempTree tree("loading-screen");
    const auto xml = tree.root / "XML";
    test::ui::write_text(xml / "GameObjectFiles.xml", "<Game_Object_Files><File>planets.xml</File></Game_Object_Files>");
    test::ui::write_text(xml / "planets.xml", R"xml(<Planets>
<Planet Name="Coruscant"><Text_ID> TEXT_NAME </Text_ID>
<Describe_Population>TEXT_POP</Describe_Population><Describe_Terrain>TEXT_TER</Describe_Terrain>
<Describe_Weather>TEXT_WEA</Describe_Weather><Describe_History>TEXT_HIS</Describe_History>
<Describe_Tactical>TEXT_TER</Describe_Tactical><Describe_Advantage>TEXT_POP</Describe_Advantage>
<Native_Icon_Name>load_royal_guard.tga</Native_Icon_Name>
<Encyclopedia_Weather_Icon>i_pa_weather_sun.tga</Encyclopedia_Weather_Icon></Planet>
<Planet Name="Bare"><Text_ID>TEXT_NAME</Text_ID></Planet>
</Planets>)xml");
    for (const auto* registry : {"HardpointDataFiles.xml", "FactionFiles.xml", "CampaignFiles.xml", "SFXEventFiles.xml"})
        test::ui::write_text(xml / registry, "<Files/>");
    test::ui::write_text(xml / "units.xml", "<Units><SpaceUnit Name=\"Kuat\"/></Units>");
    const std::array mounts{vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "loading fixture mounts");
    if (!mounted) return;
    auto catalog = data::load_catalog(mounted.value(), data::Profile::foc);
    expect(static_cast<bool>(catalog), "loading fixture catalog loads");
    if (!catalog) return;
    const auto bytes = text_file({{"TEXT_PREPARE_FOR_SPACE_BATTLE", u"Space Battle at"},
                                  {"TEXT_PREPARE_FOR_LAND_BATTLE", u"Land Battle at"}, {"TEXT_NAME", u"Coruscant"},
                                  {"TEXT_POP", u"Human"}, {"TEXT_TER", u"Urban"}, {"TEXT_WEA", u"Clear"},
                                  {"TEXT_HIS", u"The center of the universe."}, {"MTKNOWN", u"Mission heading"},
                                  {"MBKNOWN", u"Mission body"}, {"TEXT_UNRELATED_BRIEFING", u"Valid corruption briefing"}});
    assets::Source source;
    source.logical_path = "data/text/mastertextfile_english.dat";
    auto database = data::ui::load_text_database(bytes, source);
    expect(static_cast<bool>(database), "loading fixture text DB loads");
    if (!database) return;
    data::ui::TextLookup text(database.value());
    const auto* objects = &catalog.value().catalog;
    ui::LoadingScreenInput input;
    input.map_path = "data/art/maps/_mp_space_coruscant.ted";
    input.context_name = "coruscant";
    input.map_name = "Coruscant Siege";
    input.objects = objects;
    input.text = &text;
    input.exists = [](const std::string_view path) { return path == "data/art/textures/_mp_space_coruscant.dds"; };
    const auto content = ui::loading_screen_content(input);
    expect(content.dialog == "IDD_SKIRMISH_BATTLE_LOAD_DIALOG" && content.planet == "Coruscant",
           "BLC-05: a skirmish battle uses the skirmish dialog and the planet its map names");
    expect(content.title == "Space Battle at Coruscant", "BLC-05: the title is the battle text and the planet's name");
    expect(content.history == "The center of the universe." && content.weather == "Clear" && content.terrain == "Urban"
               && content.population == "Human",
           "BLC-05: the texts are the planet's Describe_* keys through the text database");
    expect(content.native_icon == "load_royal_guard.tga" && content.weather_icon == "load_weather_sun.tga"
               && content.overlay_icon == "load_overlay.tga" && content.map_icon == "data/art/textures/_mp_space_coruscant.dds",
           "BLC-05: the pictures are the planet's native and weather icons, the overlay and the map's preview");
    expect(content.diagnostics.empty() && text.diagnostics().empty(), "BLC-05: a complete planet raises nothing");
    input.campaign = true;
    const auto campaign = ui::loading_screen_content(input);
    expect(campaign.dialog == "IDD_SPACE_BATTLE_LOAD_DIALOG" && campaign.title == "Space Battle at Coruscant"
        && campaign.history == content.history && campaign.map_icon == content.map_icon
        && campaign.tactical == "Urban" && campaign.advantage == "Human" && content.tactical.empty(),
        "WGP-31: campaign uses its mounted dialog, the same title order as a skirmish and the same static descriptions");
    for (const auto kind : {ui::LoadingMission::tutorial, ui::LoadingMission::story, ui::LoadingMission::corruption}) {
        input.mission = kind; input.mission_map = "Known.TeD.backup";
        for (const auto layer : {ui::LoadingBattle::space, ui::LoadingBattle::land}) {
            input.battle = layer;
            const auto mission = ui::loading_screen_content(input);
            expect(mission.dialog == (layer == ui::LoadingBattle::space ? "IDD_SPACE_MISSION_LOAD_DIALOG" : "IDD_LAND_MISSION_LOAD_DIALOG")
                && mission.title == "Coruscant"
                && mission.mission_heading == "Mission heading" && mission.mission_body == "Mission body"
                && mission.history == content.history,
                "WGP-75/169: explicit mission kind selects either mounted resource, with independent loading/static text");
        }
    }
    input.context_name = "UnavailableLocation"; input.map_name = "RetainedLocation";
    for (const auto kind : {ui::LoadingMission::tutorial, ui::LoadingMission::story, ui::LoadingMission::corruption}) {
        input.mission = kind;
        for (const auto layer : {ui::LoadingBattle::space, ui::LoadingBattle::land}) {
            input.battle = layer;
            expect(ui::loading_screen_content(input).title == "RetainedLocation",
                "WGP-76: unresolved retained mission location also omits preparation suffix");
        }
    }
    input.context_name = "coruscant"; input.map_name = "Coruscant Siege";
    input.mission_map = "Dir/MiXeD.tEd.extra.TED";
    const auto missing_mission = ui::loading_screen_content(input);
    expect(missing_mission.mission_heading == "MTDIR/MIXED" && missing_mission.mission_body == "MBDIR/MIXED",
        "WGP-169: missing translation preserves uppercased full path through first .TED, never unrelated briefing");
    input.mission_map.reset();
    const auto unavailable_map = ui::loading_screen_content(input);
    expect(unavailable_map.mission_heading.empty() && unavailable_map.mission_body.empty() && !unavailable_map.diagnostics.empty(),
        "WGP-170: absent retained dialog map never derives loading text from selected child map");
    input.mission = ui::LoadingMission::ordinary; input.battle = ui::LoadingBattle::land;
    expect(ui::loading_screen_content(input).dialog == "IDD_LAND_BATTLE_LOAD_DIALOG", "WGP-75: ordinary land resource is distinct from missions");
    input.campaign = false;

    input.battle = ui::LoadingBattle::land;
    expect(ui::loading_screen_content(input).title == "Land Battle at Coruscant", "BLC-05: a land battle says so");
    input.battle = ui::LoadingBattle::space;

    input.exists = [](const std::string_view) { return false; };
    expect(ui::loading_screen_content(input).map_icon.empty(), "BLC-06: a map without a preview texture shows none");

    input.context_name = "Bare";
    const auto bare = ui::loading_screen_content(input);
    expect(bare.title == "Space Battle at Coruscant" && bare.history.empty() && bare.native_icon.empty()
               && bare.weather_icon.empty() && bare.diagnostics.empty(),
           "BLC-07: a planet with no Describe_* or icon tags leaves those gadgets empty");

    for (const char* context : {"Kuat", "Nowhere", ""}) {
        input.context_name = std::string(context);
        const auto fallback = ui::loading_screen_content(input);
        expect(fallback.title == "Space Battle at Coruscant Siege" && fallback.planet.empty() && fallback.history.empty()
                   && fallback.diagnostics.size() == 1U,
               "BLC-08: no planet the data describe gives the map's name, an empty body and one warning");
    }
    input.context_name = std::nullopt;
    input.map_name.clear();
    expect(ui::loading_screen_content(input).title == "Space Battle at", "BLC-08: without any name the title is the battle text");
    input.objects = nullptr;
    input.text = nullptr;
    input.context_name = "Coruscant";
    expect(ui::loading_screen_content(input).title == "TEXT_PREPARE_FOR_SPACE_BATTLE Coruscant",
           "BLC-08: without a catalogue and a text database the keys and the context name show as written");
}

void corpus() {
    auto filesystem = test::ui::foc_corpus("Loading screen");
    if (!filesystem) return;
    auto objects = data::load_catalog(*filesystem, data::Profile::foc);
    auto database = data::ui::load_language_text_database(*filesystem, "ENGLISH");
    expect(objects && database, "FoC objects and text load");
    if (!objects || !database) return;
    data::ui::TextLookup text(database.value());
    ui::LoadingScreenInput input;
    input.map_path = "data/art/maps/_mp_space_coruscant.ted";
    input.context_name = "Coruscant";
    input.objects = &objects.value().catalog;
    input.text = &text;
    input.exists = [&](const std::string_view path) { return static_cast<bool>(filesystem->stat(std::string(path))); };
    const auto content = ui::loading_screen_content(input);
    expect(content.title == "Space Battle at Coruscant" && content.weather == "Clear" && content.terrain == "Urban"
               && content.population == "Human" && content.native_icon == "load_royal_guard.tga"
               && content.weather_icon == "load_weather_sun.tga"
               && content.map_icon == "data/art/textures/_mp_space_coruscant.dds"
               && content.history.rfind("Coruscant is known as the center of the universe", 0) == 0
               && content.diagnostics.empty() && text.diagnostics().empty(),
           "BLC-01..05 on the corpus: the stock Coruscant skirmish screen matches the retail capture's texts and pictures");
}

} // namespace

int main() {
    {
        ui::CampaignLoadingGate gate;
        gate.request(true);
        expect(gate.unsafe() && !gate.take(true), "WGP-113: unsafe input cannot arm Begin");
        gate.complete(true);
        expect(!gate.unsafe() && gate.ready() && !gate.take(true), "WGP-112: completion retains the barrier");
        gate.request(false);
        expect(!gate.take(true), "WGP-114: a non-topmost request is rejected");
        gate.request(true);
        expect(!gate.take(false) && gate.take(true) && !gate.take(true), "WGP-114: accepted topmost Begin is consumed once");
        ui::CampaignLoadingGate failed;
        failed.complete(false); failed.complete(true); failed.request(true);
        expect(!failed.ready() && !failed.take(true), "WGP-80: failure never becomes ready");
    }
    names();
    wrapping();
    progress();
    screen_content();
    corpus();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " loading screen contract(s) failed\n";
        return 1;
    }
    std::cout << "loading screen contracts passed\n";
    return 0;
}

// #8396 / WGUI-166: the header of the fleet encyclopedia popup names the owner's faction. The three GameConstants
// sides and the Pirates literal keep the stock keys; every other faction is named by its own display name through the
// project format keys, so a player's own fleet never reads as the enemy.
#include "galactic/fleet_popup_title.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using eawr::test::ui::expect;
namespace view = eawr::viewer::galactic;
namespace ui = eawr::presentation::ui;
namespace data_ui = eawr::data::ui;

// A text database holding exactly `rows` (the stock-side texts of a mod whose generic pirate text reads "Enemy Fleet").
data_ui::TextDatabase database(std::vector<std::pair<std::string, std::u16string>> rows) {
    data_ui::TextDatabase result;
    for (auto& [key, value] : rows) {
        result.entries.push_back({data_ui::text_key_crc(key), std::move(key), std::move(value)});
    }
    for (std::uint32_t index = 0; index < result.entries.size(); ++index) result.crc_order.push_back(index);
    std::sort(result.crc_order.begin(), result.crc_order.end(), [&](const std::uint32_t a, const std::uint32_t b) {
        return result.entries[a].crc < result.entries[b].crc;
    });
    return result;
}

std::vector<std::pair<std::string, std::u16string>> side_texts() {
    return {{"TEXT_GOOD_SPACE_FLEET", u"Rebellion Fleet"}, {"TEXT_EVIL_SPACE_FLEET", u"Empire Fleet"},
        {"TEXT_CORRUPT_SPACE_FLEET", u"Underworld Fleet"}, {"TEXT_PIRATE_SPACE_FLEET", u"Enemy Fleet"},
        {"TEXT_GOOD_LAND_GARRISON", u"Rebel Garrison"}, {"TEXT_EVIL_LAND_GARRISON", u"Imperial Garrison"},
        {"TEXT_CORRUPT_LAND_GARRISON", u"Underworld Garrison"}, {"TEXT_PIRATE_LAND_GARRISON", u"Pirate Garrison"}};
}

bool mentions_enemy(const std::string& title) {
    std::string lower = title;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("enemy") != std::string::npos;
}

void titles(const data_ui::TextDatabase& db) {
    data_ui::TextLookup lookup(db);
    const std::function<std::string(std::string_view)> text = [&lookup](const std::string_view key) {
        return data_ui::to_utf8(lookup.text(key));
    };
    const ui::SideNames sides{"Rebel", "Empire", "Underworld"};
    // The stock sides keep their stock keys, whatever the case of the faction name.
    expect(view::fleet_popup_title("Rebel", sides, "Rebellion", false, text) == "Rebellion Fleet", "good side: stock key");
    expect(view::fleet_popup_title("EMPIRE", sides, "Empire", false, text) == "Empire Fleet", "evil side: stock key");
    expect(view::fleet_popup_title("Underworld", sides, "Criminal Underworld", false, text) == "Underworld Fleet", "corrupt side: stock key");
    expect(view::fleet_popup_title("Rebel", sides, "Rebellion", true, text) == "Rebel Garrison", "good side ground: stock key");
    expect(view::fleet_popup_title("Underworld", sides, "x", true, text) == "Underworld Garrison", "corrupt side ground: stock key");
    // The engine's Pirates literal keeps the stock pirate key.
    expect(view::fleet_popup_title("Pirates", sides, "Independent Forces", false, text) == "Enemy Fleet", "Pirates: stock key");
    expect(view::fleet_popup_title("Pirates", sides, "Independent Forces", true, text) == "Pirate Garrison", "Pirates ground: stock key");

    // Every other faction of the Remake stack (display name from its Text_ID): its own name, never "enemy".
    const std::vector<std::pair<std::string, std::string>> extra{{"Republic", "Republic Hardliners"},
        {"CIS", "Separatist Holdouts"}, {"CSA", "Corporate Sector Authority"}, {"Warlords", "Imperial Defectors"},
        {"Hapan", "Hapes Consortium"}, {"Mandalorians", "Mandalorian Protectors"}, {"Sith", "Sith Cultists"},
        {"Consortium", "Zann Consortium"}, {"Cartels", "Hutt Empire"}, {"BlackSun", "Black Sun"},
        {"Moffs", "Imperial Moffs"}, {"Junta", "Junta"}, {"Hand", "Empire of the Hand"}, {"DarkEmpire", "Dark Empire"},
        {"Hutts", "Invaders"}};
    for (const auto& [faction, name] : extra) {
        const auto fleet = view::fleet_popup_title(faction, sides, name, false, text);
        const auto garrison = view::fleet_popup_title(faction, sides, name, true, text);
        expect(fleet == name + " Fleet", "an extra faction's fleet is its display name plus Fleet");
        expect(garrison == name + " Garrison", "an extra faction's garrison is its display name plus Garrison");
        expect(!mentions_enemy(fleet) && !mentions_enemy(garrison), "no extra faction reads as the enemy");
    }
    // A faction with no display name text falls back to its own faction name, an unnamed one to the stock pirate key.
    expect(view::fleet_popup_title("Republic", sides, "", false, text) == "Republic Fleet", "no display name: the faction name");
    expect(view::fleet_popup_title("", sides, "", false, text) == "Enemy Fleet", "nothing to name: the stock pirate key");
    // A mod that renames its sides keeps the sides on their stock keys, and the old stock names become extra factions.
    const ui::SideNames modded{"Alliance", "Imperium", ""};
    expect(view::fleet_popup_title("Alliance", modded, "The Alliance", false, text) == "Rebellion Fleet", "a renamed good side keeps its key");
    expect(view::fleet_popup_title("Rebel", modded, "Rebel", false, text) == "Rebel Fleet", "a retail name that is not a side is named");
    expect(view::fleet_popup_title("Underworld", modded, "Underworld", true, text) == "Underworld Garrison", "an unset corrupt side matches nothing");
}

void project_format_keys() {
    // The built-in defaults serve a text table without the project keys.
    const auto plain = database(side_texts());
    titles(plain);
    // A mod text table may override either format (any token form the stock text uses).
    auto rows = side_texts();
    rows.push_back({"TEXT_EAWR_FACTION_FLEET_FORMAT", u"Flotte : %s"});
    rows.push_back({"TEXT_EAWR_FACTION_GARRISON_FORMAT", u"Garnison : %s"});
    const auto french = database(std::move(rows));
    data_ui::TextLookup lookup(french);
    const std::function<std::string(std::string_view)> text = [&lookup](const std::string_view key) {
        return data_ui::to_utf8(lookup.text(key));
    };
    const ui::SideNames sides{"Rebel", "Empire", "Underworld"};
    expect(view::fleet_popup_title("Republic", sides, "Hardliners", false, text) == "Flotte : Hardliners", "a mod overrides the fleet format");
    expect(view::fleet_popup_title("Republic", sides, "Hardliners", true, text) == "Garnison : Hardliners", "a mod overrides the garrison format");
    expect(view::fleet_popup_title("Empire", sides, "Empire", false, text) == "Empire Fleet", "the stock sides ignore the project formats");
    // A text source with no project table at all still names the faction.
    const std::function<std::string(std::string_view)> keys_only = [](const std::string_view key) { return std::string(key); };
    expect(view::fleet_popup_title("Republic", sides, "Hardliners", false, keys_only) == "Hardliners", "no text for the format: the bare name");
    expect(plain.find("TEXT_EAWR_FACTION_FLEET_FORMAT") == nullptr, "the plain table carries no project key");
    data_ui::TextLookup built_in(plain);
    expect(built_in.text("TEXT_EAWR_FACTION_FLEET_FORMAT") == u"%s Fleet", "built-in fleet format");
    expect(built_in.text("TEXT_EAWR_FACTION_GARRISON_FORMAT") == u"%s Garrison", "built-in garrison format");
    expect(built_in.diagnostics().empty(), "a built-in default reports nothing");
}
} // namespace

int main() {
    project_format_keys();
    return eawr::test::ui::failures() ? 1 : 0;
}

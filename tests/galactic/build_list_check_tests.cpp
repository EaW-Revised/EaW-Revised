#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <fstream>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/text.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/vfs/vfs.hpp"

// #8161: the standing authored-versus-offered check. For every campaign of one installed content stack (stock, or a
// workshop mod chain over it) this compares what the data authors for a build tab (a type with a Build_Tab_* flag whose
// Affiliation names a faction of the campaign) with what the production setup offers. Every type gets one reason token (the
// token set is stable: scripts and the standing report match on it):
//   offered               a production offer admits it for direct building (a later gate may still lock it: tech, a required
//                         structure, a base level; those show locked in the tray and are not differences)
//   retail-refused        the original's own gate refuses it as well (cinematic-only, a ground-base type, a build cost below 1,
//                         a named hero that authors no base level, a company or squadron member): reported, never a defect
//   restricted            Required_Planets names no planet of the campaign
//   no-role               no DUMMY_* production behaviour: not a build wrapper
//   DEFECT-dropped        authored for a tab and affiliated with a campaign faction, but no production row exists
//   DEFECT-hidden         a row exists, direct building is refused, and none of the retail reasons holds
//   DEFECT-wrong-list     offered, but on the other list than the original's build-domain gate selects (ground when the effective
//                         required ground level is nonzero or the type is a ground structure, space otherwise; the tab flags do
//                         not decide it: GHUD-S90, so a flag that disagrees is only reported as tab-flag-differs, never a defect)
//   DEFECT-unregistered   an authored build wrapper (tab flag, affiliation, DUMMY_* behaviour) missing from the galactic type table
// Any DEFECT fails the test. Named cases: the battle droids of Republic at War (level 0 in the data, promoted to ground level 1
// by the original's load fixup, WGEP-158) are offered on the ground tab. Stock, Remake and BFC have no named case: none is known
// to rely on the fixup there, so their runs check the token classes only.
namespace {
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;

struct MissingMod : std::runtime_error { using std::runtime_error::runtime_error; };

std::vector<std::filesystem::path> workshop_roots(const std::filesystem::path& game) {
    std::vector<std::filesystem::path> roots;
    if (const auto workshop = eawr::testing::environment("EAWR_WORKSHOP_ROOT")) roots.emplace_back(*workshop);
    if (const auto mod = eawr::testing::environment("EAWR_REMAKE_MOD_ROOT")) roots.push_back(std::filesystem::path(*mod).parent_path());
    roots.push_back(game.parent_path().parent_path() / "workshop" / "content" / "32470");
    roots.push_back(game.parent_path() / "workshop" / "content" / "32470");
    roots.push_back(game.parent_path().parent_path() / "mods" / "workshop" / "content" / "32470");
    return roots;
}

std::vector<std::string> split_chain(const std::string& chain, const char separator = ';') {
    std::vector<std::string> ids;
    std::stringstream stream(chain);
    for (std::string id; std::getline(stream, id, separator);) if (!id.empty()) ids.push_back(id);
    return ids;
}

// `mod_chain` is empty (stock) or one workshop id, or a leaf-first ';' chain of ids (a mod over its parents).
std::vector<eawr::vfs::MountSpec> mount_stack(const std::string& mod_chain) {
    const auto root = eawr::testing::game_root();
    if (!root) throw MissingMod("set EAWR_EAW_GAME_ROOT");
    const std::filesystem::path game(*root);
    std::vector<eawr::vfs::LayerRoot> layers;
    const auto ids = split_chain(mod_chain);
    if (!ids.empty()) {
        for (const auto& workshop : workshop_roots(game)) {
            std::error_code unreadable;
            bool all = true;
            for (const auto& id : ids) all = all && std::filesystem::is_directory(workshop / id, unreadable) && !unreadable;
            if (!all) continue;
            std::string joined;
            for (const auto& id : ids) joined += (joined.empty() ? "" : ";") + (workshop / id).string();
            layers = eawr::vfs::mod_chain_roots(std::filesystem::path(joined));
            break;
        }
        if (layers.empty()) throw MissingMod("workshop mod chain " + mod_chain + " is not installed on this host");
    }
    layers.emplace_back("expansion", game / "corruption" / "Data");
    layers.emplace_back("base", game / "GameData" / "Data");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (auto& layer : take(eawr::vfs::resolve_manifest_chain(layers))) mounts.push_back(std::move(layer.mount));
    return mounts;
}

struct TypeSeen {
    bool row{}, direct{}, cinematic{}, ground{};
    std::size_t planets{};
};

int check(const std::string& label, const std::string& mod_id, const std::vector<std::string>& must_offer) {
    const auto mounts = mount_stack(mod_id);
    const auto files = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
    const auto loaded = take(eawr::data::load_catalog(*files, mod_id.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake));
    const auto data = take(eawr::data::galactic::load_galactic_data(*files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    std::vector<std::string> names;
    for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    std::map<std::string, gc::TypeId> ids;
    for (const auto& name : names) ids[name] = static_cast<gc::TypeId>(ids.size() + 1);

    const char* tabs[] = {"Build_Tab_Land_Units", "Build_Tab_Space_Units", "Build_Tab_Heroes", "Build_Tab_Special_Structures",
        "Build_Tab_Outpost", "Build_Tab_Space_Station"};
    const auto layers_of = [&](const std::string& name) {
        std::vector<const eawr::data::Definition*> layers;
        const auto resolved = loaded.catalog.resolve(name, eawr::data::Category::game_object);
        if (!resolved) return layers;
        for (const auto& link : resolved.value().chain)
            if (const auto* layer = loaded.catalog.find(link, eawr::data::Category::game_object)) layers.push_back(layer);
        return layers;
    };
    const auto flag_of = [&](const std::string& name, const char* tag) {
        const auto layers = layers_of(name);
        if (const auto* node = eawr::data::retail::single(layers, tag)) {
            const auto value = eawr::data::retail::boolean(node->raw_text);
            return value && *value;
        }
        return false;
    };
    const auto in_tab = [&](const std::string& name) {
        for (const auto* tab : tabs) if (flag_of(name, tab)) return true;
        return false;
    };
    const auto text_of = [&](const std::string& name, const char* tag) {
        const auto layers = layers_of(name);
        const auto* node = eawr::data::retail::single(layers, tag);
        return node ? node->raw_text : std::string{};
    };
    std::vector<const eawr::units::GalacticType*> authored;
    for (const auto& type : types.types) if (!type.affiliation.empty() && in_tab(type.name)) authored.push_back(&type);

    std::size_t campaigns_run{}, offered_total{}, retail_refused{}, restricted{}, defects{}, no_role{}, unregistered{}, flag_differs{};
    std::set<std::string> reported;
    {
        // The authored build wrappers the galactic type table never registered: they cannot be offered, whatever the setup does.
        std::set<std::string> registered, factions_all;
        for (const auto& name : names) registered.insert(eawr::core::to_lower(name));
        for (const auto& row : data.factions) factions_all.insert(eawr::core::to_lower(row.name));
        for (const auto& definition : loaded.catalog.definitions()) {
            if (definition.category != eawr::data::Category::game_object || !definition.winner) continue;
            if (registered.contains(eawr::core::to_lower(definition.id)) || !in_tab(definition.id)) continue;
            if (text_of(definition.id, "Behavior").find("DUMMY_") == std::string::npos) continue;
            if (flag_of(definition.id, "Cinematic_Object_Only")) continue;
            const auto affiliation = eawr::core::to_lower(text_of(definition.id, "Affiliation"));
            bool wanted = false;
            for (const auto& faction : factions_all) wanted = wanted || affiliation.find(faction) != std::string::npos;
            if (!wanted) continue;
            ++unregistered; ++defects;
            const auto line = label + " " + definition.id + " DEFECT-unregistered: authored build wrapper missing from the galactic type table";
            std::cout << "build-list " << line << '\n';
            expect(false, line);
        }
    }
    std::map<std::string, bool> must_seen;
    for (const auto& name : must_offer) must_seen[name] = false;
    for (const auto& source : data.campaigns) {
        eawr::campaign::CampaignSetupOptions options;
        options.seed = 42;
        std::set<std::string> factions;
        for (const auto& row : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            options.players.push_back({{id, id, 0}, row.name, true});
            factions.insert(eawr::core::to_lower(row.name));
            const bool human = row.name == source.starting_active_player;
            if (human) options.solo_reference_human = id;
            if (!row.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
                human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
        }
        for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
        auto created = eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, data, types, source.name, options,
            "Normal_Default", false);
        if (!created) {
            std::cout << "build-list " << label << " " << source.name << " SKIPPED: " << eawr::core::format_diagnostic(created.error()) << '\n';
            continue;
        }
        const auto prepared = std::move(created).value();
        ++campaigns_run;
        std::set<std::string> planet_names;
        for (const auto& location : prepared->plan().locations()) if (location.planet) planet_names.insert(eawr::core::to_lower(location.original_type));
        std::map<gc::TypeId, TypeSeen> seen;
        if (prepared->layout().production)
            for (const auto& planet : prepared->layout().production->planets)
                for (const auto& offer : planet.offers) {
                    auto& row = seen[offer.type];
                    row.row = true; ++row.planets;
                    row.direct = row.direct || offer.direct_build_allowed;
                    row.cinematic = row.cinematic || offer.cinematic;
                    row.ground = row.ground || offer.domain == gc::ProductionDomain::ground;
                }
        const auto has_behavior = [](const eawr::units::GalacticType& type, const char* role) {
            return std::any_of(type.behavior.begin(), type.behavior.end(), [&](const auto& value) { return eawr::core::iequals(value, role); });
        };
        for (const auto* type : authored) {
            bool wanted = false;
            for (const auto& faction : type->affiliation) wanted = wanted || factions.contains(eawr::core::to_lower(faction));
            if (!wanted) continue;
            const auto id = ids.at(type->name);
            const auto at = seen.find(id);
            const bool has_row = at != seen.end() && at->second.row;
            const auto level_ground = type->required_ground_base_level.value_or(0), level_star = type->required_star_base_level.value_or(0);
            const bool cost_ok = type->build_cost_credits.value_or(eawr::sim::math::Fixed{}).raw() >= eawr::sim::math::Fixed::scale;
            const bool constructible = cost_ok && (level_ground != 0 || level_star != 0 || has_behavior(*type, "DUMMY_GROUND_STRUCTURE")
                || (has_behavior(*type, "DUMMY_STAR_BASE") && type->base_level.value_or(0) == 1));
            const bool production_role = has_behavior(*type, "DUMMY_GROUND_COMPANY") || has_behavior(*type, "DUMMY_STARSHIP")
                || has_behavior(*type, "DUMMY_SPACE_FIGHTER_SQUADRON") || has_behavior(*type, "DUMMY_GROUND_STRUCTURE")
                || has_behavior(*type, "DUMMY_ORBITAL_STRUCTURE") || has_behavior(*type, "DUMMY_STAR_BASE");
            const bool domain_ground = level_ground != 0 || has_behavior(*type, "DUMMY_GROUND_STRUCTURE");
            std::string verdict, detail;
            const bool land_tab = flag_of(type->name, "Build_Tab_Land_Units");
            const bool space_tab = flag_of(type->name, "Build_Tab_Space_Units") || flag_of(type->name, "Build_Tab_Space_Station");
            if (has_row && at->second.direct) {
                const bool expect_ground = domain_ground;
                if (expect_ground != at->second.ground) {
                    verdict = "DEFECT-wrong-list"; ++defects;
                    detail = std::string("the build-domain gate selects the ") + (expect_ground ? "ground" : "space") + " list but it is offered on the "
                        + (at->second.ground ? "ground" : "space") + " list";
                } else {
                    if ((land_tab && !space_tab && !at->second.ground) || (space_tab && !land_tab && at->second.ground)) {
                        const auto note = label + " " + type->name + " tab-flag-differs: authored for the " + (land_tab ? "ground" : "space")
                            + " tab, the gate puts it on the " + (at->second.ground ? "ground" : "space") + " list (as the original does)";
                        if (reported.insert(note).second) { ++flag_differs; std::cout << "build-list " << note << "\n"; }
                    }
                    verdict = "offered"; ++offered_total;
                    if (must_seen.contains(type->name) && at->second.ground) must_seen[type->name] = true;
                }
            } else if ((has_row && at->second.cinematic) || flag_of(type->name, "Cinematic_Object_Only")) {
                verdict = "retail-refused"; detail = "cinematic only"; ++retail_refused;
            } else if (has_behavior(*type, "DUMMY_GROUND_BASE")) {
                verdict = "retail-refused"; detail = "ground base type (placed by its level, never bought)"; ++retail_refused;
            } else if (!cost_ok) {
                verdict = "retail-refused"; detail = "build cost below 1"; ++retail_refused;
            } else if (!constructible) {
                verdict = "retail-refused";
                detail = type->named_hero.value_or(false) ? "named hero authoring no base level" : "no required level and no structure/starbase role";
                ++retail_refused;
            } else if (!production_role) {
                verdict = "no-role"; detail = "no DUMMY_* production behaviour: not a build wrapper"; ++no_role;
            } else if (type->ground_company_member == true || type->squadron_member == true || has_behavior(*type, "TRANSPORT")) {
                verdict = "retail-refused"; detail = "company or squadron member, or a transport (WGEP-20)"; ++retail_refused;
            } else if (!type->required_planets.empty() && std::none_of(type->required_planets.begin(), type->required_planets.end(),
                           [&](const auto& planet) { return planet_names.contains(eawr::core::to_lower(planet)); })) {
                verdict = "restricted"; ++restricted;
            } else if (!has_row) {
                verdict = "DEFECT-dropped"; ++defects;
                detail = "constructible by the original's gate, affiliated with a campaign faction, no production row on any planet";
            } else {
                verdict = "DEFECT-hidden"; ++defects;
                detail = "constructible by the original's gate (ground level " + std::to_string(level_ground) + ", star level "
                    + std::to_string(level_star) + ") but direct building is refused on every planet";
            }
            const auto line = label + " " + type->name + " " + verdict + (detail.empty() ? "" : ": " + detail);
            if (verdict != "offered" && reported.insert(line).second) {
                std::cout << "build-list " << line << " (first seen in " << source.name << ")\n";
                expect(verdict.rfind("DEFECT", 0) != 0, line);
            }
        }
    }
    std::cout << "build-list " << label << " summary: campaigns=" << campaigns_run << " authored_types=" << authored.size()
        << " offered=" << offered_total << " retail_refused=" << retail_refused << " no_role=" << no_role << " restricted=" << restricted
        << " tab_flag_differs=" << flag_differs << " unregistered=" << unregistered << " defects=" << defects << '\n';
    expect(campaigns_run > 0, label + ": at least one campaign prepared");
    for (const auto& [name, ok] : must_seen) expect(ok, label + ": " + name + " is offered on the ground tab of a campaign");
    return 0;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string name_of(const std::filesystem::path& workshop, const std::string& id) {
    const auto text = read_file(workshop / id / "modinfo.json");
    static const std::regex name("\"name\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    return std::regex_search(text, match, name) ? match[1].str() : std::string("(no name)");
}

// Units-gap check, data and loader level only (no campaign): for every build-tab type of one content stack, the build-domain
// gate under the old rule (the authored levels as they are) and under the new one (after the WGEP-158 load fixup), side by side.
// Prints one gap-row per type the old rule did not offer: faction, needed building, list, level tag state, status, reason.
int gap_one(const std::string& label, const std::string& chain) {
    const auto mounts = mount_stack(chain);
    const auto files = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
    const auto loaded = take(eawr::data::load_catalog(*files, chain.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    const char* tabs[] = {"Build_Tab_Land_Units", "Build_Tab_Space_Units", "Build_Tab_Heroes", "Build_Tab_Special_Structures",
        "Build_Tab_Outpost", "Build_Tab_Space_Station"};
    const auto layers_of = [&](const std::string& name) {
        std::vector<const eawr::data::Definition*> layers;
        const auto resolved = loaded.catalog.resolve(name, eawr::data::Category::game_object);
        if (!resolved) return layers;
        for (const auto& link : resolved.value().chain)
            if (const auto* layer = loaded.catalog.find(link, eawr::data::Category::game_object)) layers.push_back(layer);
        return layers;
    };
    const auto has_behavior = [](const eawr::units::GalacticType& type, const char* role) {
        return std::any_of(type.behavior.begin(), type.behavior.end(), [&](const auto& value) { return eawr::core::iequals(value, role); });
    };
    std::size_t authored = 0, offered_before = 0, fixed = 0, still = 0, lost = 0, space_on_ground = 0, heroes_skipped = 0, placeholders_skipped = 0, absent_fixed = 0, explicit_zero_fixed = 0;
    for (const auto& type : types.types) {
        if (type.affiliation.empty()) continue;
        const auto layers = layers_of(type.name);
        bool in_tab = false;
        for (const auto* tab : tabs) if (const auto* node = eawr::data::retail::single(layers, tab)) {
            const auto flag = eawr::data::retail::boolean(node->raw_text);
            in_tab = in_tab || (flag && *flag);
        }
        if (!in_tab) continue;
        const auto cinematic = eawr::data::retail::single(layers, "Cinematic_Object_Only");
        if (cinematic) { const auto flag = eawr::data::retail::boolean(cinematic->raw_text); if (flag && *flag) continue; }
        const bool role = has_behavior(type, "DUMMY_GROUND_COMPANY") || has_behavior(type, "DUMMY_STARSHIP")
            || has_behavior(type, "DUMMY_SPACE_FIGHTER_SQUADRON") || has_behavior(type, "DUMMY_GROUND_STRUCTURE")
            || has_behavior(type, "DUMMY_ORBITAL_STRUCTURE") || has_behavior(type, "DUMMY_STAR_BASE");
        if (!role || has_behavior(type, "DUMMY_GROUND_BASE")) continue;
        // Only a type on a building's authored build list is a candidate: a hero (the loader's classification), a company or
        // squadron member, a transport placeholder and the Delete_/Upgrade_ entries are never bought, so never a gap.
        const auto lowered_name = eawr::core::to_lower(type.name);
        if (type.named_hero.value_or(false)) { ++heroes_skipped; continue; }
        if (has_behavior(type, "TRANSPORT") || type.ground_company_member == true || type.squadron_member == true
            || lowered_name.rfind("delete_", 0) == 0 || lowered_name.rfind("upgrade_", 0) == 0) { ++placeholders_skipped; continue; }
        ++authored;
        const auto level = [&](const char* tag, bool& present) {
            const auto* node = eawr::data::retail::single(layers, tag);
            present = node != nullptr;
            if (!node) return 0;
            try { return std::stoi(node->raw_text); } catch (...) { return 0; }
        };
        bool ground_present = false, star_present = false;
        const int authored_ground = level("Required_Ground_Base_Level", ground_present);
        const int authored_star = level("Required_Star_Base_Level", star_present);
        const bool cost_ok = type.build_cost_credits.value_or(eawr::sim::math::Fixed{}).raw() >= eawr::sim::math::Fixed::scale;
        const bool structure = has_behavior(type, "DUMMY_GROUND_STRUCTURE");
        const bool station = has_behavior(type, "DUMMY_STAR_BASE") && type.base_level.value_or(0) == 1;
        const bool old_offered = cost_ok && (authored_ground != 0 || structure || authored_star != 0 || station);
        const bool new_ground = eawr::units::built_on_ground(type);
        const bool new_offered = new_ground || (cost_ok && (type.required_star_base_level.value_or(0) != 0 || station));
        std::string status, reason;
        if (old_offered && new_offered) { status = "offered-both"; ++offered_before; }
        else if (!old_offered && new_offered) { status = "FIXED-by-level-0-fixup"; ++fixed; (ground_present ? explicit_zero_fixed : absent_fixed)++; }
        else if (!old_offered) {
            status = "not-offered-after"; ++still;
            reason = !cost_ok ? "no cost of at least 1" : type.named_hero.value_or(false) ? "named hero authoring no level (the original refuses it too)"
                : "no level and no structure/starbase role";
        } else { status = "DEFECT-lost"; ++lost; reason = "offered by the old rule, not by the new one"; }
        if (old_offered) continue;
        std::string factions;
        for (const auto& faction : type.affiliation) factions += (factions.empty() ? "" : "|") + faction;
        const char* kind = has_behavior(type, "DUMMY_STARSHIP") ? "ship" : has_behavior(type, "DUMMY_SPACE_FIGHTER_SQUADRON") ? "squadron"
            : has_behavior(type, "DUMMY_GROUND_COMPANY") ? "company" : has_behavior(type, "DUMMY_STAR_BASE") ? "starbase" : "structure";
        if (new_offered && new_ground && (std::string(kind) == "ship" || std::string(kind) == "squadron")) ++space_on_ground;
        const std::string tag_state = !ground_present ? "absent" : "explicit " + std::to_string(authored_ground);
        std::cout << "gap-row\t" << label << '\t' << type.name << '\t' << factions << '\t' << type.required_special_structures << '\t'
            << kind << '\t' << (new_ground ? "ground" : "space") << '\t' << "ground-level " << tag_state << '\t' << status << '\t' << reason << '\n';
    }
    std::cout << "gap-summary\t" << label << "\tauthored_build_types=" << authored << "\toffered_before_and_after=" << offered_before
        << "\tnot_offered_before_fixed_after=" << fixed << "\t(of which level tag absent=" << absent_fixed << ", explicit 0=" << explicit_zero_fixed
        << ")\theroes_skipped=" << heroes_skipped << "\tplaceholders_skipped=" << placeholders_skipped << "\tships_or_squadrons_on_ground_list=" << space_on_ground << "\tnot_offered_after=" << still << "\tlost_by_new_rule=" << lost << '\n';
    galactic_test::expect(lost == 0, label + ": the new rule never loses a type the old rule offered");
    return 0;
}

// The units-gap check over stock and the named stacks. A stack is workshop ids joined by '+', leaf first (the chain the game
// mounts, as in the coverage profiles); a lone id is a standalone mod. A stack not installed on this host is reported, not failed.
int gap(const std::string& stacks_csv) {
    const auto game_root = eawr::testing::game_root();
    if (!game_root) throw MissingMod("set EAWR_EAW_GAME_ROOT");
    const auto stacks = split_chain(stacks_csv, ',');
    std::cout << "gap-stack\tstock\n";
    int status = gap_one("stock", "");
    for (const auto& stack : stacks) {
        const auto ids = split_chain(stack, '+');
        if (ids.empty()) continue;
        std::string joined;
        for (const auto& id : ids) joined += (joined.empty() ? "" : ";") + id;
        std::string name = "(not installed)";
        for (const auto& workshop : workshop_roots(std::filesystem::path(*game_root))) {
            std::error_code unreadable;
            if (std::filesystem::is_directory(workshop / ids.front(), unreadable) && !unreadable) { name = name_of(workshop, ids.front()); break; }
        }
        std::cout << "gap-stack\t" << ids.front() << '\t' << name << "\tchain=" << joined << '\n';
        try {
            status += gap_one(ids.front(), joined);
        } catch (const MissingMod& missing) {
            std::cout << "gap-mod-missing\t" << joined << '\t' << missing.what() << '\n';
        } catch (const std::exception& error) {
            std::cout << "gap-skipped\t" << joined << "\tEXCEPTION " << error.what() << '\n';
        }
    }
    return status;
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    std::cout << std::unitbuf;
    try {
        if (mode == "stock") return check("stock", "", {}) + galactic_test::checker.finish();
        if (mode == "raw") return check("raw", "1129810972", {"BattleDroid_Squad", "Cis_Super_BattleDroid_Squad", "CIS_Militia_Platoon"}) + galactic_test::checker.finish();
        if (mode == "remake") return check("remake", "1770851727", {}) + galactic_test::checker.finish();
        if (mode == "bfc") return check("bfc", "2712013423", {}) + galactic_test::checker.finish();
        if (mode.rfind("mod:", 0) == 0) return check(std::string(mode), std::string(mode.substr(4)), {}) + galactic_test::checker.finish();
        if (mode.rfind("gap:", 0) == 0) return gap(std::string(mode.substr(4))) + galactic_test::checker.finish();
        std::cerr << "usage: galactic_build_list_check_tests stock|raw|remake|bfc|mod:<workshop id>|gap:<stack>[,<stack>] (a stack is ids joined by '+', leaf first)\n";
        return 2;
    } catch (const MissingMod& missing) {
        std::cout << "SKIPPED: " << missing.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "EXCEPTION " << error.what() << '\n';
        return 1;
    }
}

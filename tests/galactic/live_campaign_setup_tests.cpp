#include <exception>
#include <initializer_list>
#include "../support/env.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/tactical_return_live.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/campaign/galactic_ai_services.hpp"
#include "../../src/platform/galactic/save_repository.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include "eawr/sim/galactic/control.hpp"
#include "galactic/fleet_traits.hpp"
#include "galactic/planet_hud.hpp"
#include "galactic/ground_roster.hpp"
#include "eawr/presentation/ui/campaign_setup.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/ui/shell_anchors.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <regex>
#include "../support/temp_dir.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/campaign/planet_economy.hpp"
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
bool control_sources_match(const gc::GalacticSnapshot& snapshot) {
    if (!snapshot.control()) return false;
    auto projected = *snapshot.control(); projected.ability_sources.clear();
    for (const auto& actor : snapshot.actor_records()) gc::update_control_actor(projected, actor);
    return projected.ability_sources == snapshot.control()->ability_sources;
}
std::optional<std::string> game_root() {
    return eawr::testing::game_root();
}
void production_domains(const gc::GalacticSetup& setup, const cp::CampaignSetupPlan& plan) {
    std::set<std::string> orbital_companies;
    for (const auto& planet : setup.production->planets) for (const auto& offer : planet.offers) {
        const auto* type = plan.original_type(offer.type);
        if (!type) throw std::runtime_error("production domain requires its original type");
        const bool priced = type->build_cost_credits && type->build_cost_credits->raw() >= eawr::sim::math::Fixed::scale;
        const bool structure = std::find(type->behavior.begin(), type->behavior.end(), "DUMMY_GROUND_STRUCTURE") != type->behavior.end();
        const bool ground = priced && (type->required_ground_base_level.value_or(0) != 0 || structure);
        expect(offer.domain == (ground ? gc::ProductionDomain::ground : gc::ProductionDomain::space),
            "root build domain follows authored ground construction, independent of company representation");
        if (!ground && priced && std::find(type->behavior.begin(), type->behavior.end(), "DUMMY_GROUND_COMPANY") != type->behavior.end())
            orbital_companies.insert(type->name);
    }
    expect(orbital_companies.contains("Probe_Droid_Team") && orbital_companies.contains("Darth_Team_Executor"),
        "root offers both reported companies in the space production domain");
    for (const auto& name : orbital_companies) std::cout << "orbital company " << name << '\n';
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto created = gc::GalacticSession::create(setup);
        if (!created) throw std::runtime_error(eawr::core::format_diagnostic(created.error()));
        auto session = std::move(created).value();
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 3; ++tick) {
            auto step = session.step(executor);
            if (!step) throw std::runtime_error(eawr::core::format_diagnostic(step.error()));
            hashes.push_back(step.value().state_hash.get());
        }
        for (const auto& row : session.snapshot()->production_offers()) {
            const auto* type = plan.original_type(row.type);
            if (type && orbital_companies.contains(type->name))
                expect(row.domain == gc::ProductionDomain::space, "published company choice preserves the orbital domain");
        }
        if (workers == 1) baseline = hashes;
        else expect(hashes == baseline, "production domains remain deterministic on 1/2/4/8 workers");
    }
}
void structure_slot_counter(cp::CampaignRoot campaign, const cp::CampaignSetupPlan& plan,
    const cp::CampaignSetupOptions& options) {
    namespace hud = eawr::viewer::galactic;
    const auto local = *options.solo_reference_human;
    auto initial = campaign.game()->galaxy().snapshot();
    const auto offers = initial->production_offers();
    const auto mining = std::find_if(offers.begin(), offers.end(), [&](const auto& row) {
        const auto* type = plan.original_type(row.type);
        return row.player == local && row.domain == gc::ProductionDomain::ground
            && row.kind == gc::ProductionKind::structure && row.admission == gc::ProductionAdmission::allowed
            && type && type->name == "Empire_Ground_Mining_Facility";
    });
    if (mining == offers.end()) throw std::runtime_error("stock local mining offer missing");
    const auto planet = mining->planet;
    const auto type = mining->type;
    const auto facts = [&](const auto& snapshot) {
        const auto observations = snapshot->observations();
        const auto observation = std::find_if(observations.begin(), observations.end(), [&](const auto& row) {
            return row.player == local && row.planet == planet;
        });
        const auto& producers = snapshot->production()->planets;
        const auto producer = std::find_if(producers.begin(), producers.end(), [&](const auto& row) { return row.planet == planet; });
        if (observation == observations.end() || producer == producers.end())
            throw std::runtime_error("stock owned planet HUD inputs missing");
        return hud::observed_hud_facts(*observation, nullptr, &*producer, local, {});
    };
    const auto before = facts(initial);
    expect(before.structures_known && before.structures.has_value(), "stock local structure count is known");
    if (!before.structures) throw std::runtime_error("stock local structure knowledge missing");
    const auto initial_roster = hud::ground_roster(*initial, local, planet);
    expect(initial_roster.structures.size() == *before.structures,
        "initial filled structure icon slots match the HUD facts");
    const auto seat = std::find_if(options.seats.begin(), options.seats.end(), [&](const auto& row) { return row.player == local; });
    auto started = eawr::platform::LiveGameSession::start(std::move(campaign),
        {.workers = 2, .pacing = eawr::platform::LiveGameSession::Pacing::driven});
    if (!started) throw std::runtime_error(eawr::core::format_diagnostic(started.error()));
    auto& live = *started.value();
    live.submit(gc::GameCommand{{0, seat->seat_id, 1}, gc::ProductionCommand{planet, type,
        gc::ProductionAction::buy, gc::ProductionDomain::ground, 0}});
    std::shared_ptr<const gc::GalacticSnapshot> completed;
    for (std::uint64_t tick = 1; tick <= 10000; ++tick) {
        live.advance_to(tick);
        if (!live.wait_for(tick, std::chrono::seconds(30))) throw std::runtime_error("stock mining completion timed out");
        auto snapshot = live.frame().latest;
        const auto after = facts(snapshot);
        if (after.structures && before.structures && *after.structures == *before.structures + 1) {
            live.advance_to(tick + 1);
            if (!live.wait_for(tick + 1, std::chrono::seconds(30))) throw std::runtime_error("next mining snapshot timed out");
            completed = live.frame().latest; break;
        }
    }
    live.stop();
    expect(completed != nullptr && !live.failure() && live.rejected_orders().empty(), "stock mining completes through the live campaign publication");
    if (!completed) return;
    const auto after = facts(completed);
    expect(after.structures_known && after.structures == std::optional(*before.structures + 1)
        && after.ground_capacity == before.ground_capacity,
        "completion preserves structure knowledge and authored ground capacity");
    const auto roster = hud::ground_roster(*completed, local, planet);
    expect(roster.structures.size() == *after.structures
        && roster.structure_capacity == static_cast<std::size_t>(*after.ground_capacity),
        "focused roster and HUD count the same structures once");
    std::set<eawr::sim::EntityId> ids;
    std::size_t new_mines{};
    for (const auto& structure : roster.structures) {
        expect(ids.insert(structure.id).second, "filled structure slot identities are unique");
        if (structure.type == type && std::none_of(initial_roster.structures.begin(), initial_roster.structures.end(),
            [&](const auto& old) { return old.id == structure.id; })) ++new_mines;
    }
    expect(new_mines == 1 && roster.structures.size() == initial_roster.structures.size() + 1,
        "one completed ground structure fills exactly one additional icon slot");
    std::cout << "slot-counter before=" << *before.structures << " after=" << *after.structures
        << " capacity=" << *after.ground_capacity << " tick=" << completed->completed_tick() << '\n';
}
void installed(bool domains_only = false, bool slots_only = false, std::string_view protection_case = {}) {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: stock live campaign requires EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        if (!mount) throw std::runtime_error(eawr::core::format_diagnostic(mount.error()));
        mounts.push_back(std::move(mount).value().mount);
    }
    const auto filesystem_owner = std::make_shared<const eawr::vfs::Vfs>(eawr::vfs::Vfs::mount(mounts).value());
    const auto& filesystem = *filesystem_owner;
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    std::vector<std::pair<std::string, std::int64_t>> authored_tech_targets;
    for (const auto& type : types.types)
        if (type.build_advances_tech_level)
            authored_tech_targets.emplace_back(type.name, *type.build_advances_tech_level);
    std::sort(authored_tech_targets.begin(), authored_tech_targets.end());
    expect(authored_tech_targets == std::vector<std::pair<std::string, std::int64_t>>{
        {"DS_Durasteel", 5}, {"DS_Primary_Hyperdrive", 2}, {"DS_Shield_Gen", 3}, {"DS_Superlaser_Core", 4}},
        "every installed stock Build_Advances_Tech_Level value parses as its numeric target");
    bool installed_stealth_hero_fleet = false, installed_stealth_company = false, lifecycle_checked = false;
    unsigned droid_teams_seen{}, gate_droid_teams_seen{};
    unsigned lifecycle_steps{};
    const bool solo_sources = protection_case == "--solo-protection-sources";
    const bool all_mp = protection_case == "--all-mp-protection-sources";
    std::vector<std::string> campaigns{"Sandbox_Equal_Footing_Underworld", "Sandbox_Equal_Footing_Rebel", "Sandbox_Equal_Footing_Empire"};
    if (solo_sources) {
        campaigns.clear();
        // The active Underworld story still needs its orbital scanner placement
        // provider (#4478). Keep that refusal intact; this case covers authored Tarkin sources.
        for (const auto& source : data.campaigns) {
            if (source.name == "Full_Story_Campaign_Underworld") continue;
            if (std::any_of(source.starting_forces.begin(), source.starting_forces.end(),
                [](const auto& force) { return force.type == "Grand_Moff_Tarkin_Team"; })) campaigns.push_back(source.name);
        }
        expect(!campaigns.empty(), "installed campaigns include authored protection sources");
    }
    if (all_mp) {
        campaigns.clear();
        for (const auto& source : data.campaigns) if (source.is_multiplayer.value_or(false)) campaigns.push_back(source.name);
        expect(campaigns.size() == 6, "installed stock catalog contains all six multiplayer campaigns");
    }
    for (const auto& name : campaigns) {
        if (!protection_case.empty() && !solo_sources && !all_mp && name != "Sandbox_Equal_Footing_Underworld") continue;
        if ((domains_only || slots_only) && std::string_view(name) != "Sandbox_Equal_Footing_Empire") continue;
        const auto* source = data.campaign(name); expect(source != nullptr, "stock family exists"); if (!source) continue;
        cp::CampaignSetupOptions options; options.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            options.players.push_back({{id, id, 0}, faction.name, true});
            const bool local = faction.name == source->starting_active_player;
            if (local) options.solo_reference_human = id;
            if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
                local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
        }
        if (solo_sources && !options.solo_reference_human) {
            for (auto& seat : options.seats) {
                const auto* faction = data.faction(options.players[seat.player - 1].faction);
                if (!faction || !faction->is_playable.value_or(false)) continue;
                options.solo_reference_human = seat.player;
                seat.kind = gc::SeatKind::human; seat.human_side_id = seat.player;
                break;
            }
        }
        std::vector<std::string> originals;
        for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
        if (all_mp) {
            options.mode = cp::CampaignSetupMode::multiplayer;
            options.solo_reference_human.reset();
            unsigned humans{};
            for (auto& seat : options.seats) {
                const auto* faction = data.faction(options.players[seat.player - 1].faction);
                const bool human = faction && faction->is_playable.value_or(false) && humans < 2;
                if (human) ++humans;
                seat.kind = human ? gc::SeatKind::human : gc::SeatKind::ai;
                seat.human_side_id = human ? seat.player : 0;
            }
            expect(humans == 2, "stock MP campaign prepares two human seats");
            for (const bool heroes : {false, true}) {
                options.custom = cp::CampaignCustomOptions{12000, 1, 5, heroes};
                auto plan = cp::plan_live_campaign_setup(filesystem, data, types, name, options, "Normal_Default");
                if (!plan) throw std::runtime_error(name + ": " + eawr::core::format_diagnostic(plan.error()));
                auto services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, types, plan.value(), "Normal_Default");
                if (!services) throw std::runtime_error(name + ": " + eawr::core::format_diagnostic(services.error()));
                auto created = cp::CampaignRoot::create(plan.value(), services.value());
                if (!created) throw std::runtime_error(name + ": " + eawr::core::format_diagnostic(created.error()));
                const auto& setup = created.value().game()->galaxy().setup();
                expect(control_sources_match(*created.value().game()->galaxy().snapshot()),
                    "all stock MP automatic control sources match their live actor identities");
                unsigned sources{};
                for (const auto& force : plan.value().forces()) if (force.original_type == "Grand_Moff_Tarkin_Team") {
                    ++sources;
                    expect(setup.census && std::count_if(setup.census->objects.begin(), setup.census->objects.end(), [&](const auto& row) {
                        return row.original_type == force.type && row.owner == force.owner;
                    }) == 1, "authored Tarkin is placed once, also when MP hero planning references him");
                }
                expect(sources != 0, "each stock MP campaign exercises an authored protection source");
                expect(heroes || plan.value().multiplayer_heroes().empty(), "heroes-off suppresses only the generated hero pass");
                auto prepared = eawr::platform::PreparedLiveCampaign::from_plan(filesystem_owner, catalog.catalog,
                    data, types, plan.value(), "Normal_Default");
                if (!prepared) throw std::runtime_error(name + ": " + eawr::core::format_diagnostic(prepared.error()));
                expect(prepared.value()->native_ai_installed() && !prepared.value()->native_ai_players().empty(),
                    "stock MP installs faction-authored native AI after typed protection placement");
                std::cout << "MP-protection-inert campaign=" << name << " heroes=" << heroes
                    << " typed_root=passed placed=passed native_ai=installed\n";
            }
            continue;
        }
        if (!protection_case.empty()) {
            auto protection_data = data;
            const bool authored = protection_case == "--authored-protection-source";
            if (authored) {
                const auto selected = std::find_if(protection_data.campaigns.begin(), protection_data.campaigns.end(),
                    [&](const auto& row) { return row.name == source->name; });
                selected->starting_forces.push_back({"Empire", "Coruscant", "Grand_Moff_Tarkin_Team"});
            } else if (!solo_sources) {
                options.mode = cp::CampaignSetupMode::multiplayer;
                options.custom = cp::CampaignCustomOptions{12000, 1, 5, true};
            }
            auto plan = cp::plan_live_campaign_setup(filesystem, protection_data, types, name, options, "Normal_Default");
            if (!plan) throw std::runtime_error(name + ": " + eawr::core::format_diagnostic(plan.error()));
            auto services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, protection_data,
                types, plan.value(), "Normal_Default");
            if (!services) throw std::runtime_error(name + ": " + eawr::core::format_diagnostic(services.error()));
            std::optional<cp::CampaignHero> tarkin;
            if (authored || solo_sources) {
                expect(plan.value().mode() == cp::CampaignSetupMode::single_player && plan.value().multiplayer_heroes().empty(),
                    "authored protection source prepares without the multiplayer hero path");
                for (const auto& force : plan.value().forces()) if (force.original_type == "Grand_Moff_Tarkin_Team")
                    tarkin = cp::CampaignHero{force.owner, force.planet, force.type, force.original_type};
                expect(tarkin.has_value() || source->is_story_campaign.value_or(false), "authored protection test contains Tarkin or a stock story");
            } else {
                for (const auto& hero : plan.value().multiplayer_heroes()) if (hero.original_type == "Grand_Moff_Tarkin_Team") tarkin = hero;
                expect(tarkin.has_value(), "stock multiplayer plan includes Tarkin with heroes enabled");
            }
            auto created = cp::CampaignRoot::create(plan.value(), services.value());
            if (!created) throw std::runtime_error(name + ": " + eawr::core::format_diagnostic(created.error()));
            const auto& setup = created.value().game()->galaxy().setup();
            if (tarkin) {
                expect(setup.census && std::any_of(setup.census->objects.begin(), setup.census->objects.end(), [&](const auto& row) {
                    return row.original_type == tarkin->original_type_id && row.owner == tarkin->player && row.planet == tarkin->home;
                }), "Tarkin is placed at his authored or planned multiplayer location");
                const auto child = std::find_if(options.types.begin(), options.types.end(),
                    [](const auto& row) { return row.original_type == "Grand_Moff_Tarkin"; });
                expect(child != options.types.end() && setup.census && std::any_of(setup.census->objects.begin(), setup.census->objects.end(),
                    [&](const auto& row) { return row.original_type == child->id && row.owner == tarkin->player && row.planet == tarkin->home; }),
                    "Tarkin's nested actor is placed with its original identity");
                const auto snapshot = created.value().game()->galaxy().snapshot();
                bool has_protection{}, has_transition{};
                if (child != options.types.end()) for (const auto& row : snapshot->control()->ability_sources) {
                    if (row.type != child->id || row.owner != tarkin->player || row.planet != tarkin->home) continue;
                    const auto profile = std::find_if(snapshot->control()->ability_types.begin(), snapshot->control()->ability_types.end(),
                        [&](const auto& value) { return value.type == row.type; });
                    if (profile == snapshot->control()->ability_types.end() || row.ability >= profile->abilities.size()) continue;
                    const auto& ability = profile->abilities[row.ability];
                    has_protection |= ability.protection;
                    has_transition |= !ability.protection && ability.multiplier == eawr::sim::math::Fixed::from_ratio(1, 4).value();
                }
                expect(has_protection && has_transition, "Tarkin's nested actor supplies separate protection and direct-quarter-time sources");
            }
            expect(control_sources_match(*created.value().game()->galaxy().snapshot()),
                "every authored/story/MP control source follows the placed actor");
            if (authored || protection_case == "--mp-hero-protection-live") {
                const eawr::platform::ThreadWorkerAdapter executor(1);
                for (unsigned gate_tick = 0; gate_tick < 3; ++gate_tick) {
                    auto stepped = created.value().game()->step(executor);
                    if (!stepped) throw std::runtime_error(eawr::core::format_diagnostic(stepped.error()));
                    expect(control_sources_match(*created.value().game()->galaxy().snapshot()),
                        "automatic control source ledger remains bound after campaign ticks");
                }
            }
            if (protection_case == "--mp-heroes-tarkin" && tarkin) {
                for (const bool galactic_production : {false, true}) {
                    auto behavior_data = data;
                    for (const auto& home : plan.value().homes()) if (home.planet) {
                        auto planet = std::find_if(behavior_data.planets.begin(), behavior_data.planets.end(),
                            [&](const auto& row) { return row.name == home.original_type; });
                        if (planet == behavior_data.planets.end()) throw std::runtime_error("MP home authoring missing");
                        std::erase(planet->behaviors, "PRODUCTION"); std::erase(planet->galactic_behaviors, "PRODUCTION");
                        if (galactic_production) planet->galactic_behaviors.push_back("PRODUCTION");
                    }
                    auto behavior_plan = cp::plan_live_campaign_setup(filesystem, behavior_data, types, name, options, "Normal_Default");
                    if (!behavior_plan) throw std::runtime_error(eawr::core::format_diagnostic(behavior_plan.error()));
                    auto behavior_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, behavior_data,
                        types, behavior_plan.value(), "Normal_Default");
                    if (!behavior_services) throw std::runtime_error(eawr::core::format_diagnostic(behavior_services.error()));
                    auto behavior_root = cp::CampaignRoot::create(behavior_plan.value(), behavior_services.value());
                    if (!behavior_root) throw std::runtime_error(eawr::core::format_diagnostic(behavior_root.error()));
                    const auto& objects = behavior_root.value().game()->galaxy().setup().census->objects;
                    const bool placed = std::any_of(objects.begin(), objects.end(), [&](const auto& row) {
                        return row.original_type == tarkin->original_type_id && row.owner == tarkin->player;
                    });
                    expect(placed == galactic_production,
                        "MP heroes need authored production behavior, including the galactic-only behavior list");
                }
            }
            std::cout << "protection-live campaign=" << name << " mode=" << (solo_sources || authored ? "solo" : "MP")
                << " authored_tarkin=" << tarkin.has_value() << " prepared=passed placed=passed\n";
            if (solo_sources) continue;
            return;
        }
        auto planned = cp::plan_live_campaign_setup(filesystem, data, types, name, options, "Normal_Default");
        if (!planned) throw std::runtime_error(eawr::core::format_diagnostic(planned.error()));
        if (source->name == "Sandbox_Equal_Footing_Underworld") {
            auto omitted = data;
            for (auto& faction : omitted.factions) if (faction.name == source->starting_active_player)
                faction.benefits_from_corruption.reset();
            const auto omission_plan = cp::plan_live_campaign_setup(filesystem, omitted, types, name, options, "Normal_Default");
            if (!omission_plan) throw std::runtime_error(eawr::core::format_diagnostic(omission_plan.error()));
            const auto omission_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, omitted, types,
                omission_plan.value(), "Normal_Default");
            if (!omission_services) throw std::runtime_error(eawr::core::format_diagnostic(omission_services.error()));
            expect(std::count_if(omission_services.value().load_diagnostics.begin(), omission_services.value().load_diagnostics.end(),
                [&](const auto& note) {
                    return note.code == "campaign_omitted_faction_tag" && note.severity == eawr::core::Severity::warning
                        && note.message.find(source->starting_active_player + ": Benefits_From_Corruption") != std::string::npos
                        && note.message.find("false constructor default (WGS-94)") != std::string::npos;
                }) == 1, "WGS-94 omitted faction benefit emits one load note naming the default");
            auto omission_root = cp::CampaignRoot::create(omission_plan.value(), omission_services.value());
            if (!omission_root) throw std::runtime_error(eawr::core::format_diagnostic(omission_root.error()));
            const auto& movement_players = omission_root.value().game()->galaxy().setup().movement->players;
            expect(std::any_of(movement_players.begin(), movement_players.end(), [&](const auto& player) {
                return player.player == *options.solo_reference_human && !player.benefits_from_corruption;
            }), "WGS-94 the live campaign uses false for an omitted faction benefit");
            auto malformed = data;
            malformed.issues.push_back({eawr::data::galactic::IssueKind::bad_value, {}, 0,
                source->starting_active_player, "Benefits_From_Corruption", "unknown", "invalid Boolean"});
            expect(!cp::prepare_live_campaign_services(filesystem, catalog.catalog, malformed, types,
                planned.value(), "Normal_Default"), "malformed faction authoring cannot become a fresh default");
            auto nonplayable_options = options;
            const auto pirates = std::find_if(options.players.begin(), options.players.end(),
                [](const auto& player) { return player.faction == "Pirates"; });
            expect(pirates != options.players.end(), "stock non-playable Pirates binding exists");
            if (pirates != options.players.end()) {
                std::erase_if(nonplayable_options.seats, [&](const auto& seat) { return seat.player == pirates->player.player_id; });
                nonplayable_options.seats.push_back({gc::SeatId{99}, pirates->player.player_id, gc::SeatKind::human, 99, true, 99});
                auto rejected = eawr::platform::PreparedLiveCampaign::create(filesystem_owner, catalog.catalog,
                    data, types, name, nonplayable_options, "Normal_Default");
                expect(!rejected && rejected.error().message.find("playable faction") != std::string::npos,
                    "active human Pirates seat refuses explicitly before live startup");
            }
            auto protected_data = data;
            const auto selected = std::find_if(protected_data.campaigns.begin(), protected_data.campaigns.end(),
                [&](const auto& row) { return row.name == source->name; });
            selected->starting_forces.push_back({"Empire", source->planets.front(), "Grand_Moff_Tarkin_Team"});
            auto protected_plan = cp::plan_live_campaign_setup(filesystem, protected_data, types, name, options, "Normal_Default");
            if (!protected_plan) throw std::runtime_error(eawr::core::format_diagnostic(protected_plan.error()));
            auto protection = cp::prepare_live_campaign_services(filesystem, catalog.catalog, protected_data,
                types, protected_plan.value(), "Normal_Default");
            expect(static_cast<bool>(protection), "authored Tarkin source prepares with live political abilities");
            auto economic_data = data;
            const auto economic_campaign = std::find_if(economic_data.campaigns.begin(), economic_data.campaigns.end(),
                [&](const auto& row) { return row.name == source->name; });
            const auto rebel_ground = std::find_if(source->starting_forces.begin(), source->starting_forces.end(),
                [&](const auto& row) {
                    const auto* original = types.find(row.type);
                    return row.faction == "Rebel" && original && std::find(original->behavior.begin(), original->behavior.end(),
                        "DUMMY_GROUND_COMPANY") != original->behavior.end();
                });
            expect(rebel_ground != source->starting_forces.end(), "stock Rebel surface force provides a compatible test location");
            if (rebel_ground == source->starting_forces.end()) throw std::runtime_error("missing Rebel ground starting force");
            economic_campaign->starting_forces.push_back({"Rebel", rebel_ground->location, "Mon_Mothma_Team"});
            auto economic_plan = cp::plan_live_campaign_setup(filesystem, economic_data, types, name, options, "Normal_Default");
            if (!economic_plan) throw std::runtime_error(eawr::core::format_diagnostic(economic_plan.error()));
            auto economic_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, economic_data,
                types, economic_plan.value(), "Normal_Default");
            if (!economic_services) throw std::runtime_error(eawr::core::format_diagnostic(economic_services.error()));
            auto economic_root = cp::CampaignRoot::create(economic_plan.value(), economic_services.value());
            if (!economic_root) throw std::runtime_error(eawr::core::format_diagnostic(economic_root.error()));
            const auto mothma = std::find_if(options.types.begin(), options.types.end(),
                [](const auto& row) { return row.original_type == "Mon_Mothma"; });
            expect(mothma != options.types.end(), "stock Mon Mothma original binds");
            if (mothma == options.types.end()) throw std::runtime_error("stock Mon Mothma binding missing");
            const auto& nested_income = *economic_root.value().game()->galaxy().setup().income;
            expect(std::any_of(nested_income.sources.begin(), nested_income.sources.end(),
                [&](const auto& row) { return row.type == mothma->id; }),
                "nested Mon Mothma economic actor binds without refusing campaign startup");

            // Stock data supplies both discounts; only the test's starting roster changes.
            auto price_data = data;
            const auto price_campaign = std::find_if(price_data.campaigns.begin(), price_data.campaigns.end(),
                [&](const auto& row) { return row.name == source->name; });
            price_campaign->starting_forces.push_back({"Empire", "Coruscant", "Emperor_Palpatine_Team"});
            auto price_options = options;
            const auto empire = std::find_if(options.players.begin(), options.players.end(),
                [](const auto& row) { return row.faction == "Empire"; });
            if (empire == options.players.end()) throw std::runtime_error("stock Empire player missing");
            price_options.solo_reference_human = empire->player.player_id;
            for (auto& seat : price_options.seats) {
                seat.kind = seat.player == *price_options.solo_reference_human ? gc::SeatKind::human : gc::SeatKind::ai;
                seat.human_side_id = seat.kind == gc::SeatKind::human ? seat.player : 0;
            }
            auto price_plan = cp::plan_live_campaign_setup(filesystem, price_data, types, name, price_options, "Normal_Default");
            if (!price_plan) throw std::runtime_error(eawr::core::format_diagnostic(price_plan.error()));
            auto price_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, price_data,
                types, price_plan.value(), "Normal_Default");
            if (!price_services) throw std::runtime_error(eawr::core::format_diagnostic(price_services.error()));
            auto price_root = cp::CampaignRoot::create(price_plan.value(), price_services.value());
            if (!price_root) throw std::runtime_error(eawr::core::format_diagnostic(price_root.error()));
            const auto coruscant = std::find_if(price_plan.value().locations().begin(), price_plan.value().locations().end(),
                [](const auto& row) { return row.original_type == "Coruscant"; });
            const auto tartan = std::find_if(options.types.begin(), options.types.end(),
                [](const auto& row) { return row.original_type == "Tartan_Patrol_Cruiser"; });
            if (coruscant == price_plan.value().locations().end() || !coruscant->planet || tartan == options.types.end())
                throw std::runtime_error("stock Coruscant/Tartan binding missing");
            auto price_session = gc::GalacticSession::create(price_root.value().game()->galaxy().setup());
            if (!price_session) throw std::runtime_error(eawr::core::format_diagnostic(price_session.error()));
            auto price_galaxy = std::move(price_session).value();
            const auto price = [&] {
                const auto snapshot = price_galaxy.snapshot();
                const auto rows = snapshot->production_offers();
                const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& value) {
                    return value.planet == coruscant->planet->id && value.type == tartan->id;
                });
                if (row == rows.end() || !row->price) throw std::runtime_error("stock Tartan price unavailable");
                return row->price->trunc_to_integer();
            };
            expect(price() == 688, "Coruscant Tartan price includes Sullust and the nested Emperor: 688");
            const auto emperor_type = std::find_if(options.types.begin(), options.types.end(),
                [](const auto& row) { return row.original_type == "Emperor_Palpatine"; });
            if (emperor_type == options.types.end()) throw std::runtime_error("stock Emperor type missing");
            const auto actors = price_root.value().game()->galaxy().snapshot()->actor_records();
            const auto emperor = std::find_if(actors.begin(), actors.end(),
                [&](const auto& actor) { return actor.original_type == emperor_type->id; });
            if (emperor == actors.end()) throw std::runtime_error("nested Emperor actor missing");
            const std::vector<eawr::sim::EntityId> losses{emperor->id};
            auto removed = price_galaxy.apply_battle_losses(losses);
            if (!removed) throw std::runtime_error(eawr::core::format_diagnostic(removed.error()));
            expect(price() == 1000, "without the Emperor Coruscant keeps only Sullust's discount: 1000");
        }
        auto services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, types, planned.value(), "Normal_Default");
        if (!services) throw std::runtime_error(eawr::core::format_diagnostic(services.error()));
        gc::GalacticSetup missing_movement;
        missing_movement.census.emplace(); missing_movement.production.emplace();
        auto refused_income = services.value().income(planned.value(), missing_movement);
        expect(!refused_income && refused_income.error().message.find("movement contents") != std::string::npos,
            "income binding requires movement inputs rather than silently ignoring nested sources");
        auto created = cp::CampaignRoot::create(planned.value(), services.value());
        if (!created) throw std::runtime_error(eawr::core::format_diagnostic(created.error()));
        auto campaign = std::move(created).value(); const auto& setup = campaign.game()->galaxy().setup();
        if (name == "Sandbox_Equal_Footing_Empire") {
            const auto probe = std::find_if(options.types.begin(), options.types.end(),
                [](const auto& row) { return row.original_type == "Generic_Probe_Droid"; });
            expect(probe != options.types.end() && setup.ability_host.has_value(),
                "installed XML probe reaches the live ability-host provider");
            if (probe == options.types.end() || !setup.ability_host)
                throw std::runtime_error("installed probe ability host missing");
            const auto& host = *setup.ability_host;
            const auto profile = std::find_if(host.profiles.begin(), host.profiles.end(),
                [&](const auto& row) { return row.type == probe->id; });
            expect(profile != host.profiles.end() && profile->spy.has_value(),
                "unit System_Spy_Ability survives XML economic loading into the live profile");
            if (profile == host.profiles.end() || !profile->spy)
                throw std::runtime_error("installed probe spy profile missing");
            expect(profile->spy->name == "PROBE_DROID_SYSTEM_SPY" && profile->spy->initially_enabled
                && profile->spy->causes_despawn && profile->spy->duration == eawr::sim::math::Fixed::from_integer(60).value()
                && std::find(profile->ground_activated.begin(), profile->ground_activated.end(), gc::AbilityKind::system_spy)
                    != profile->ground_activated.end()
                && std::find(host.activated_spy_abilities.begin(), host.activated_spy_abilities.end(), profile->spy->name)
                    != host.activated_spy_abilities.end(),
                "installed probe has its enabled consuming 60-second Ground_Activated spy and authored route");
        }
        if (name == "Sandbox_Equal_Footing_Underworld") {
            // CGS-21: a synthetic costless original has no force, roster or
            // purchase route, but a planet's category time effect still selects it.
            const auto economics = eawr::units::galactic_economic_table(catalog.catalog);
            std::optional<gc::TypeId> source_id;
            std::vector<std::string> categories;
            for (const auto& profile : economics.types) {
                if (profile.object_class != "Planet") continue;
                const auto location = std::find_if(planned.value().locations().begin(), planned.value().locations().end(),
                    [&](const auto& row) { return row.original_type == profile.name && row.planet.has_value(); });
                if (location == planned.value().locations().end()) continue;
                for (const auto& ability : profile.reductions) {
                    if (ability.price || ability.included_categories.empty() || !ability.excluded_categories.empty()) continue;
                    categories = ability.included_categories;
                    const auto source_actor = std::find_if(setup.census->objects.begin(), setup.census->objects.end(),
                        [&](const auto& row) { return row.id == location->planet->id; });
                    if (source_actor != setup.census->objects.end()) source_id = source_actor->original_type;
                    break;
                }
                if (source_id) break;
            }
            expect(source_id.has_value(), "installed campaign supplies a category time-reduction planet");
            if (!source_id) throw std::runtime_error("category time-reduction source missing");
            auto modded = types; auto modded_options = options;
            eawr::units::GalacticType extra; extra.name = "Synthetic_Costless_Target";
            extra.object_class = "SpaceUnit"; extra.category_mask = categories;
            const auto extra_id = modded_options.types.back().id + 1;
            modded.types.push_back(extra); modded_options.types.push_back({extra_id, extra.name});
            const auto modded_plan = cp::plan_live_campaign_setup(filesystem, data, modded, name, modded_options, "Normal_Default").value();
            const auto modded_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, modded,
                modded_plan, "Normal_Default").value();
            const auto modded_root = cp::CampaignRoot::create(modded_plan, modded_services).value();
            const auto& modded_setup = modded_root.game()->galaxy().setup();
            expect(std::any_of(modded_setup.income->types.begin(), modded_setup.income->types.end(),
                [&](const auto& profile) { return std::any_of(profile.reductions.begin(), profile.reductions.end(),
                    [&](const auto& row) { return row.type == extra_id && !row.price; }); }),
                "CGS-21 live selection keeps a costless original outside the creation table");
            expect(std::none_of(modded_setup.census->objects.begin(), modded_setup.census->objects.end(),
                [&](const auto& row) { return row.original_type == extra_id; }), "economic target selection creates no object");
        }
        // Story-only leaders (no starting object) still carry their authored price reductions: the plot places them later.
        for (const auto* leader : {"Emperor_Palpatine", "Mon_Mothma"}) {
            const auto original = std::find_if(options.types.begin(), options.types.end(),
                [&](const auto& row) { return row.original_type == leader; });
            expect(original != options.types.end(), std::string(leader) + " binds in the stock campaign");
            if (original == options.types.end()) continue;
            const auto profile = std::find_if(setup.income->types.begin(), setup.income->types.end(),
                [&](const auto& row) { return row.type == original->id; });
            expect(profile != setup.income->types.end() && !profile->reductions.empty(),
                std::string(leader) + " keeps its price reductions although no stock start object holds it");
        }
        if (slots_only) {
            const auto ui = eawr::data::ui::load_command_bar(filesystem).value();
            for (const auto shell_name : {"i_main_commandbar", "i_main_organize"}) {
                const auto* component = ui.catalog.find(shell_name);
                if (!component) throw std::runtime_error("stock galactic shell missing");
                const auto anchors = eawr::data::ui::load_shell_anchors(filesystem,
                    component->text(eawr::data::ui::Field::model_name)).value();
                expect(!anchors.shell.find("Text_Planetary_Pop"), "stock galactic shells do not author a numeric structure counter");
                if (std::string_view(shell_name) == "i_main_organize")
                    expect(anchors.shell.find("Planet_structure_00") && anchors.shell.find("Planet_structure_08"),
                        "stock focused shell authors the structure icon slots");
                else expect(anchors.shell.find("Text_Day_Counter"), "the numeric fiscal cycle belongs to the galactic command bar");
            }
            structure_slot_counter(std::move(campaign), planned.value(), options); return;
        }
        const auto* bothawui = data.planet("Bothawui");
        expect(bothawui && bothawui->system_spies.size() == 1
            && bothawui->system_spies[0].mask == 61438U
            && bothawui->system_spies[0].influence_range == eawr::sim::math::Fixed::from_raw(150 * eawr::sim::math::Fixed::scale),
            "installed Bothawui authored spy flags and range load");
        const auto network = std::find_if(setup.observer->planet_spies.begin(), setup.observer->planet_spies.end(),
            [](const auto& row) { return row.text_identity == "Bothawui"; });
        expect(network != setup.observer->planet_spies.end(), "installed live campaign registers Bothawui network without a runtime name special case");
        if (network != setup.observer->planet_spies.end()) {
            const auto origin = std::find_if(setup.planets.begin(), setup.planets.end(), [&](const auto& row) { return row.id == network->planet; });
            std::size_t targets{};
            for (const auto& record : setup.observer->records) for (const auto& effect : record.sources)
                if (effect.source == network->planet && effect.ability == network->ability) {
                    expect(record.player == origin->owner && effect.mask == 61438U
                        && effect.final_frame == std::numeric_limits<std::uint64_t>::max(), "installed automatic network applies authored permanent intelligence for its live owner");
                    ++targets;
                }
            expect(targets > 1, "installed Bothawui network influences nearby campaign planets");
        }
        expect(setup.control && setup.production && setup.census && setup.income && setup.observer, "stock root installs complete services");
        if (domains_only) { production_domains(setup, planned.value()); return; }
        // WGEP-158..162: positive cost never admits alone. The oracle below recomputes the authored
        // predicates from the type table, independently of the setup path.
        const auto has_behavior = [](const eawr::units::GalacticType& type, const char* behavior) {
            return std::find(type.behavior.begin(), type.behavior.end(), behavior) != type.behavior.end();
        };
        const auto authored_gate = [&](const eawr::units::GalacticType& type) {
            const bool cost = type.build_cost_credits && type.build_cost_credits->raw() >= eawr::sim::math::Fixed::scale;
            return cost && (type.required_ground_base_level.value_or(0) != 0 || has_behavior(type, "DUMMY_GROUND_STRUCTURE")
                || type.required_star_base_level.value_or(0) != 0
                || (has_behavior(type, "DUMMY_STAR_BASE") && type.base_level.value_or(0) == 1));
        };
        std::set<std::string> member_names;
        for (const auto& type : types.types) {
            member_names.insert(type.company_units.begin(), type.company_units.end());
            member_names.insert(type.squadron_units.begin(), type.squadron_units.end());
        }
        // The shape the other content gates would admit for direct production, so only the new gate decides.
        const auto direct_shape = [&](const eawr::units::GalacticType& type) {
            const bool role = has_behavior(type, "DUMMY_STAR_BASE") || has_behavior(type, "DUMMY_GROUND_STRUCTURE")
                || has_behavior(type, "DUMMY_ORBITAL_STRUCTURE") || has_behavior(type, "DUMMY_STARSHIP")
                || has_behavior(type, "DUMMY_GROUND_COMPANY") || has_behavior(type, "DUMMY_SPACE_FIGHTER_SQUADRON")
                || type.build_advances_tech_level.value_or(0) > 0;
            return role && !has_behavior(type, "DUMMY_GROUND_BASE") && !has_behavior(type, "TRANSPORT")
                && type.required_planets.empty() && !member_names.contains(type.name);
        };
        const auto offer_of = [&](const gc::GalacticSetup& built, gc::TypeId id) -> const gc::ProductionOffer* {
            for (const auto& location : built.production->planets)
                for (const auto& row : location.offers) if (row.type == id) return &row;
            return nullptr;
        };
        std::set<std::string> refused_stock;
        std::size_t kept_stock = 0, orbital_offers = 0;
        for (const auto& location : setup.production->planets) for (const auto& row : location.offers) {
            const auto* original = planned.value().original_type(row.type);
            if (!original) continue;
            // RGC-01/02: the orbital token selects structure admission; authored gates still apply.
            if (has_behavior(*original, "DUMMY_ORBITAL_STRUCTURE")) {
                ++orbital_offers;
                expect(row.kind == gc::ProductionKind::structure && row.domain == gc::ProductionDomain::space,
                    "orbital production offers use space-special structure slot accounting");
            }
            const bool gate = authored_gate(*original);
            if (!gate) expect(!row.direct_build_allowed, "a type without level or role is never directly buildable");
            // Unpriced types are refused by the price gate regardless; only priced types are attributable here.
            const bool priced = original->build_cost_credits && original->build_cost_credits->raw() >= eawr::sim::math::Fixed::scale;
            if (!priced || !direct_shape(*original)) continue;
            expect(row.direct_build_allowed == gate, "directly buildable shape follows the authored construction predicates");
            if (row.direct_build_allowed) ++kept_stock; else refused_stock.insert(original->name);
        }
        expect(orbital_offers > 0, "stock production regression exercises authored orbital offers");
        // Stock heroes and their dummy variants author a cost but no level and no role.
        // Fin_Home_One is the cinematic-only variant of Home_One; the cinematic gate already refused it.
        expect(refused_stock == std::set<std::string>{"Fin_Home_One", "Home_One", "Millennium_Falcon_Ground",
            "Millennium_Falcon_Space", "Red_Squadron", "Sundered_Heart", "Sundered_Heart_Cinematic_Clone"},
            "stock construction gate refuses only the zero-level hero ships and their variants");
        expect(kept_stock > 0, "stock construction gate keeps ordinary ships, companies, structures and starbases");
        std::cout << "construction gate campaign=" << name << " refused=" << refused_stock.size()
            << " kept_direct_offers=" << kept_stock << '\n';
        for (const auto& refused : refused_stock) std::cout << "  refused " << refused << '\n';
        const auto ordinary = std::find_if(types.types.begin(), types.types.end(), [&](const auto& type) {
            return has_behavior(type, "DUMMY_STARSHIP") && direct_shape(type)
                && !type.named_hero.value_or(false) && type.required_star_base_level.value_or(0) != 0
                && type.build_cost_credits && type.build_cost_credits->raw() >= eawr::sim::math::Fixed::scale
                && std::find(type.affiliation.begin(), type.affiliation.end(), source->starting_active_player) != type.affiliation.end();
        });
        const auto hero_binding = std::find_if(options.types.begin(), options.types.end(),
            [](const auto& row) { return row.original_type == "Home_One"; });
        const auto ordinary_binding = ordinary == types.types.end() ? options.types.end() : std::find_if(options.types.begin(), options.types.end(),
            [&](const auto& row) { return row.original_type == ordinary->name; });
        const auto published_admission = [&](const gc::GalacticSnapshot& snapshot, gc::TypeId id) {
            std::optional<gc::ProductionAdmission> result;
            for (const auto& row : snapshot.production_offers()) {
                if (row.type != id || row.player != *options.solo_reference_human) continue;
                if (!result || *result == gc::ProductionAdmission::unavailable) result = row.admission;
            }
            return result;
        };
        if (source->name == "Sandbox_Equal_Footing_Rebel") {
            expect(hero_binding != options.types.end() && ordinary_binding != options.types.end(),
                "stock Rebel hero ship and ordinary ship bind for the mod-shaped gate controls");
            if (hero_binding == options.types.end() || ordinary_binding == options.types.end())
                throw std::runtime_error("stock gate controls unbound");
            const auto stock_hero = published_admission(*campaign.game()->galaxy().snapshot(), hero_binding->id);
            expect(stock_hero && *stock_hero == gc::ProductionAdmission::unavailable,
                "stock hero ship is unavailable in the published production list");
            const auto stock_ordinary = published_admission(*campaign.game()->galaxy().snapshot(), ordinary_binding->id);
            expect(stock_ordinary && *stock_ordinary != gc::ProductionAdmission::unavailable,
                "stock ordinary ship passes the content gates in the published production list");
            // Mod-shaped authoring: a hero gains a starbase level (buildable), while an ordinary ship keeps its
            // positive cost but loses both levels (refused). No name list is involved in either direction.
            auto modded = types;
            for (auto& type : modded.types) {
                if (type.name == "Home_One") type.required_star_base_level = 1;
                if (type.name == ordinary->name) { type.required_star_base_level = 0; type.required_ground_base_level = 0; }
            }
            auto modded_plan = cp::plan_live_campaign_setup(filesystem, data, modded, name, options, "Normal_Default");
            if (!modded_plan) throw std::runtime_error(eawr::core::format_diagnostic(modded_plan.error()));
            auto modded_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, modded,
                modded_plan.value(), "Normal_Default");
            if (!modded_services) throw std::runtime_error(eawr::core::format_diagnostic(modded_services.error()));
            auto modded_root = cp::CampaignRoot::create(modded_plan.value(), modded_services.value());
            if (!modded_root) throw std::runtime_error(eawr::core::format_diagnostic(modded_root.error()));
            auto modded_campaign = std::move(modded_root).value();
            const auto& modded_setup = modded_campaign.game()->galaxy().setup();
            const auto* modded_hero = offer_of(modded_setup, hero_binding->id);
            const auto* modded_ordinary = offer_of(modded_setup, ordinary_binding->id);
            expect(modded_hero && modded_hero->direct_build_allowed && modded_hero->price.raw() > 0,
                "mod-authored starbase level makes a named hero directly buildable");
            expect(modded_ordinary && !modded_ordinary->direct_build_allowed && modded_ordinary->price.raw() > 0,
                "positive cost with zero levels and no role stays refused");
            const auto modded_snapshot = modded_campaign.game()->galaxy().snapshot();
            const auto hero_row = published_admission(*modded_snapshot, hero_binding->id);
            expect(hero_row && *hero_row != gc::ProductionAdmission::unavailable,
                "mod-authored hero reaches the later production gates in the published list");
            const auto ordinary_row = published_admission(*modded_snapshot, ordinary_binding->id);
            expect(ordinary_row && *ordinary_row == gc::ProductionAdmission::unavailable,
                "zero-level positive-cost ship is unavailable in the published production list");
        }
        expect(!setup.fleets.empty() && !setup.movement->contents.empty(), "stock nested forces retain live identities");
        // These authored Equal Footing rosters have no named stealth hero.
        // Exercise the actual installed hero constructor, including its real
        // transport/payload and automatic ability initialization, before save.
        // Keep the original root at tick zero for the startup ownership transfer.
        auto hero_data = data;
        std::optional<cp::CampaignRoot> hero_campaign;
        eawr::checkpoint::GameLoadOptions load_options;
        eawr::sim::EntityId hero_birth_frontier{};
        if (source->name == "Sandbox_Equal_Footing_Underworld") {
            const auto hero = std::find_if(options.types.begin(), options.types.end(),
                [](const auto& row) { return row.original_type == "Boba_Fett_Team"; });
            const auto owner = std::find_if(options.players.begin(), options.players.end(),
                [](const auto& row) { return row.faction == "Empire"; });
            expect(hero != options.types.end() && owner != options.players.end(), "installed Boba Fett constructor and faction bind");
            if (hero == options.types.end() || owner == options.players.end()) throw std::runtime_error("stock stealth hero fixture unbound");
            const auto planet = std::find_if(setup.planets.begin(), setup.planets.end(),
                [&](const auto& row) { return row.owner == owner->player.player_id; });
            if (planet == setup.planets.end()) throw std::runtime_error("stock stealth hero has no owned construction site");
            const auto location = std::find_if(planned.value().locations().begin(), planned.value().locations().end(),
                [&](const auto& row) { return row.planet && row.planet->id == planet->id; });
            if (location == planned.value().locations().end()) throw std::runtime_error("stock stealth hero site has no authored location");
            // Reachability comes from the authored roster, through the same
            // stock plan/services path as production; catalog-wide IDs alone
            // cannot qualify an unbuildable hero or its nested dependencies.
            const auto hero_source = std::find_if(hero_data.campaigns.begin(), hero_data.campaigns.end(),
                [&](const auto& row) { return row.name == source->name; });
            if (hero_source == hero_data.campaigns.end()) throw std::runtime_error("stock stealth hero campaign is absent");
            hero_source->starting_forces.push_back({owner->faction, location->original_type, hero->original_type});
            // Equal Footing does not place the droid team at tick zero. Qualify
            // its real transport/payload through the same authored roster path.
            const auto droid_site = std::find_if(source->starting_forces.begin(), source->starting_forces.end(),
                [](const auto& force) { return force.faction == "Rebel"; });
            expect(droid_site != source->starting_forces.end(), "stock droid fixture has an authored Rebel site");
            if (droid_site == source->starting_forces.end()) throw std::runtime_error("stock droid fixture site missing");
            hero_source->starting_forces.push_back({droid_site->faction, droid_site->location, "Droids_Team"});
            auto hero_plan = cp::plan_live_campaign_setup(filesystem, hero_data, types, name, options, "Normal_Default");
            if (!hero_plan) throw std::runtime_error(eawr::core::format_diagnostic(hero_plan.error()));
            auto hero_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, hero_data,
                types, hero_plan.value(), "Normal_Default");
            if (!hero_services) throw std::runtime_error(eawr::core::format_diagnostic(hero_services.error()));
            expect(std::any_of(hero_services.value().stock_starting_forces->bindings.begin(),
                hero_services.value().stock_starting_forces->bindings.end(),
                [&](const auto& row) { return row.id == hero->id && row.original_type == hero->original_type; }),
                "authored hero fixture qualifies its original through the stock constructor binding graph");
            auto hero_created = cp::CampaignRoot::create(std::move(hero_plan).value(), hero_services.value());
            if (!hero_created) throw std::runtime_error(eawr::core::format_diagnostic(hero_created.error()));
            hero_campaign.emplace(std::move(hero_created).value());
            auto& hero_game = *hero_campaign->game();
            // Named failure seam: retain the stock constructor tree but remove
            // its qualified combat dispatch, so the live aggregate must refuse.
            for (const auto workers : eawr::platform::determinism_worker_counts()) {
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                auto unqualified_setup = hero_game.galaxy().setup();
                for (auto& type : unqualified_setup.production->nested_creation->types)
                    type.combat_role = gc::ActorCombatRole::unavailable;
                auto unqualified = gc::GalacticSession::create(unqualified_setup);
                if (!unqualified) throw std::runtime_error(eawr::core::format_diagnostic(unqualified.error()));
                const gc::GalacticSession::NativeProductionRequest request{owner->player.player_id, planet->id, hero->id, {}};
                const auto stock = cp::stock_native_force_builder(hero_services.value().stock_starting_forces, request);
                const auto rejected = unqualified.value().place_due_hero(request,
                    [stock](const auto& snapshot, auto frontier) {
                        auto placement = stock(snapshot, frontier);
                        if (placement) for (auto& actor : placement.value().originals)
                            actor.combat_role = gc::ActorCombatRole::unavailable;
                        return placement;
                    }, executor);
                expect(!rejected && rejected.error().code == gc::diagnostic_codes::invalid_setup,
                    "unqualified constructed fleet refuses the actual predicate instead of storing original-type defaults");
                expect(!unqualified.value().step(executor), "failed constructor cannot resume its partial mutation");
            }
            hero_birth_frontier = hero_game.galaxy().next_entity_id();
            cp::TacticalReturnState due;
            due.heroes.push_back({hero_birth_frontier, hero->id, 0,
                owner->player.player_id, owner->player.player_id, planet->id, 1});
            // This synthetic deadline is fixture setup, not a recorded command.
            // Reinstall the same initial record and real placement adapter before
            // checkpoint resimulation so it reconstructs the same actor birth.
            load_options.configure_session = [inputs = hero_services.value().stock_starting_forces,
                due_bytes = cp::encode_tactical_return_state(due)](cp::GameSession& game) {
                auto bound = cp::bind_stock_hero_returns(game, inputs);
                if (!bound) return bound;
                return game.restore_return_state(due_bytes);
            };
            auto scheduled = load_options.configure_session(hero_game);
            if (!scheduled) throw std::runtime_error(eawr::core::format_diagnostic(scheduled.error()));
            const eawr::platform::ThreadWorkerAdapter executor(1);
            const auto stepped = hero_game.step(executor);
            if (!stepped) throw std::runtime_error(eawr::core::format_diagnostic(stepped.error()));
            // GFS-10/GFC-25: the first scheduled-creation pass reads frame zero;
            // GFD-04 advances the timer only after the frame's services finish.
            expect(hero_game.return_state().heroes.size() == 1 && hero_game.galaxy().clock().galactic_frame == 1,
                "installed deadline-one hero remains pending after the frame-zero pass");
            const auto due_step = hero_game.step(executor);
            if (!due_step) throw std::runtime_error(eawr::core::format_diagnostic(due_step.error()));
            expect(hero_game.return_state().heroes.empty(), "installed stock constructor completes the explicit due hero fixture");
        }
        const auto& checkpoint_game = hero_campaign ? *hero_campaign->game() : *campaign.game();
        const auto born = checkpoint_game.galaxy().snapshot();
        bool stock_stealth_hero_fleet = false;
        std::vector<gc::FleetId> stealth_hero_fleets;
        for (const auto& fleet : born->fleets()) {
            const auto skip = born->fleet_combat_skip(fleet.id);
            if (!skip) throw std::runtime_error("installed fleet " + std::to_string(fleet.id) + ": "
                + eawr::core::format_diagnostic(skip.error()));
            if (hero_campaign && fleet.id >= hero_birth_frontier) {
                const auto& blockers = *born->production()->fleet_blockers;
                const auto blocker = std::find_if(blockers.begin(), blockers.end(),
                    [&](const auto& row) { return row.fleet == fleet.id; });
                expect(blocker != blockers.end() && blocker->skips_space_combat == skip.value(),
                    "due hero constructor blocker matches its actual recursive combat-skip predicate before saving");
            }
            if (!skip || !skip.value()) continue;
            for (const auto& actor : born->actor_records()) {
                // The roster hero qualifies the graph; only the later due birth
                // may satisfy this constructor/aggregate-stealth proof.
                if (actor.id < hero_birth_frontier) continue;
                const auto binding = std::find_if(options.types.begin(), options.types.end(),
                    [&](const auto& row) { return row.id == actor.current_type; });
                const auto* authored = binding == options.types.end() ? nullptr : types.find(binding->original_type);
                auto parent = actor.container;
                for (unsigned depth = 0; parent && parent != fleet.id && depth < 32; ++depth) {
                    const auto records = born->actor_records();
                    const auto at = std::find_if(records.begin(), records.end(), [&](const auto& row) { return row.id == parent; });
                    parent = at == records.end() ? 0 : at->container;
                }
                stock_stealth_hero_fleet |= parent == fleet.id && authored && authored->named_hero.value_or(false)
                    && actor.abilities.combat_skip;
                if (parent == fleet.id && authored && authored->named_hero.value_or(false) && actor.abilities.combat_skip)
                    stealth_hero_fleets.push_back(fleet.id);
            }
        }
        installed_stealth_hero_fleet |= stock_stealth_hero_fleet;
        std::cout << "live hero fleet campaign=" << name << " stealthed=" << stock_stealth_hero_fleet << '\n';
        for (const auto& actor : born->actor_records()) {
            const auto garrison = born->actor_garrison(actor.id);
            expect(garrison && !garrison.value(), "installed ordinary GC construction retains sourced false garrison bit");
        }
        expect(setup.production->technology_types.size() == planned.value().type_bindings().size(),
            "WGEP-147 live technology registry includes every registered binding beyond the production graph");
        for (const auto& row : setup.production->technology_types) {
            const auto* registered_type = planned.value().original_type(row.type);
            expect(registered_type && row.tech == registered_type->tech_level.value_or(0), "registry retains authored internal technology");
        }
        std::size_t companies{}, squadrons{};
        for (const auto& row : born->actor_records()) {
            const auto binding = std::find_if(options.types.begin(), options.types.end(),
                [&](const auto& value) { return value.id == row.original_type; });
            if (binding == options.types.end()) continue;
            const auto* original = types.find(binding->original_type);
            if (!original) continue;
            const auto is = [&](const char* behavior) {
                return std::find(original->behavior.begin(), original->behavior.end(), behavior) != original->behavior.end();
            };
            std::string current;
            if (is("DUMMY_GROUND_COMPANY")) { current = original->company_transport; ++companies; }
            else if (is("DUMMY_SPACE_FIGHTER_SQUADRON")) { current = "Galactic_Squadron"; ++squadrons; }
            else continue;
            const auto literal = std::find_if(options.types.begin(), options.types.end(),
                [&](const auto& value) { return value.original_type == current; });
            const auto reported = born->current_type(row.id);
            expect(literal != options.types.end() && reported && reported.value() == literal->id,
                "installed fresh current representation equals the actual constructor binding");
        }
        expect(companies && squadrons, "installed fresh stock campaign proves company transport and squadron current types");
        // WGM-38/WGM-163: a company transport skips space combat exactly when every contained actor does. The expectation
        // derives from each member's authored initially enabled Galactic_Stealth_Ability, never from a unit name.
        for (const auto& row : born->actor_records()) {
            const auto binding = std::find_if(options.types.begin(), options.types.end(),
                [&](const auto& value) { return value.id == row.original_type; });
            if (binding == options.types.end()) continue;
            const auto* original = types.find(binding->original_type);
            if (!original || std::find(original->behavior.begin(), original->behavior.end(), "DUMMY_GROUND_COMPANY") == original->behavior.end()) continue;
            bool all_members_stealth = !original->company_units.empty();
            for (const auto& member : original->company_units) {
                const auto* unit = types.find(member);
                all_members_stealth = all_members_stealth && unit && std::any_of(unit->initial_stealth.begin(), unit->initial_stealth.end(),
                    [](const auto& ability) { return ability.initially_enabled.value_or(true); });
            }
            // The expectation above derives from the same type table, so pin the authored stealth members of the stock
            // Rebel droid team directly: R2-D2 and C-3PO each author an enabled Galactic_Stealth_Ability.
            std::string lowered = binding->original_type;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowered == "droids_team") {
                ++droid_teams_seen;
                for (const auto& member : original->company_units) {
                    const auto* unit = types.find(member);
                    expect(unit && !unit->initial_stealth.empty(), "stock droid team member type table carries its authored Galactic_Stealth_Ability: " + member);
                }
                expect(all_members_stealth, "stock droid team is an all-stealth company in the type table");
            }
            // Name the failing input: the transport role, the contained actors and their retained stealth scalar.
            std::size_t contained{}, contained_stealth{};
            for (const auto& member : born->actor_records()) if (member.container == row.id) {
                ++contained; contained_stealth += member.abilities.present && member.abilities.combat_skip;
            }
            expect(row.combat_role == gc::ActorCombatRole::transport, "installed company transport actor carries the transport combat role: " + binding->original_type);
            expect(contained == original->company_units.size(), "installed company transport contains every authored member actor: " + binding->original_type);
            if (all_members_stealth || lowered == "droids_team") expect(contained_stealth == contained,
                "installed company members retain their authored enabled stealth scalar: " + binding->original_type);
            const auto skip = born->actor_combat_skip(row.id);
            expect(skip && skip.value() == (all_members_stealth || lowered == "droids_team"),
                "installed company transport skips combat exactly when every contained member authors enabled stealth (WGM-38/163): " + binding->original_type);
            installed_stealth_company |= all_members_stealth && skip && skip.value();
            // The release gate prepares the same campaign with only its selected factions active (Empire, Rebel and the
            // authored human), which is the game the AI actually plays. The droid team must keep its stealth there too.
            if (lowered == "droids_team" && std::string_view(name) == "Sandbox_Equal_Footing_Underworld" && gate_droid_teams_seen == 0) {
                cp::CampaignSetupOptions gate_options = options;
                gate_options.players.clear(); gate_options.seats.clear(); gate_options.solo_reference_human.reset();
                for (const auto& faction : data.factions) {
                    const auto id = static_cast<gc::PlayerId>(gate_options.players.size() + 1);
                    const bool selected_faction = faction.name == "Empire" || faction.name == "Rebel" || faction.name == source->starting_active_player;
                    gate_options.players.push_back({{id, id, 0}, faction.name, selected_faction || !faction.is_playable.value_or(false)});
                    if (faction.name == "Empire" || faction.name == "Rebel")
                        gate_options.seats.push_back({gc::SeatId{id}, id, gc::SeatKind::ai, id - 1, true, 0});
                    if (faction.name == source->starting_active_player) {
                        gate_options.solo_reference_human = id;
                        gate_options.seats.push_back({gc::SeatId{id}, id, gc::SeatKind::human, id - 1, true, 1});
                    }
                }
                auto gate_plan = cp::plan_live_campaign_setup(filesystem, hero_data, types, name, gate_options, "Normal_Default");
                expect(bool(gate_plan), "gate-shaped installed campaign plans");
                if (gate_plan) {
                    auto gate_services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, hero_data, types, gate_plan.value(), "Normal_Default");
                    expect(bool(gate_services), "gate-shaped installed campaign services prepare");
                    if (gate_services) {
                        auto gate_root = cp::CampaignRoot::create(gate_plan.value(), gate_services.value());
                        expect(bool(gate_root), "gate-shaped installed campaign creates");
                        if (gate_root) {
                            const auto check_droids = [&](std::string_view when) {
                                const auto gate_born = gate_root.value().game()->galaxy().snapshot();
                                for (const auto& transport : gate_born->actor_records()) {
                                    const auto bound = std::find_if(gate_options.types.begin(), gate_options.types.end(),
                                        [&](const auto& value) { return value.id == transport.original_type; });
                                    if (bound == gate_options.types.end() || bound->original_type != binding->original_type) continue;
                                    if (when == "creation") ++gate_droid_teams_seen;
                                    std::size_t members{}, stealthy{};
                                    for (const auto& member : gate_born->actor_records()) if (member.container == transport.id) {
                                        ++members; stealthy += member.abilities.present && member.abilities.combat_skip;
                                    }
                                    expect(members == original->company_units.size() && stealthy == members,
                                        std::string("gate-shaped droid team members retain their authored enabled stealth scalar at ") + std::string(when));
                                    const auto gate_skip = gate_born->actor_combat_skip(transport.id);
                                    expect(gate_skip && gate_skip.value(), std::string("gate-shaped droid team transport skips combat at ") + std::string(when));
                                }
                            };
                            check_droids("creation");
                            const eawr::platform::ThreadWorkerAdapter gate_executor(1);
                            for (unsigned gate_tick = 0; gate_tick < 3; ++gate_tick) {
                                const auto gate_stepped = gate_root.value().game()->step(gate_executor);
                                expect(bool(gate_stepped), "gate-shaped installed campaign steps");
                                if (!gate_stepped) break;
                            }
                            check_droids("the third galactic tick");
                        }
                    }
                }
            }
            // The retained stealth inputs survive the writes the AI makes to a company transport: separation from
            // its fleet, surface landing and surface launch (all ordinary admitted native operations).
            if (all_members_stealth && !lifecycle_checked) {
                auto made = gc::GalacticSession::create(checkpoint_game.galaxy().setup());
                expect(bool(made), "installed company lifecycle world creates");
                if (made) {
                    auto world = std::move(made).value();
                    // Reconstruct the same fixture, then resolve its transport
                    // by authored identity rather than borrowing a snapshot ID.
                    const auto initial = world.snapshot();
                    const auto actors = initial->actor_records();
                    const auto match = std::find_if(actors.begin(), actors.end(), [&](const auto& actor) {
                        return actor.original_type == row.original_type && actor.owner == row.owner
                            && actor.planet == row.planet && !actor.dead && !actor.delete_pending;
                    });
                    expect(match != actors.end(), "installed lifecycle fixture resolves its authored company transport");
                    if (match == actors.end()) continue;
                    lifecycle_checked = true;
                    const auto lifecycle_actor = match->id;
                    const auto skips = [&](std::string_view step) {
                        const auto value = world.snapshot()->actor_combat_skip(lifecycle_actor);
                        expect(value && value.value(), std::string("installed stealth company transport still skips combat after ") + std::string(step));
                    };
                    skips("creation");
                    gc::FleetId fleet_id{}; gc::PlayerId owner{}; gc::PlanetId orbit{}; std::size_t size{};
                    const auto locate = [&] {
                        fleet_id = 0;
                        for (const auto& fleet : world.snapshot()->fleets()) for (const auto& member : fleet.members) if (member.id == lifecycle_actor) {
                            fleet_id = fleet.id; owner = fleet.owner; orbit = fleet.orbit; size = fleet.members.size();
                        }
                    };
                    locate();
                    if (fleet_id && size > 1) {
                        const auto removed = world.admit_native_fleet(owner, gc::RemoveFleetMembers{fleet_id, {lifecycle_actor}});
                        bool separated = false;
                        if (removed && removed.value().admitted) {
                            const auto recreated = world.admit_native_fleet(owner, gc::CreateFleet{orbit, {lifecycle_actor}, 0});
                            separated = recreated && recreated.value().admitted;
                        }
                        if (separated) { ++lifecycle_steps; skips("separation from its fleet"); }
                        locate();
                    }
                    if (fleet_id) {
                        const auto removed = world.admit_native_fleet(owner, gc::RemoveFleetMembers{fleet_id, {lifecycle_actor}});
                        bool linked = false;
                        if (removed && removed.value().admitted) {
                            const auto landed = world.land_native_transport(owner, lifecycle_actor, orbit);
                            linked = landed && landed.value().linked;
                        }
                        if (linked) {
                            ++lifecycle_steps; skips("surface landing");
                            const auto launched = world.launch_native_transport(owner, lifecycle_actor);
                            if (launched && launched.value().admitted) { ++lifecycle_steps; skips("surface launch"); }
                        }
                    }
                }
            }
        }
        std::cout << "current representations campaign=" << name << " companies=" << companies << " squadrons=" << squadrons << '\n';
        for (const auto& [original, target] : {std::pair{"DS_Primary_Hyperdrive", 2},
                std::pair{"DS_Shield_Gen", 3}, std::pair{"DS_Superlaser_Core", 4}, std::pair{"DS_Durasteel", 5}}) {
            const auto binding = std::find_if(options.types.begin(), options.types.end(),
                [&](const auto& row) { return row.original_type == original; });
            expect(binding != options.types.end(), "stock tech upgrade retains its original binding");
            if (binding == options.types.end()) continue;
            for (const auto& location : setup.production->planets) {
                const auto value = std::find_if(location.offers.begin(), location.offers.end(),
                    [&](const auto& row) { return row.type == binding->id; });
                expect(value != location.offers.end() && value->kind == gc::ProductionKind::technology
                    && value->target_tech == target, "stock DS offer applies its authored numeric target 2-5");
            }
        }
        expect(std::any_of(setup.planets.begin(), setup.planets.end(), [&](const auto& planet) { return planet.owner == *options.solo_reference_human; }),
            "authored local forces establish owned planets");
        for (const auto& request : planned.value().forces()) {
            const auto* type = planned.value().original_type(request.type);
            const bool base = type && std::find(type->behavior.begin(), type->behavior.end(), "DUMMY_STAR_BASE") != type->behavior.end();
            const auto producer = std::find_if(setup.production->planets.begin(), setup.production->planets.end(),
                [&](const auto& row) { return row.planet == request.planet; });
            if (base) expect(producer != setup.production->planets.end() && producer->space_level >= type->base_level.value_or(0), "authored station reaches its location");
            else expect(std::any_of(setup.census->objects.begin(), setup.census->objects.end(), [&](const auto& row) {
                return row.original_type == request.type && row.owner == request.owner && row.planet == request.planet;
            }), "authored starting original retains owner and location");
        }
        for (const auto& state : setup.control->state)
            expect(state.initialized && state.current_player == state.target_player && state.current == state.target, "complete tick-zero allegiance");
        const auto identities = eawr::checkpoint::game_identities(checkpoint_game.galaxy().setup());
        const auto save = eawr::checkpoint::save_game_checkpoint(checkpoint_game, identities);
        if (!save) throw std::runtime_error(eawr::core::format_diagnostic(save.error()));
        const auto saved = save.value();
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto loaded = eawr::checkpoint::load_game_checkpoint(saved, identities, load_options, executor);
            if (!loaded) throw std::runtime_error(eawr::core::format_diagnostic(loaded.error()));
            auto restored = std::move(loaded).value();
            for (const auto id : stealth_hero_fleets)
                expect(restored.session.galaxy().snapshot()->fleet_combat_skip(id).value(),
                    "installed constructed hero fleet retains live stealth after fresh restore");
            expect(eawr::checkpoint::save_game_checkpoint(restored.session, identities).value() == saved, "stock checkpoint exact bytes");
            std::vector<std::string> hashes;
            for (int tick = 0; tick < 3; ++tick) {
                auto step = restored.session.step(executor);
                if (!step) throw std::runtime_error(eawr::core::format_diagnostic(step.error()));
                hashes.push_back(step.value().state_hash.get());
            }
            if (workers == 1) reference = hashes; else expect(hashes == reference, "stock continuation agrees at 1/2/4/8");
        }
        eawr::platform::LiveGameSession::Options live_options; live_options.workers = 2;
        live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
        expect(campaign.game()->completed_tick() == 0, "retained startup root remains initialized at tick zero");
        auto live = eawr::platform::LiveGameSession::start(std::move(campaign), live_options);
        if (!live) throw std::runtime_error(eawr::core::format_diagnostic(live.error()));
        expect(live.value()->frame().latest != nullptr, "retained root publishes live snapshot"); live.value()->stop();
        if (source->name == "Sandbox_Equal_Footing_Underworld") {
            auto prepared = eawr::platform::PreparedLiveCampaign::create(filesystem_owner, catalog.catalog, data,
                types, name, options, "Normal_Default");
            if (!prepared) throw std::runtime_error(eawr::core::format_diagnostic(prepared.error()));
            const auto initial = prepared.value()->snapshot();
            expect(prepared.value()->plan().forces().size() == planned.value().forces().size()
                && prepared.value()->layout().planets == setup.planets, "opaque startup publishes the same initialized layout");
            auto transferred = eawr::platform::LiveGameSession::start(std::move(prepared).value(), live_options);
            if (!transferred) throw std::runtime_error(eawr::core::format_diagnostic(transferred.error()));
            expect(transferred.value()->frame().latest == initial,
                "opaque owner transfer retains the exact prepared snapshot without recreating startup");
            transferred.value()->stop();
        }
        std::cout << "stock live root " << name << " forces=" << planned.value().forces().size() << '\n';
    }
    if (all_mp || solo_sources) return;
    expect(installed_stealth_hero_fleet, "installed automatic stealth hero has a recursively stealthed fleet");
    expect(installed_stealth_company, "installed stock includes an all-stealth company transport that skips combat");
    expect(droid_teams_seen > 0 && gate_droid_teams_seen > 0, "installed stock exercised the droid team in the ordinary and the gate-shaped campaign");
    expect(lifecycle_checked && lifecycle_steps > 0, "installed company lifecycle exercised a real separation, landing or launch");
    std::cout << "company lifecycle steps=" << lifecycle_steps << '\n';
}
void installed_origin_starbases() {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: Origin starbases require EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data").value().mount);
    const auto fs = std::make_shared<const eawr::vfs::Vfs>(eawr::vfs::Vfs::mount(mounts).value());
    const auto catalog = eawr::data::load_catalog(*fs, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(*fs, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    namespace ui = eawr::presentation::ui;
    // The preview's campaign setup screen supplies these same listing/variant facts.
    std::vector<ui::CampaignFacts> facts;
    for (const auto& source : data.campaigns) {
        ui::CampaignFacts entry;
        entry.name = source.name; entry.campaign_set = source.campaign_set;
        entry.faction = source.starting_active_player; entry.title = source.text_id;
        entry.sort_order = source.sort_order.value_or(1000);
        entry.listed = source.is_listed.value_or(true);
        entry.multiplayer = source.is_multiplayer.value_or(false);
        entry.tutorial = source.tutorial.value_or(false);
        facts.push_back(std::move(entry));
    }
    std::vector<ui::FactionFacts> factions;
    for (const auto& source : data.factions)
        factions.push_back({source.name, source.name, source.is_playable.value_or(false),
            source.displayed_tech_level_adjustment.value_or(0)});
    ui::CampaignSetupModel menu(std::move(facts), std::move(factions));
    bool mixed_surface_seen = false;
    for (const auto* name : {"Sandbox_Origin_Rebel", "Sandbox_Origin_Empire"}) {
        const auto* source = data.campaign(name);
        if (!source) throw std::runtime_error("missing stock Origin campaign");
        std::optional<std::size_t> row;
        for (std::size_t i = 0; i < menu.rows().size(); ++i)
            if (menu.campaigns()[menu.rows()[i].campaign].campaign_set == source->campaign_set) row = i;
        expect(row.has_value(), "Origin appears in the preview campaign setup list");
        menu.select_row(row);
        for (std::size_t i = 0; i < menu.factions().size(); ++i)
            if (menu.factions()[i].name == source->starting_active_player) menu.select_faction(i);
        const auto request = menu.start_request();
        expect(request && request->campaign == name, "listed Origin faction produces the campaign launch request");
        if (!request) throw std::runtime_error("Origin campaign is unreachable from the menu model");
        cp::CampaignSetupOptions options; options.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            options.players.push_back({{id, id, 0}, faction.name, true});
            const bool human = faction.name == source->starting_active_player;
            if (human) options.solo_reference_human = id;
            if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
                human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
        }
        std::vector<std::string> names;
        for (const auto& type : types.types) names.push_back(type.name);
        std::sort(names.begin(), names.end());
        for (const auto& type : names) options.types.push_back({options.types.size() + 1, type});
        auto result = eawr::platform::PreparedLiveCampaign::create(fs, catalog.catalog, data,
            types, request->campaign, options, "Normal_Default");
        if (!result) throw std::runtime_error(std::string(name) + ": " + eawr::core::format_diagnostic(result.error()));
        auto prepared = std::move(result).value();
        const auto& plan = prepared->plan(); const auto& setup = prepared->layout();
        for (const auto& planet : setup.control->planets) if (!planet.landed.empty())
            mixed_surface_seen |= std::any_of(planet.landed.begin(), planet.landed.end(),
                [&](const auto& force) { return force.owner != planet.landed.front().owner; });
        std::map<gc::PlanetId, cp::StartingForceRequest> last;
        std::size_t duplicates{};
        for (const auto& force : plan.forces()) {
            const auto* type = plan.original_type(force.type);
            if (!type || std::find(type->behavior.begin(), type->behavior.end(), "DUMMY_STAR_BASE") == type->behavior.end()) continue;
            duplicates += last.contains(force.planet) ? 1U : 0U;
            last[force.planet] = force;
        }
        expect(duplicates == (source->starting_active_player == "Rebel" ? 2U : 6U),
            "installed Origin retains every authored duplicate station");
        for (const auto& [planet, force] : last) {
            const auto control = std::find_if(setup.control->planets.begin(), setup.control->planets.end(),
                [&](const auto& entry) { return entry.planet == planet; });
            const auto production = std::find_if(setup.production->planets.begin(), setup.production->planets.end(),
                [&](const auto& entry) { return entry.planet == planet; });
            const auto level = static_cast<std::uint32_t>(*plan.original_type(force.type)->base_level);
            expect(control != setup.control->planets.end() && control->starbase == gc::ControlBase{force.owner, level}
                && production != setup.production->planets.end() && production->starbase_type == force.type
                && production->starbase_owner == force.owner && production->space_level == level,
                "installed station type, level and owner match the last authored record");
        }
        for (const auto* planet_name : {"Bothawui", "Mandalore"}) {
            const auto location = std::find_if(plan.locations().begin(), plan.locations().end(),
                [&](const auto& entry) { return entry.original_type == planet_name; });
            if (location == plan.locations().end() || !location->planet) throw std::runtime_error("missing Origin test planet");
            const auto& force = last.at(location->planet->id);
            expect(force.original_type == "Underworld_Star_Base_2", "Bothawui and Mandalore resolve to the authored Underworld station");
            const auto surface = std::find_if(setup.control->planets.begin(), setup.control->planets.end(),
                [&](const auto& entry) { return entry.planet == location->planet->id; });
            const auto index = static_cast<std::size_t>(surface - setup.control->planets.begin());
            expect(surface != setup.control->planets.end() && !surface->landed.empty(), "Origin test planet has authored surface forces");
            const auto ground = surface->ground_base.level ? surface->ground_base.owner : surface->landed.front().owner;
            expect(setup.control->state[index].controller == ground, "WGS-186 Origin initial controller follows ground base then first transport");
            std::cout << name << ' ' << planet_name << " station=" << force.original_type << " owner=" << force.owner << '\n';
        }
        std::string baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto world = gc::GalacticSession::create(setup).value();
            const auto stepped = world.step(executor).value();
            const auto snapshot = world.snapshot();
            for (std::size_t i = 0; i < snapshot->control()->planets.size(); ++i) {
                const auto& surface = snapshot->control()->planets[i];
                if (surface.landed.empty()) continue;
                const auto owner = surface.ground_base.level ? surface.ground_base.owner : surface.landed.front().owner;
                expect(snapshot->control()->state[i].controller == owner
                    && snapshot->production()->planets[i].ground_occupier == owner,
                    "WGS-186 installed Origin controller and production ground occupation retain authored precedence");
            }
            if (baseline.empty()) baseline = stepped.state_hash.get();
            else expect(stepped.state_hash.get() == baseline, "Origin startup and continuation equal on 1/2/4/8 workers");
        }
        eawr::platform::LiveGameSession::Options live_options;
        live_options.workers = 2; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
        auto live = eawr::platform::LiveGameSession::start(std::move(prepared), live_options).value();
        live->advance_to(1);
        expect(live->wait_for(1, std::chrono::seconds(20)), "Origin menu launch creates and advances a live campaign");
        live->stop();
        std::cout << "origin-live " << name << " duplicates=" << duplicates << " menu-listed=yes parity=1/2/4/8\n";
    }
    expect(mixed_surface_seen, "installed Origin startup retains authored mixed surface owners");
}
void installed_starting_levels(const bool accepted_route = false) {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: starting technology requires EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data").value().mount);
    const auto fs = std::make_shared<const eawr::vfs::Vfs>(eawr::vfs::Vfs::mount(mounts).value());
    const auto catalog = eawr::data::load_catalog(*fs, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(*fs, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    std::vector<std::string> names;
    for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    const auto directory = std::filesystem::temp_directory_path() / ("starting-tech-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::size_t cases{}, live_cases{};
    for (const auto& source : data.campaigns) {
        if (!source.is_listed.value_or(true) || source.is_multiplayer.value_or(false) || source.tutorial.value_or(false)) continue;
        const bool equal_footing = source.name == "Sandbox_Equal_Footing_Underworld"
            || source.name == "Sandbox_Equal_Footing_Rebel" || source.name == "Sandbox_Equal_Footing_Empire";
        if (accepted_route && !equal_footing && source.name != "Sandbox_Rim_Worlds_Empire") continue;
        cp::CampaignSetupOptions options; options.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            options.players.push_back({{id, id, 0}, faction.name, true});
            const bool human = faction.name == source.starting_active_player;
            if (human) options.solo_reference_human = id;
            if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
                human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
        }
        for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
        const auto human_credits = std::find_if(source.starting_credits.begin(), source.starting_credits.end(),
            [&](const auto& row) { return row.faction == source.starting_active_player; });
        if (human_credits == source.starting_credits.end()) throw std::runtime_error("missing authored test credits");
        for (const std::int64_t level : {1, 2, 3, 4, 5}) {
            if (accepted_route && level != 5) continue;
            options.custom = cp::CampaignCustomOptions{human_credits->value, level, 5, true};
            if (accepted_route) {
                namespace ui = eawr::presentation::ui;
                // The menu's selected faction variant, real option edits and Accept
                // produce the request passed to the live campaign launcher.
                ui::CampaignFacts facts; facts.name = source.name; facts.campaign_set = source.campaign_set;
                facts.faction = source.starting_active_player; facts.starting_credits = human_credits->value;
                facts.supports_custom_settings = source.supports_custom_settings.value_or(true);
                facts.minimum_custom_tech = source.minimum_custom_tech.value_or(1);
                facts.maximum_custom_tech = source.maximum_custom_tech.value_or(5);
                for (const auto& row : source.starting_tech_levels) if (row.faction == facts.faction) { facts.starting_tech = row.value; break; }
                for (const auto& row : source.max_tech_levels) if (row.faction == facts.faction) { facts.maximum_tech = row.value; break; }
                std::vector<ui::FactionFacts> choices;
                for (const auto& row : data.factions) choices.push_back({row.name, row.name,
                    row.is_playable.value_or(false), row.displayed_tech_level_adjustment.value_or(0)});
                ui::CampaignSetupModel menu({facts}, std::move(choices), {false, true});
                menu.select_row(0);
                for (std::size_t i = 0; i < menu.factions().size(); ++i)
                    if (menu.factions()[i].name == facts.faction) menu.select_faction(i);
                expect(menu.open_options() && menu.select_maximum_tech(5) && menu.select_starting_tech(5), "menu selects level five");
                menu.close_options(true);
                const auto request = menu.start_request();
                expect(request && request->campaign == source.name && request->economy, "accepted menu request retains faction variant and options");
                if (!request || !request->economy) throw std::runtime_error("accepted menu request missing economy");
                options.custom = cp::CampaignCustomOptions{request->economy->credits,
                    request->economy->starting_tech, request->economy->maximum_tech, true};
            }
            const auto checked = [&]<class T>(eawr::core::Result<T> result) -> T {
                if (!result) throw std::runtime_error(source.name + " level=" + std::to_string(level) + ": "
                    + eawr::core::format_diagnostic(result.error()));
                return std::move(result).value();
            };
            auto planned = checked(cp::plan_live_campaign_setup(*fs, data, types, source.name, options, "Normal_Default"));
            for (std::size_t i = 0; i < options.players.size(); ++i) {
                const auto* faction = data.faction(options.players[i].faction);
                const auto adjustment = faction->displayed_tech_level_adjustment.value_or(0) != 0 ? 1 : 0;
                expect(planned.tech()[i].requested_current == level - adjustment
                    && planned.tech()[i].requested_maximum == 5 - adjustment,
                    "all stock plans map the selected level through each human/AI faction");
            }
            ++cases;
            // Retain all 90 tech-plan mappings and exercise live creation in
            // every Equal Footing faction variant. Origin duplicate stations
            // have their own focused installed_origin_starbases coverage.
            if (!equal_footing && !accepted_route) continue;
            auto services = checked(cp::prepare_live_campaign_services(*fs, catalog.catalog, data, types, planned, "Normal_Default"));
            auto campaign = checked(cp::CampaignRoot::create(planned, services));
            const auto& setup = campaign.game()->galaxy().setup();
            if (accepted_route) {
                auto low_options = options; low_options.custom->starting_tech = 1;
                auto prepared = checked(eawr::platform::PreparedLiveCampaign::create(fs, catalog.catalog, data,
                    types, source.name, options, "Normal_Default"));
                auto low_prepared = checked(eawr::platform::PreparedLiveCampaign::create(fs, catalog.catalog, data,
                    types, source.name, low_options, "Normal_Default"));
                const auto high_snapshot = prepared->snapshot();
                const auto low_snapshot = low_prepared->snapshot();
                expect(high_snapshot->player_economy() == campaign.game()->galaxy().snapshot()->player_economy(),
                    "prepared accepted startup preserves every canonical economy field from the independent root");
                bool available_upgrade{}, authored_gate{};
                const auto& low_accounts = low_snapshot->player_economy()->players;
                const auto local_account = std::find_if(low_accounts.begin(), low_accounts.end(),
                    [&](const auto& row) { return row.player == *options.solo_reference_human; });
                const auto& high_accounts = high_snapshot->player_economy()->players;
                const auto high_account = std::find_if(high_accounts.begin(), high_accounts.end(),
                    [&](const auto& row) { return row.player == *options.solo_reference_human; });
                bool requires_authored_gate{};
                if (local_account != low_accounts.end() && high_account != high_accounts.end())
                    for (const auto& planet : high_snapshot->production()->planets) {
                    if (planet.target_owner != local_account->player) continue;
                    for (const auto& offer : planet.offers)
                        if ((offer.kind == gc::ProductionKind::starbase || offer.kind == gc::ProductionKind::structure) && offer.affiliated
                            && !offer.initially_locked && offer.tech > local_account->current_tech
                            && offer.tech <= high_account->current_tech)
                            requires_authored_gate = true;
                }
                for (const auto& high : high_snapshot->production_offers()) {
                    if ((high.kind != gc::ProductionKind::starbase && high.kind != gc::ProductionKind::structure) || !high.current
                        || high.admission != gc::ProductionAdmission::allowed) continue;
                    const auto low_rows = low_snapshot->production_offers();
                    const auto low = std::find_if(low_rows.begin(), low_rows.end(), [&](const auto& row) {
                        return row.planet == high.planet && row.type == high.type;
                    });
                    const auto* type = planned.original_type(high.type);
                    const auto account = std::find_if(low_accounts.begin(), low_accounts.end(),
                        [&](const auto& row) { return row.player == high.player; });
                    if (low == low_rows.end() || !type || account == low_accounts.end()) continue;
                    // WGEP-147: an initially locked type has an independent unlock
                    // gate before numeric technology. This case isolates tech-only
                    // admission; the complete initial-unlock pass is checked below.
                    if (type->build_initially_locked.value_or(false)) continue;
                    const bool gated = type->tech_level.value_or(0) > account->current_tech;
                    // Strict initial unlocks can keep a zero-tech Rebel card locked
                    // at internal zero. Select a numeric tech contrast, or a card
                    // available at both levels when this interval authors no gate.
                    if (!gated && (!low->current || low->admission != gc::ProductionAdmission::allowed)) continue;
                    expect(gated ? !low->current && low->admission == gc::ProductionAdmission::tech
                        : low->current && low->admission == gc::ProductionAdmission::allowed,
                        "startup structure/upgrade follows its authored technology gate at levels one and five");
                    std::cout << "accepted-tech contrast " << source.name << " planet=" << high.planet
                        << " type=" << type->name << " tech=" << type->tech_level.value_or(0)
                        << " high=allowed low=" << (gated ? "tech" : "allowed")
                        << " admission=" << static_cast<unsigned>(low->admission) << '\n';
                    available_upgrade = true;
                    if (gated) { authored_gate = true; break; }
                }
                expect(available_upgrade, "same-seed authored startup structure/upgrade is enabled at five");
                expect(authored_gate == requires_authored_gate,
                    "tech-only denial is required wherever offered affiliated upgrades author a higher technology level");
                std::cout << "accepted-tech authored-gate " << source.name << " present=" << authored_gate << '\n';
                eawr::platform::LiveGameSession::Options live_options;
                live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
                auto live = checked(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));
                const auto snapshot = live->frame().latest;
                expect(snapshot == high_snapshot, "live owner transfer retains the exact accepted startup publication");
                expect(snapshot && snapshot->player_economy(), "accepted startup publishes technology accounts");
                if (snapshot && snapshot->player_economy()) for (const auto& account : snapshot->player_economy()->players) {
                    const auto expected = std::find_if(setup.player_economy->players.begin(), setup.player_economy->players.end(),
                        [&](const auto& row) { return row.player == account.player; });
                    std::cout << "accepted-tech live " << source.name << " player=" << account.player
                        << " current=" << account.current_tech << " maximum=" << account.maximum_tech << '\n';
                    if (expected != setup.player_economy->players.end())
                        std::cout << "accepted-economy input/live " << source.name << " player=" << account.player
                            << " credits=" << expected->credits.raw() << '/' << account.credits.raw()
                            << " cap=" << expected->credit_cap.raw() << '/' << account.credit_cap.raw()
                            << " current=" << expected->current_tech << '/' << account.current_tech
                            << " maximum=" << expected->maximum_tech << '/' << account.maximum_tech << '\n';
                    expect(expected != setup.player_economy->players.end() && account.current_tech == expected->current_tech
                        && account.maximum_tech == expected->maximum_tech, "accepted current/max technology survives the actual launch path");
                }
                live->stop();
            }
            for (std::size_t i = 0; i < options.players.size(); ++i) {
                const auto* faction = data.faction(options.players[i].faction);
                const auto expected = level - (faction->displayed_tech_level_adjustment.value_or(0) != 0 ? 1 : 0);
                expect(setup.player_economy->players[i].current_tech == expected
                    && setup.player_economy->players[i].maximum_tech == 5 - (faction->displayed_tech_level_adjustment.value_or(0) != 0 ? 1 : 0),
                    "every stock family/faction receives custom start/max tech including AI");
                const auto& player = setup.production->players[i];
                for (const auto& type : setup.production->technology_types) {
                    const bool eligible = type.initially_locked && type.slicer_unlockable && type.tech < expected
                        && std::binary_search(type.affiliated_players.begin(), type.affiliated_players.end(), player.player);
                    expect(std::binary_search(player.unlocked.begin(), player.unlocked.end(), type.type) == eligible,
                        "stock creation uses the complete faction-affiliated strict initial unlock pass");
                }
            }
            // Every level and faction restores with all worker counts.
            {
                const auto ids = eawr::checkpoint::game_identities(setup);
                const auto saved = eawr::checkpoint::save_game_checkpoint(*campaign.game(), ids).value();
                std::string baseline;
                for (const auto workers : {1U, 2U, 4U, 8U}) {
                    const eawr::platform::ThreadWorkerAdapter executor(workers);
                    auto loaded = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor).value();
                    expect(loaded.session.galaxy().setup().production == setup.production
                        && loaded.session.galaxy().setup().player_economy == setup.player_economy, "stock custom tech/unlocks save/load");
                    const auto hash = loaded.session.step(executor).value().state_hash.get();
                    if (baseline.empty()) baseline = hash; else expect(hash == baseline, "stock custom creation 1/2/4/8 equality");
                }
            }
            if (source.name == "Sandbox_Equal_Footing_Empire") {
                // Session-free read, then fresh preparation from the saved metadata: the menu path.
                auto prepared = eawr::platform::PreparedLiveCampaign::create(fs, catalog.catalog, data,
                    types, source.name, options, "Normal_Default").value();
                eawr::platform::LiveGameSession::Options live_options;
                live_options.workers = 2; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
                live_options.saves.directory = directory.string();
                auto live = eawr::platform::LiveGameSession::start(std::move(prepared), live_options).value();
                eawr::platform::LiveSaveRequest request; request.seat = {*options.solo_reference_human}; request.name = "level" + std::to_string(level);
                const auto token = live->request_save(request); live->advance_to(1);
                expect(live->wait_for(1, std::chrono::seconds(20)) && live->wait_save(token, std::chrono::seconds(20)), "custom stock save completes");
                const auto saved_bytes = eawr::platform::save_detail::read(directory.string(), request.name).value();
                const auto checkpoint = eawr::checkpoint::parse_checkpoint(saved_bytes).value();
                const auto* ai_section = checkpoint.find(eawr::checkpoint::SectionId::galactic_ai);
                if (!ai_section) throw std::runtime_error("default campaign save missing galactic AI section");
                const auto saved_ai = cp::decode_galactic_ai_save(ai_section->bytes).value();
                expect(!saved_ai.state, accepted_route
                    ? "accepted menu launch does not install native galactic AI by default"
                    : "fresh campaign does not install native galactic AI by default");
                const auto slot = eawr::platform::read_save_slot(directory.string(), request.name).value();
                expect(!slot.diagnostic && slot.campaign && slot.campaign->custom == options.custom, "menu listing retains accepted startup values");
                auto fresh_options = options; fresh_options.custom = slot.campaign->custom;
                auto fresh = eawr::platform::PreparedLiveCampaign::create(fs, catalog.catalog, data, types,
                    slot.campaign->campaign, fresh_options, slot.campaign->difficulty).value();
                auto owner = eawr::platform::LiveGameSession::start(std::move(fresh), live_options).value();
                auto restored = owner->prepare_load(request.name).value();
                auto running = live->prepare_load(request.name).value();
                expect(restored->state_sha256() == running->state_sha256(), "main-menu custom restore matches in-campaign restore without weakening identity");
                owner->stop(); live->stop();
            }
            ++live_cases;
            std::cout << "starting-tech " << source.name << " level=" << level << '\n';
        }
    }
    expect(cases == (accepted_route ? 4U : 90U), "all selected stock faction/family plans");
    expect(live_cases == (accepted_route ? 4U : 15U), "all selected stock faction variants create and restore");
    if (std::filesystem::exists(directory)) {
        for (const auto& file : std::filesystem::directory_iterator(directory)) std::filesystem::remove(file.path());
        std::filesystem::remove(directory);
    }
}
// Retail rig observation (Sandbox Equal Footing, human Empire then Rebel, first campaign day): what the local
// observer knows of planets it has not scouted. Empire (default mask 0) learns no foreign owner and no fleet;
// Rebel (default mask 17) learns owner and fleet presence of every discovered planet. Planets outside the
// discovery reach stay undiscovered for both (WGUI-04/05/06/12/38).
void installed_unscouted_intel() {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: stock live campaign requires EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        if (!mount) throw std::runtime_error(eawr::core::format_diagnostic(mount.error()));
        mounts.push_back(std::move(mount).value().mount);
    }
    const auto filesystem_owner = std::make_shared<const eawr::vfs::Vfs>(eawr::vfs::Vfs::mount(mounts).value());
    const auto& filesystem = *filesystem_owner;
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    for (const bool empire : {true, false}) {
        const std::string name = empire ? "Sandbox_Equal_Footing_Empire" : "Sandbox_Equal_Footing_Rebel";
        const auto* source = data.campaign(name); expect(source != nullptr, "stock family exists"); if (!source) continue;
        cp::CampaignSetupOptions options; options.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            options.players.push_back({{id, id, 0}, faction.name, true});
            const bool local = faction.name == source->starting_active_player;
            if (local) options.solo_reference_human = id;
            if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
                local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
        }
        std::vector<std::string> originals;
        for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
        auto planned = cp::plan_live_campaign_setup(filesystem, data, types, name, options, "Normal_Default");
        if (!planned) throw std::runtime_error(eawr::core::format_diagnostic(planned.error()));
        auto services = cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, types, planned.value(), "Normal_Default");
        if (!services) throw std::runtime_error(eawr::core::format_diagnostic(services.error()));
        auto created = cp::CampaignRoot::create(planned.value(), services.value());
        if (!created) throw std::runtime_error(eawr::core::format_diagnostic(created.error()));
        auto campaign = std::move(created).value();
        auto session = gc::GalacticSession::create(campaign.game()->galaxy().setup());
        if (!session) throw std::runtime_error(eawr::core::format_diagnostic(session.error()));
        auto galaxy = std::move(session).value();
        const eawr::platform::ThreadWorkerAdapter executor(1);
        auto step = galaxy.step(executor); // the first service pass runs discovery
        if (!step) throw std::runtime_error(eawr::core::format_diagnostic(step.error()));
        const auto snapshot = galaxy.snapshot();
        const auto local = *options.solo_reference_human;
        const auto by_name = [&](const char* planet) -> const gc::ObservedPlanet* {
            const auto row = std::find_if(planned.value().locations().begin(), planned.value().locations().end(),
                [&](const auto& value) { return value.original_type == planet; });
            if (row == planned.value().locations().end() || !row->planet) return nullptr;
            for (const auto& view : snapshot->observations())
                if (view.player == local && view.planet == row->planet->id) return &view;
            return nullptr;
        };
        const auto owner_of = [&](const gc::ObservedPlanet& view) -> std::optional<gc::PlayerId> {
            for (const auto& planet : campaign.game()->galaxy().setup().planets)
                if (planet.id == view.planet) return planet.owner;
            return std::optional<gc::PlayerId>{};
        };
        const std::string label = empire ? "Empire: " : "Rebel: ";
        for (const char* wireframe : empire
            ? std::initializer_list<const char*>{"Thyferra", "AlzocIII", "Hoth", "Mustafar", "Shola", "Hypori", "Ryloth", "Bonadan"}
            : std::initializer_list<const char*>{"Coruscant", "Byss", "Alderaan", "Corulag", "Kuat", "Abregado_Rae", "Fresia"}) {
            const auto* view = by_name(wireframe);
            expect(view && !view->discovered, label + wireframe + " is outside the first-day discovery reach (retail wireframe)");
        }
        std::size_t scouted_foreign = 0;
        for (const auto& view : snapshot->observations()) {
            if (view.player != local || !view.discovered) continue;
            const auto owner = owner_of(view);
            if (owner == local) continue;
            ++scouted_foreign;
            if (empire)
                expect(!view.knowledge.ownership && !view.knowledge.fleet_presence && !view.known_owner,
                    "Empire's default intelligence shows a discovered foreign planet with no owner and no fleet");
            else
                expect(view.knowledge.ownership && view.knowledge.fleet_presence && view.known_owner == owner,
                    "Rebel's default intelligence shows owner and fleet presence of every discovered planet");
        }
        expect(scouted_foreign >= 10, label + "discovery reaches foreign planets beyond the owned ones");
        if (empire) {
            for (const char* planet : {"Jabiim", "Carida", "Fondor", "Muunilinst", "Dagobah", "NalHutta"}) {
                const auto* view = by_name(planet);
                expect(view && view->discovered, std::string("Empire: ") + planet + " is textured but unowned in the retail capture");
            }
        } else {
            for (const char* planet : {"Anaxes", "AetenII", "Wayland"}) {
                const auto* view = by_name(planet);
                expect(view && view->discovered && view->known_owner && owner_of(*view) != local,
                    std::string("Rebel: ") + planet + " shows its Empire owner in the retail capture");
            }
        }
        if (empire) {
            // Growth over time (owner retail game, day 13: Naboo and Nal Hutta owner-coloured, Tatooine, Bothawui and
            // Dagobah still neutral; rig timeline, human Empire, no player action: Jabiim turned owner-coloured on
            // day 6 and two more planets on days 8 and 11). Stock starts every AI planet beside the Empire with a
            // level-2 starbase and the Empire's Minimum_Visible_Base_Level is 3 (Rebel and Underworld 1), so the AI's
            // base upgrades are what reveal ownership to the Empire: WGUI-11/12. Step one planet's levels 0..5.
            const auto& observer = *campaign.game()->galaxy().setup().observer;
            const auto player = std::find_if(observer.players.begin(), observer.players.end(),
                [&](const auto& row) { return row.player == local; });
            expect(player != observer.players.end() && player->minimum_visible_base_level == 3, "Empire's authored visibility threshold is 3");
            const std::int64_t threshold = player->minimum_visible_base_level;
            for (const char* stock_planet : {"Jabiim", "Fondor", "Bothawui", "Tatooine", "Naboo", "NalHutta", "Dagobah", "Carida"}) {
                const auto row = std::find_if(planned.value().locations().begin(), planned.value().locations().end(),
                    [&](const auto& value) { return value.original_type == stock_planet; });
                expect(row != planned.value().locations().end() && row->planet, std::string(stock_planet) + " is a stock planet");
                if (row == planned.value().locations().end() || !row->planet) continue;
                const auto planet = std::find_if(observer.planets.begin(), observer.planets.end(),
                    [&](const auto& value) { return value.planet == row->planet->id; });
                expect(planet != observer.planets.end(), std::string(stock_planet) + " has an observer row");
                if (planet == observer.planets.end()) continue;
                std::cout << "growth " << stock_planet << " start space_level=" << planet->space_level
                          << " ground_level=" << planet->ground_level << " owner=" << planet->target_owner << '\n';
                expect(std::max(planet->space_level, planet->ground_level) < threshold,
                    std::string(stock_planet) + " starts below the Empire's visibility threshold, so its owner is unknown");
                std::string line = std::string("growth ") + stock_planet + " steps:";
                for (std::int64_t level = 0; level <= 5; ++level) {
                    for (const bool space : {true, false}) {
                        auto grown = *planet; (space ? grown.space_level : grown.ground_level) = level;
                        (space ? grown.ground_level : grown.space_level) = 0;
                        if (space) grown.space_owner = grown.target_owner; else grown.ground_owner = grown.target_owner;
                        gc::IntelligenceContext context;
                        context.observer = local; context.local_player = observer.solo_reference_human;
                        context.allegiance = grown.allegiance; context.target_owner = grown.target_owner;
                        context.space_owner = grown.space_owner; context.ground_owner = grown.ground_owner;
                        context.space_stealth = grown.space_stealth; context.ground_stealth = grown.ground_stealth;
                        context.space_level = grown.space_level; context.ground_level = grown.ground_level;
                        context.minimum_visible_level = threshold; context.mask = player->default_intelligence;
                        const auto knowledge = gc::planet_knowledge(context);
                        const bool revealed = knowledge.ownership;
                        // A stealth base hides its level whatever it is (WGUI-10): Dagobah's ground base never reveals.
                        const bool stealth = space ? planet->space_stealth : planet->ground_stealth;
                        expect(revealed == (level >= threshold && !stealth), std::string(stock_planet) + (space ? " starbase" : " ground base")
                            + " level " + std::to_string(level) + ": the owner is known exactly from the threshold");
                        if (space) line += " " + std::to_string(level) + (revealed ? "=owner" : "=grey");
                    }
                }
                std::cout << line << '\n';
            }

            // Owner capture 2026-10-16 (day 15, Aeten 2): an Empire-owned planet with a Rebel fleet in orbit listed the
            // fleet as three count-only cells in our tooltip, where retail shows the ship icons. The owner mask of an
            // owned planet must keep the fleet's contents known while a hostile fleet sits there (WGUI-28/29/73).
            auto hostile_setup = campaign.game()->galaxy().setup();
            const auto own_planet = std::find_if(hostile_setup.planets.begin(), hostile_setup.planets.end(),
                [&](const auto& row) { return row.owner == local; });
            auto hostile = std::find_if(hostile_setup.fleets.begin(), hostile_setup.fleets.end(),
                [&](const auto& row) { return row.owner != local && !row.members.empty(); });
            expect(own_planet != hostile_setup.planets.end() && hostile != hostile_setup.fleets.end(),
                "Empire campaign has an own planet and a foreign fleet to stage");
            if (own_planet != hostile_setup.planets.end() && hostile != hostile_setup.fleets.end()) {
                const auto target = own_planet->id;
                hostile->orbit = target; hostile->orbit_slot = 0; hostile->motion.transit.reset();
                auto staged = gc::GalacticSession::create(hostile_setup);
                if (!staged) throw std::runtime_error(eawr::core::format_diagnostic(staged.error()));
                auto world = std::move(staged).value();
                for (int tick = 0; tick < 6; ++tick) {
                    auto stepped = world.step(executor);
                    if (!stepped) throw std::runtime_error(eawr::core::format_diagnostic(stepped.error()));
                    const auto view_snapshot = world.snapshot();
                    for (const auto& view : view_snapshot->observations()) {
                        if (view.player != local || view.planet != target) continue;
                        // The viewer supplies a display trait for every type (hero class) through the live helper.
                        struct Row { bool hero{}, named_hero{}, force_sensitive{}; std::optional<float> max_health; };
                        std::map<std::string, Row> art;
                        std::map<gc::TypeId, std::string> type_names;
                        for (const auto& fleet : view_snapshot->fleets())
                            for (const auto& member : fleet.members) {
                                type_names.emplace(member.type, "type" + std::to_string(member.type));
                                art.emplace("type" + std::to_string(member.type), Row{});
                            }
                        const auto traits = eawr::viewer::galactic::live_fleet_display_traits(type_names, art);
                        // The viewer passes the planets the seat currently sees; without that list no card is published.
                        std::vector<gc::FleetDisplayVisibility> sight;
                        for (const auto& row : view_snapshot->observations())
                            if (row.player == local) sight.push_back({row.planet, true});
                        const auto cards = gc::observed_fleets(*view_snapshot, observer.solo_reference_human, traits, 8, sight);
                        std::size_t typed = 0, groups = 0;
                        for (const auto& card : cards.cards) {
                            if (card.planet != target || card.local) continue;
                            for (const auto& group : card.groups) { ++groups; typed += group.type ? 1U : 0U; }
                        }
                        std::cout << "hostile tick " << tick << " ownership=" << view.knowledge.ownership
                                  << " contents=" << view.knowledge.fleet_contents << " count=" << view.knowledge.fleet_count
                                  << " known_owner=" << view.known_owner.value_or(0) << " typed_groups=" << typed << '/' << groups << '\n';
                        expect(view.knowledge.fleet_contents && groups > 0 && typed == groups,
                            "an own planet lists the typed groups of a hostile fleet in its orbit");
                    }
                }
            }
            // Korriban (owner capture 2026-10-16: Empire fleet in orbit, space battle imminent, enemy fleet hidden).
            // An own fleet in orbit makes the owner known (WGUI-12) but reveals neither the foreign fleet nor the
            // starbase level; only an active probe does (WGUI-25: stock Probe_Droid_System_Spy, Ground_Activated, 60 s,
            // despawns, bases, fleet count/contents, ground, structures, political control; no income, no stealth).
            const auto korriban_row = std::find_if(planned.value().locations().begin(), planned.value().locations().end(),
                [&](const auto& value) { return value.original_type == "Korriban"; });
            expect(korriban_row != planned.value().locations().end() && korriban_row->planet, "Korriban is a stock planet");
            if (korriban_row != planned.value().locations().end() && korriban_row->planet) {
                const auto korriban = std::find_if(observer.planets.begin(), observer.planets.end(),
                    [&](const auto& value) { return value.planet == korriban_row->planet->id; });
                expect(korriban != observer.planets.end(), "Korriban has an observer row");
                if (korriban != observer.planets.end()) {
                    gc::IntelligenceContext context;
                    context.observer = local; context.local_player = observer.solo_reference_human;
                    context.allegiance = korriban->allegiance; context.target_owner = korriban->target_owner;
                    context.space_owner = korriban->space_owner; context.ground_owner = korriban->ground_owner;
                    context.space_stealth = korriban->space_stealth; context.ground_stealth = korriban->ground_stealth;
                    context.space_level = korriban->space_level; context.ground_level = korriban->ground_level;
                    context.minimum_visible_level = threshold; context.mask = player->default_intelligence;
                    context.own_orbit = true;
                    const auto passive = gc::planet_knowledge(context);
                    expect(!passive.fleet_presence && !passive.fleet_count && !passive.fleet_contents,
                        "an own fleet in orbit alone does not reveal the foreign fleet");
                    expect(!passive.space_level || korriban->space_level >= threshold,
                        "an own fleet in orbit alone does not reveal the starbase level below the Empire threshold");
                    using gc::Intelligence;
                    context.mask |= gc::bit(Intelligence::space_level) | gc::bit(Intelligence::ground_level)
                        | gc::bit(Intelligence::fleet_presence) | gc::bit(Intelligence::fleet_count)
                        | gc::bit(Intelligence::fleet_contents) | gc::bit(Intelligence::ground_count)
                        | gc::bit(Intelligence::ground_contents) | gc::bit(Intelligence::structures)
                        | gc::bit(Intelligence::political_control);
                    const auto probed = gc::planet_knowledge(context);
                    expect(probed.space_level && probed.ground_level && probed.fleet_contents && probed.fleet_count
                        && probed.structures && !probed.income && !probed.income_breakdown,
                        "an active probe reveals bases, fleet contents and structures but no income");
                    std::cout << "korriban passive fleet_contents=" << passive.fleet_contents << " space_level=" << passive.space_level
                              << " probed fleet_contents=" << probed.fleet_contents << " space_level=" << probed.space_level << '\n';
                }
            }
        }
    }
}
// G3: malformed hero data and invalid ruined income degrade through the actual startup callbacks.
void installed_guard_load_notes(bool production_guard = false) {
    using Fixed = eawr::sim::math::Fixed;
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: GC load-note startup requires EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data").value().mount);
    const auto base = eawr::vfs::Vfs::mount(mounts).value();
    const auto directory = std::filesystem::temp_directory_path() / ("gc-hero-load-notes-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    const auto original = base.open("data/xml/namedherounits.xml").value();
    std::string text(reinterpret_cast<const char*>(original.data()), original.size());
    text = std::regex_replace(text, std::regex("<Min_Respawn_Time_Per_Tech_Level>[^<]*</Min_Respawn_Time_Per_Tech_Level>", std::regex::icase),
        "<Min_Respawn_Time_Per_Tech_Level>broken</Min_Respawn_Time_Per_Tech_Level>");
    text = std::regex_replace(text, std::regex("</HeroUnit>", std::regex::icase),
        "<Respawn_Tied_To_Structure>Missing_Guard_Structure</Respawn_Tied_To_Structure></HeroUnit>");
    std::filesystem::create_directories(directory / "xml");
    { std::ofstream out(directory / "xml/namedherounits.xml"); out << text; }
    std::string guarded_initial_base;
    if (production_guard) {
        const auto originals = eawr::data::load_catalog(base, eawr::data::Profile::foc).value();
        const auto stock_types = eawr::units::galactic_type_table(originals.catalog);
        const auto selected = std::find_if(stock_types.types.begin(), stock_types.types.end(), [](const auto& type) {
            return type.object_class == "SpaceUnit" && type.file != "data/xml/namedherounits.xml"
                && type.required_star_base_level.value_or(0) > 0
                && type.build_cost_credits.value_or(Fixed{}).raw() > 0
                && std::find(type.behavior.begin(), type.behavior.end(), "DUMMY_STARSHIP") != type.behavior.end();
        });
        if (selected == stock_types.types.end()) throw std::runtime_error("production guard fixture has no authored starship");
        const auto stock_data = eawr::data::galactic::load_galactic_data(base, originals.catalog).value();
        const auto* definition = stock_data.campaign("Sandbox_Equal_Footing_Rebel");
        if (!definition) throw std::runtime_error("production guard campaign is missing");
        const eawr::units::GalacticType* starting_base{};
        for (const auto& force : definition->starting_forces) {
            const auto* type = stock_types.find(force.type);
            if (type && type->object_class == "StarBase" && type->file != "data/xml/namedherounits.xml"
                && type->required_planets.empty()
                && type->build_advances_tech_level.value_or(0) == 0
                && std::find(type->behavior.begin(), type->behavior.end(), "DUMMY_STAR_BASE") != type->behavior.end()) {
                starting_base = type; break;
            }
        }
        if (!starting_base) throw std::runtime_error("production guard fixture has no authored starting base");
        guarded_initial_base = starting_base->name;
        // A logical file can contribute definitions from several mounted layers. Copy the
        // catalog winners, rather than VFS::open's single file, to retain every identity.
        const auto escape = [](std::string_view value) {
            std::string result;
            for (const char ch : value) switch (ch) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            default: result += ch; break;
            }
            return result;
        };
        const auto emit = [&](auto&& self, std::ostream& out, const eawr::data::XmlNode& node,
                              std::string_view extra = {}) -> void {
            out << '<' << node.name;
            for (const auto& attribute : node.attributes)
                out << ' ' << attribute.name << "=\"" << escape(attribute.value) << '\"';
            out << '>' << escape(node.raw_text);
            for (const auto& child : node.children) self(self, out, child, {});
            out << extra << "</" << node.name << ">\n";
        };
        for (const auto& file : std::set<std::string>{selected->file, starting_base->file}) {
            if (!file.starts_with("data/")) throw std::runtime_error("production fixture source is outside data");
            const auto path = directory / std::filesystem::path(file.substr(5));
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path);
            out << "<Types>\n";
            for (const auto& entry : originals.catalog.definitions()) {
                if (!entry.active || !entry.namespace_winner || entry.category != eawr::data::Category::game_object
                    || entry.root.source.logical_path != file) continue;
                std::string_view extra;
                if (entry.id == selected->name) extra =
                    "<Tech_Level>-2</Tech_Level><Required_Star_Base_Level>99</Required_Star_Base_Level>"
                    "<Base_Level>99</Base_Level><Build_Advances_Tech_Level>-1</Build_Advances_Tech_Level>";
                if (entry.id == starting_base->name) extra =
                    "<Base_Level>99</Base_Level><Required_Star_Base_Level>1</Required_Star_Base_Level>"
                    "<Build_Cost_Credits>100</Build_Cost_Credits>";
                emit(emit, out, entry.root, extra);
            }
            out << "</Types>\n";
            if (!out) throw std::runtime_error("production guard overlay write failed");
        }
    }
    mounts.insert(mounts.begin(), {"hero-guard-fixture", directory, "data", {}});
    const auto files = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(files, eawr::data::Profile::foc).value();
    auto data = eawr::data::galactic::load_galactic_data(files, catalog.catalog).value();
    for (auto& planet : data.planets) planet.planet_destroyed_credit_value = Fixed::from_integer(-1).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    if (production_guard) {
        const auto* guarded = types.find(guarded_initial_base);
        if (!guarded || guarded->base_level != 99 || guarded->required_star_base_level != 1
            || guarded->build_cost_credits != Fixed::from_integer(100).value())
            throw std::runtime_error("starting base overlay did not retain its identity and authored values");
    }
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == "Rebel";
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    for (const auto& type : types.types) options.types.push_back({options.types.size() + 1, type.name});
    const auto plan = cp::plan_live_campaign_setup(files, data, types, "Sandbox_Equal_Footing_Rebel", options, "Normal_Default").value();
    const auto services = cp::prepare_live_campaign_services(files, catalog.catalog, data, types, plan, "Normal_Default").value();
    auto created = cp::CampaignRoot::create(plan, services);
    if (!created) throw std::runtime_error(eawr::core::format_diagnostic(created.error()));
    auto campaign = std::move(created).value();
    const auto notes = campaign.load_notes();
    if (production_guard) {
        for (const auto tag : {"Tech_Level", "Required_Star_Base_Level", "Base_Level"})
            expect(std::any_of(notes.begin(), notes.end(), [&](const auto& note) {
                return note.code == "EAWR-LOAD-1001" && note.tag == tag
                    && note.classification == eawr::core::LoadCategory::retail_accepts
                    && note.layer.mod_id == "hero-guard-fixture" && note.line.has_value()
                    && !note.file.empty() && !note.authored_value.empty();
            }), "G1 unusual XML levels load with B notes carrying the winning mod source");
        expect(std::any_of(notes.begin(), notes.end(), [](const auto& note) {
            return note.code == "EAWR-LOAD-1001" && note.tag == "Required_Star_Base_Level"
                && note.authored_value == "99" && note.effective_value == "99";
        }), "G1 requirement level 99 is retained through live preparation");
        expect(std::any_of(notes.begin(), notes.end(), [](const auto& note) {
            return note.code == "EAWR-LOAD-1035" && note.action == eawr::core::LoadAction::skipped_placement;
        }), "unsupported starting base emits C only when its placement is actually skipped");
        for (const auto& note : notes) if (note.code == "EAWR-LOAD-1035")
            expect(std::any_of(plan.forces().begin(), plan.forces().end(), [&](const auto& force) {
                const auto* original_type = plan.original_type(force.type);
                return original_type && original_type->name == note.object;
            }), "unused registered base prototypes do not emit a skipped-placement note");
        std::size_t intact_bases{};
        const auto base_binding = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(),
            [&](const auto& binding) { return binding.original_type == guarded_initial_base; });
        if (base_binding == plan.type_bindings().end()) throw std::runtime_error("guarded initial base identity is missing");
        for (const auto& producer : campaign.game()->galaxy().setup().production->planets)
            for (const auto& offer : producer.offers) if (offer.type == base_binding->id) {
                ++intact_bases;
                expect(offer.kind == gc::ProductionKind::starbase && offer.base_level == 99
                    && offer.required_space_level == 1 && offer.price == Fixed::from_integer(100).value()
                    && offer.direct_build_allowed, "skipping an initial base leaves its authored production offer intact");
            }
        expect(intact_bases > 0, "starting-base fixture exercises actual retained production offers");
    }
    for (const auto code : {"EAWR-LOAD-1230", "EAWR-LOAD-1231", "EAWR-LOAD-1232"})
        expect(std::any_of(notes.begin(), notes.end(), [&](const auto& note) {
            return note.code == code && note.classification == eawr::core::LoadCategory::unknown_degrade;
        }), "GC root snapshot includes every hero guard family");
    const auto& abilities = *campaign.game()->galaxy().setup().ability_host;
    expect(abilities.ruined_income.empty(), "invalid ruined income retains ordinary income");
    bool theft_seen{};
    for (const auto& profile : abilities.profiles) {
        expect(profile.respawn_structure == 0, "unresolved hero structure dependency cleared");
        if (profile.theft) { theft_seen = true; expect(profile.theft->absence_min.empty() && profile.theft->absence_max.empty(), "malformed theft arrays clear both"); }
    }
    expect(theft_seen, "installed hero fixture exercised theft parsing");
    const auto snapshot_again = services.load_notes();
    const auto expected_snapshot = eawr::core::load_notes_json(notes);
    const auto expected_provider = production_guard ? eawr::core::load_notes_json(services.load_notes()) : expected_snapshot;
    expect(eawr::core::load_notes_json(snapshot_again) == expected_provider,
        "GC provider snapshot merge is deterministic and idempotent");
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto session = gc::GalacticSession::create(campaign.game()->galaxy().setup()).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers); std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 3; ++tick) hashes.push_back(session.step(executor).value().state_hash.get());
        if (workers == 1) baseline = hashes; else expect(hashes == baseline, "degraded hero campaign continues deterministically");
    }
    std::vector<eawr::core::LoadNote> published;
    eawr::platform::LiveGameSession::Options live;
    live.load_note_sink = [&](std::span<const eawr::core::LoadNote> rows) { published.assign(rows.begin(), rows.end()); };
    const auto started = eawr::platform::LiveGameSession::start(std::move(campaign), std::move(live));
    expect(static_cast<bool>(started) && eawr::core::load_notes_json(published) == expected_snapshot,
        "session start hands GC snapshot to the shared load-note sink");
}
// LSA-17/18: temporary XML overlays exercise the live readers and full campaign root.
void installed_retail_production_numbers() {
    using Fixed = eawr::sim::math::Fixed;
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: retail production numbers require EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data").value().mount);
    const auto base = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(base, eawr::data::Profile::foc).value();
    const auto text = [&](std::string_view path) {
        const auto bytes = base.open(path).value();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    const auto constants = text("data/xml/gameconstants.xml");
    const auto difficulty = text("data/xml/difficultyadjustments.xml");
    std::map<std::string, std::string> faction_files;
    for (const auto& definition : catalog.catalog.definitions()) if (definition.category == eawr::data::Category::faction
        && definition.active && !faction_files.contains(definition.root.source.logical_path))
        faction_files.emplace(definition.root.source.logical_path, text(definition.root.source.logical_path));
    // CTest's working directory is the build tree; retail and mod folders are read-only inputs.
    const auto directory = std::filesystem::current_path() / ("retail-production-numbers-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
    const auto write = [&](std::string_view logical, const std::string& xml) {
        if (!logical.starts_with("data/")) throw std::runtime_error("numeric overlay requires a canonical data path");
        const auto path = directory / std::filesystem::path(logical.substr(5));
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path); out << xml;
        if (!out) throw std::runtime_error("numeric fixture overlay write failed");
    };
    const auto replace = [](std::string xml, const std::string& tag, const std::string& value) {
        const std::regex pattern("<" + tag + "\\b[^>]*>[^<]*</" + tag + "\\s*>", std::regex::icase);
        return std::regex_replace(xml, pattern, "<" + tag + ">" + value + "</" + tag + ">");
    };
    const auto load = [&](unsigned family, const std::string& value, bool omit_benefits = false) {
        auto changed = replace(constants, "Production_Speed_Factor", family == 0 ? value : "1");
        for (unsigned i = 0; i < 5; ++i)
            changed = replace(std::move(changed), "Production_Speed_Mod_Base_Vs_Tech_" + std::to_string(i), family == 1 ? value : "1");
        write("data/xml/gameconstants.xml", changed);
        write("data/xml/difficultyadjustments.xml", replace(difficulty, "Galactic_Build_Time_Multiplier", family == 2 ? value : "1"));
        for (const auto& [path, original] : faction_files) {
            auto faction = replace(original, "Corruption_Galactic_Production_Price_Multipliers", (family == 3 ? value : "1") + " 0.5");
            faction = replace(std::move(faction), "Corruption_Galactic_Production_Time_Multipliers", (family == 3 ? value : "1") + " 0.5");
            if (omit_benefits) faction = std::regex_replace(faction,
                std::regex("<Benefits_From_Corruption\\b[^>]*>[^<]*</Benefits_From_Corruption\\s*>", std::regex::icase), "");
            write(path, faction);
        }
        auto layered = mounts;
        layered.insert(layered.begin(), {"numeric-fixture", directory, "data", {}});
        const auto fs = eawr::vfs::Vfs::mount(layered).value();
        const auto loaded = eawr::data::load_catalog(fs, eawr::data::Profile::foc).value();
        const auto data = eawr::data::galactic::load_galactic_data(fs, loaded.catalog).value();
        const auto types = eawr::units::galactic_type_table(loaded.catalog);
        cp::CampaignSetupOptions options; options.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            options.players.push_back({{id, id, 0}, faction.name, true});
            const bool human = faction.name == "Rebel";
            if (human) options.solo_reference_human = id;
            if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
                human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
        }
        std::vector<std::string> names;
        for (const auto& type : types.types) names.push_back(type.name);
        std::sort(names.begin(), names.end());
        for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
        const auto plan = cp::plan_live_campaign_setup(fs, data, types, "Sandbox_Equal_Footing_Rebel", options, "Normal_Default").value();
        auto services = cp::prepare_live_campaign_services(fs, loaded.catalog, data, types, plan, "Normal_Default");
        if (!services) throw std::runtime_error("retail numbers family=" + std::to_string(family) + " value=" + value + ": " + services.error().message);
        auto campaign = cp::CampaignRoot::create(plan, services.value());
        if (!campaign) throw std::runtime_error("retail numbers family=" + std::to_string(family) + " value=" + value + ": " + campaign.error().message);
        if (omit_benefits) {
            for (const auto& binding : plan.player_bindings()) {
                const auto* faction = data.faction(binding.faction);
                expect(faction && !faction->benefits_from_corruption.has_value(), "WGS-94 omission reaches the real XML loader");
                const auto prefix = binding.faction + ": Benefits_From_Corruption";
                expect(std::count_if(services.value().load_diagnostics.begin(), services.value().load_diagnostics.end(), [&](const auto& note) {
                    return note.code == "campaign_omitted_faction_tag" && note.severity == eawr::core::Severity::warning
                        && note.message.starts_with(prefix) && note.message.find("false constructor default (WGS-94)") != std::string::npos;
                }) == 1, "WGS-94 real XML omission emits exactly one existing load note per bound faction");
            }
        }
        return std::pair{campaign.value().game()->galaxy().setup(), plan};
    };
    const auto baseline = load(4, "1");
    expect(baseline.first.production.has_value() && baseline.first.economy_corruption.has_value(), "numeric fixture baseline reaches full production and corruption campaign setup");
    const auto omitted = load(4, "1", true);
    expect(omitted.first.economy_corruption && std::all_of(omitted.first.economy_corruption->players.begin(), omitted.first.economy_corruption->players.end(),
        [](const auto& player) { return !player.benefits; }), "WGS-94 real XML omission defaults the raw economy catalog flag false for playable factions too");
    expect(omitted.first.movement && std::all_of(omitted.first.movement->players.begin(), omitted.first.movement->players.end(),
        [](const auto& player) { return !player.benefits_from_corruption; }), "WGS-94 real XML omission defaults movement eligibility false");
    const auto missing = cp::prepare_corruption_economy(base, eawr::data::Catalog{}, baseline.second, baseline.first);
    expect(missing && missing.value().players.size() == baseline.first.players.size(),
        "G3 a missing catalog faction retains an inert economy player");
    auto missing_data = eawr::data::galactic::load_galactic_data(base, catalog.catalog).value();
    std::erase_if(missing_data.factions, [](const auto& faction) { return faction.name == "Rebel"; });
    const auto missing_services = cp::prepare_live_campaign_services(base, catalog.catalog, missing_data,
        eawr::units::galactic_type_table(catalog.catalog), baseline.second, "Normal_Default");
    expect(!missing_services, "WGS-94 a missing faction record still refuses full live setup");
    for (unsigned family = 0; family < 4; ++family) for (const auto value : {"-1.5", "0", "text", "2.7"}) {
        const auto parsed = std::string_view(value) == "text" ? Fixed{} : Fixed::from_decimal(value).value();
        const auto [setup, plan] = load(family, value);
        expect(setup.production.has_value() && setup.economy_corruption.has_value(), "every signed/zero/text/fraction XML fixture reaches campaign load");
        std::size_t checked{};
        for (std::size_t i = 0; i < setup.production->planets.size(); ++i) {
            const auto& planet = setup.production->planets[i];
            const auto& before = baseline.first.production->planets[i];
            const bool human = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& seat) {
                return seat.player == planet.target_owner && seat.kind == gc::SeatKind::human;
            });
            const auto corruption = std::find_if(setup.economy_corruption->players.begin(), setup.economy_corruption->players.end(),
                [&](const auto& row) { return row.player == planet.target_owner; });
            for (std::size_t j = 0; j < planet.offers.size(); ++j) {
                const auto& offer = planet.offers[j]; const auto& old = before.offers[j];
                const auto* original = plan.original_type(offer.type);
                if (!original || old.seconds.raw() <= 0 || original->build_time_reduced_by_multiple_factories.value_or(false)) continue;
                bool applies = family == 0;
                if (family == 1) applies = offer.domain == gc::ProductionDomain::space && planet.space_level > 0
                    && planet.space_level - original->tech_level.value_or(0) - 1 >= 0;
                if (family == 2) applies = !human;
                if (family == 3) applies = corruption != setup.economy_corruption->players.end() && !corruption->benefits && offer.affiliated;
                if (!applies) continue;
                ++checked;
                expect(offer.seconds == gc::production_product(old.seconds, parsed), "XML production factor retains its retail signed/zero/text/fraction value in the loaded offer");
                if (family == 3) expect(offer.price == gc::production_price(gc::production_product(old.price, parsed)),
                    "XML corruption price list first token reaches signed offer pricing");
            }
        }
        expect(checked > 0, "every production factor family has an observable loaded offer");
        if (family == 3) {
            std::size_t vectors{};
            for (const auto& player : setup.economy_corruption->players) if (player.playable && !player.benefits) {
                ++vectors;
                expect(player.price[0] == parsed && player.time[0] == parsed
                    && player.price[1] == Fixed::from_ratio(1, 2).value() && player.time[1] == Fixed::from_ratio(1, 2).value(),
                    "planet economy loader retains both XML float-sequence tokens");
            }
            expect(vectors > 0, "corruption-vector XML case checks real playable faction rows");
        }
        std::cout << "retail production family=" << family << " value=" << value << " offers=" << checked << " loaded=yes\n";
    }
    // LSA-02's economy consumer must not reintroduce a whole-number gate after the data loader.
    // Use the baseline plan to isolate this reread from the separate faction-data parser contract.
    for (const auto& [path, original] : faction_files)
        write(path, replace(original, "Displayed_Tech_Level_Adjustment", "-2.7"));
    auto layered = mounts; layered.insert(layered.begin(), {"numeric-fixture", directory, "data", {}});
    const auto fs = eawr::vfs::Vfs::mount(layered).value();
    const auto modified = eawr::data::load_catalog(fs, eawr::data::Profile::foc).value();
    const auto economy = cp::prepare_corruption_economy(fs, modified.catalog, baseline.second, baseline.first).value();
    std::size_t adjustments{};
    for (const auto& binding : baseline.second.player_bindings()) {
        const auto* faction = modified.catalog.find(binding.faction, eawr::data::Category::faction);
        if (!faction || std::none_of(faction->root.children.begin(), faction->root.children.end(), [](const auto& node) {
            return node.name == "Displayed_Tech_Level_Adjustment";
        })) continue;
        const auto player = std::find_if(economy.players.begin(), economy.players.end(), [&](const auto& row) { return row.player == binding.player.player_id; });
        expect(player != economy.players.end() && player->displayed_tech_adjustment == -2,
            "fractional faction adjustment reread uses atoi truncation instead of rejecting real XML");
        ++adjustments;
    }
    expect(adjustments > 0, "fractional faction-adjustment fixture checks at least one authored real-loader row");
    // AU-80/81 ignore placeholders, retaining the same fresh false default as an omitted tag.
    for (const auto& [path, original] : faction_files)
        write(path, replace(original, "Benefits_From_Corruption", "TBD"));
    const auto placeholder_fs = eawr::vfs::Vfs::mount(layered).value();
    const auto placeholder_catalog = eawr::data::load_catalog(placeholder_fs, eawr::data::Profile::foc).value();
    const auto placeholder_economy = cp::prepare_corruption_economy(placeholder_fs, placeholder_catalog.catalog, baseline.second, baseline.first);
    expect(placeholder_economy && std::all_of(placeholder_economy.value().players.begin(), placeholder_economy.value().players.end(),
        [](const auto& player) { return !player.benefits; }), "AU-80/81 a placeholder retains the same false omission default");
}
// RGC-80/81: synthetic authored list using installed constructor inputs, without changing game files.
void unresolved_location() {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: unresolved location requires EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data").value().mount);
    const auto files = eawr::vfs::Vfs::mount(mounts).value();
    const auto loaded = eawr::data::load_catalog(files, eawr::data::Profile::foc).value();
    auto data = eawr::data::galactic::load_galactic_data(files, loaded.catalog).value();
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    auto source = *data.campaign("Sandbox_Equal_Footing_Rebel");
    const auto ship = std::find_if(source.starting_forces.begin(), source.starting_forces.end(), [&](const auto& record) {
        const auto* type = types.find(record.type);
        return record.faction == source.starting_active_player && type
            && std::find(type->behavior.begin(), type->behavior.end(), "DUMMY_STARSHIP") != type->behavior.end();
    });
    if (source.planets.size() < 2 || ship == source.starting_forces.end()) throw std::runtime_error("location fixture needs two planets and a ship");
    const auto force = *ship;
    const auto a = source.planets.front(), b = source.planets.back();
    const std::string missing = "Undefined_Campaign_Location";
    expect(!loaded.catalog.find(missing, eawr::data::Category::game_object), "fixture location is undefined");
    source.planets = {a, b}; source.locations = {a, missing, b}; source.trade_routes.clear();
    source.starting_forces = {{force.faction, a, force.type}, {force.faction, missing, force.type}, {force.faction, b, force.type}};
    source.special_case_production = {{force.faction, missing, "Undefined_Force_Type"}};
    source.home_locations = {{force.faction, a}};
    data.campaigns = {source};
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source.starting_active_player;
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    std::vector<std::string> names;
    for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto plan = cp::plan_live_campaign_setup(files, data, types, source.name, options, "Normal_Default").value();
        expect(plan.locations().size() == 3 && plan.locations()[0].planet && !plan.locations()[1].planet
            && plan.locations()[2].planet && plan.galaxy().planets.size() == 2, "missing location retains its slot without a planet");
        expect(plan.forces().size() == 2 && plan.initial_placements().empty(), "missing location receives neither forces nor special placement");
        const auto services = cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default").value();
        const auto count = std::count_if(services.load_diagnostics.begin(), services.load_diagnostics.end(), [&](const auto& note) {
            return note.code == "campaign_unresolved_location" && note.severity == eawr::core::Severity::warning
                && note.message.find(missing) != std::string::npos;
        });
        expect(count == 1, "undefined location emits one load note naming the skipped entry");
        auto created = cp::CampaignRoot::create(plan, services);
        if (!created) throw std::runtime_error(eawr::core::format_diagnostic(created.error()));
        auto campaign = std::move(created).value();
        const auto setup = campaign.game()->galaxy().setup();
        expect(setup.planets.size() == 2 && setup.fleets.size() == 2, "both valid locations survive with their starting fleets");
        for (const auto& location : plan.locations()) if (location.planet)
            expect(std::count_if(setup.fleets.begin(), setup.fleets.end(), [&](const auto& fleet) {
                return fleet.orbit == location.planet->id && fleet.members.size() == 1;
            }) == 1, "one authored ship placed at each valid location before and after the missing entry");
        auto session = gc::GalacticSession::create(setup).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 3; ++tick) hashes.push_back(session.step(executor).value().state_hash.get());
        if (workers == 1) baseline = hashes;
        else expect(hashes == baseline, "unresolved-location campaign is deterministic on 1/2/4/8 workers");
    }
}
// An omitted route control value uses the retail constructor default.
void omitted_route_control() {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: route control requires EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data").value().mount);
    const auto files = eawr::vfs::Vfs::mount(mounts).value();
    const auto loaded = eawr::data::load_catalog(files, eawr::data::Profile::foc).value();
    auto data = eawr::data::galactic::load_galactic_data(files, loaded.catalog).value();
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    const auto source = *data.campaign("Sandbox_Equal_Footing_Rebel");
    if (source.trade_routes.empty()) throw std::runtime_error("route control fixture requires a route");
    const auto route_name = source.trade_routes.front();
    const auto route = std::find_if(data.trade_routes.begin(), data.trade_routes.end(), [&](const auto& row) { return row.name == route_name; });
    if (route == data.trade_routes.end()) throw std::runtime_error("route control fixture route unavailable");
    route->political_control_gain.reset();
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source.starting_active_player;
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    std::vector<std::string> names;
    for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    const auto plan = cp::plan_live_campaign_setup(files, data, types, source.name, options, "Normal_Default").value();
    const auto services = cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default").value();
    expect(std::any_of(services.load_diagnostics.begin(), services.load_diagnostics.end(), [&](const auto& note) {
        return note.code == "campaign_route_control_default" && note.message.find(route_name) != std::string::npos;
    }), "omitted route gain emits a warning");
    const auto campaign = cp::CampaignRoot::create(plan, services).value();
    const auto& routes = campaign.game()->galaxy().setup().control->routes;
    const auto index = std::find(plan.route_names().begin(), plan.route_names().end(), route_name) - plan.route_names().begin();
    expect(index < static_cast<std::ptrdiff_t>(plan.galaxy().routes.size())
        && std::any_of(routes.begin(), routes.end(), [&](const auto& item) {
            return item.route == plan.galaxy().routes[static_cast<std::size_t>(index)].id && item.political_gain == eawr::sim::math::Fixed{};
        }), "omitted route gain loads as zero");
}
} // namespace
int main(int argc, const char* const argv[]) {
#if defined(_MSC_VER)
    expect(_putenv_s("EAWR_VERIFY_GALACTIC_HASH", "1") == 0, "installed galactic hash verifier enabled");
#else
    expect(setenv("EAWR_VERIFY_GALACTIC_HASH", "1", 1) == 0, "installed galactic hash verifier enabled");
#endif
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--mp-heroes-tarkin"
                || std::string_view(argv[1]) == "--mp-hero-protection-live"
                || std::string_view(argv[1]) == "--authored-protection-source"
                || std::string_view(argv[1]) == "--solo-protection-sources"
                || std::string_view(argv[1]) == "--all-mp-protection-sources")) installed(false, false, argv[1]);
        else if (argc == 2 && std::string_view(argv[1]) == "--guard-load-notes") installed_guard_load_notes();
        else if (argc == 2 && std::string_view(argv[1]) == "--production-load-notes") installed_guard_load_notes(true);
        else if (argc == 2 && std::string_view(argv[1]) == "--unresolved-location") { unresolved_location(); omitted_route_control(); }
        else if (argc == 2 && std::string_view(argv[1]) == "--retail-production-numbers") installed_retail_production_numbers();
        else if (argc == 2 && std::string_view(argv[1]) == "--origin-starbases") installed_origin_starbases();
        else if (argc == 2 && std::string_view(argv[1]) == "--starting-tech") installed_starting_levels();
        else if (argc == 2 && std::string_view(argv[1]) == "--accepted-starting-tech") installed_starting_levels(true);
        else if (argc == 2 && std::string_view(argv[1]) == "--unscouted-intel") installed_unscouted_intel();
        else installed(argc == 2 && std::string_view(argv[1]) == "--production-domains",
            argc == 2 && std::string_view(argv[1]) == "--structure-slots");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

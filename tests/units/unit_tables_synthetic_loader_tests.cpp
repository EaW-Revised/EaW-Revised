#include "unit_tables_support.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/units/content_selection.hpp"
#include "unit_tables_synthetic_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace unit_tables_test_support {
void synthetic_tables() {
    // AB-70: XML-authored ion filters survive both ordinary and Create_Team_Type binding.
    for (const std::string_view style : {"User_Input", "Space_Automatic", "Ground_Automatic", "Special_Attack", ""}) {
        for (const std::string_view category : {"Frigate", "Fighter", ""}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const std::string binding =
                "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>ION_CANNON_SHOT</Type>"
                "<GUI_Activated_Ability_Name>Mixed_Case_Ion</GUI_Activated_Ability_Name>"
                "<Supports_Autofire>True</Supports_Autofire></Unit_Ability></Unit_Abilities_Data>"
                "<Abilities SubObjectList=\"Yes\"><Ion_Cannon_Shot_Attack_Ability Name=\"Mixed_Case_Ion\">"
                + (style.empty() ? std::string{} : "<Activation_Style>" + std::string(style) + "</Activation_Style>")
                + (category.empty() ? std::string{} : "<Applicable_Unit_Categories>" + std::string(category)
                    + "</Applicable_Unit_Categories>")
                + "<Applicable_Unit_Types>Test_Fighter</Applicable_Unit_Types>"
                "<Excluded_Unit_Categories>Bomber</Excluded_Unit_Categories>"
                "<Excluded_Unit_Types>Test_Frigate</Excluded_Unit_Types>"
                "</Ion_Cannon_Shot_Attack_Ability></Abilities>";
            const auto frigate = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
            const auto ordinary = xml.find("<Unit_Abilities_Data", frigate);
            const auto end = xml.find("</Unit_Abilities_Data>", ordinary);
            expect(ordinary != std::string::npos && end != std::string::npos, "AB-70: replace the original ordinary row");
            if (ordinary != std::string::npos && end != std::string::npos)
                xml.erase(ordinary, end + std::string_view{"</Unit_Abilities_Data>"}.size() - ordinary);
            for (const std::string_view start : {"<Container Name=\"Test_Pair_Base_Container\">",
                    "<SpaceUnit Name=\"Test_Frigate\">"}) {
                const auto at = xml.find(start);
                expect(at != std::string::npos, "AB-70: fixture insertion point exists");
                if (at != std::string::npos) xml.insert(at + start.size(), binding);
            }
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F));
            expect(loaded.tables.has_value(), "AB-70: authored nested ion XML loads");
            if (!loaded.tables) continue;
            const auto compiled = eawr::units::ability_table(*loaded.tables);
            expect(static_cast<bool>(compiled), "AB-70: nested ion profiles compile");
            if (!compiled) continue;
            for (const std::string_view id : {"Test_Frigate", "Test_Squadron_Pair"}) {
                const auto* profile = compiled.value().find(eawr::assets::object_type_crc(id));
                const auto slot = profile ? eawr::sim::tactical::ability_slot(*profile,
                    eawr::sim::tactical::AbilityKind::ion_cannon_shot) : std::nullopt;
                expect(profile && slot && profile->special.size() == 1, "AB-70: team and unit retain a named ion handler");
                if (!profile || !slot || profile->special.size() != 1) continue;
                const auto& handler = profile->special.front();
                const std::uint64_t mask = category == "Frigate" ? 0x10U : category == "Fighter" ? 1U : 0U;
                expect(profile->abilities[*slot].gui_activated_ability_name == "Mixed_Case_Ion"
                    && handler.name == "Mixed_Case_Ion"
                    && handler.kind == eawr::sim::tactical::special_ability_kind("Ion_Cannon_Shot_Attack_Ability")
                    && handler.style == eawr::sim::tactical::special_activation_style(style.empty() ? "Special_Attack" : style)
                    && handler.filter.applicable_categories == mask
                    && handler.filter.applicable_types == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Fighter")}
                    && handler.filter.excluded_categories == 2
                    && handler.filter.excluded_types == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Frigate")},
                    "AB-70: authored style, default empty categories, names and exact filters survive the team loader");
            }
        }
    }
    for (const std::string_view starting : {"", "<Starting_Spawned_Units_Tech_0></Starting_Spawned_Units_Tech_0>"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string tag = "<Starting_Spawned_Units_Tech_0>Test_Squadron, 2</Starting_Spawned_Units_Tech_0>";
        const auto at = xml.find(tag);
        const auto base = xml.find("<StarBase Name=\"Test_Base\">");
        if (at != std::string::npos) xml.replace(at, tag.size(), starting);
        if (base != std::string::npos) xml.insert(base + std::string_view{"<StarBase Name=\"Test_Base\">"}.size(),
            "<Behavior>DUMMY_STAR_BASE</Behavior>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) {
            expect(false, "WSL-26: empty authored station hangar loads");
            continue;
        }
        const auto* station = loaded.tables->find("Skirmish_Test_Base");
        const auto motion = eawr::units::motion_table(*loaded.tables);
        const auto id = eawr::assets::object_type_crc("Skirmish_Test_Base");
        bool empty = at != std::string::npos && base != std::string::npos && station && station->spawner
            && station->spawner->starting.empty() && motion;
        for (unsigned tech = 0; tech < 6 && empty; ++tech) {
            const auto* profile = motion.value().squadrons.find_spawner_at_tech(id, 1, tech);
            empty = profile && profile->starbase && !profile->bays.empty() && profile->entries.empty();
        }
        expect(empty, "WSL-26: omitted or empty XML starting garrison binds an empty station hangar at every tech");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string tag = "<Starting_Spawned_Units_Tech_0>Test_Squadron, 2</Starting_Spawned_Units_Tech_0>";
        const auto at = xml.find(tag);
        if (at != std::string::npos) xml.insert(at + tag.size(),
            "<Starting_Spawned_Units_Tech_3>Test_Squadron_Pair, 4</Starting_Spawned_Units_Tech_3>"
            "<Reserve_Spawned_Units_Tech_2>Test_Squadron_Pair, 7</Reserve_Spawned_Units_Tech_2>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* station = loaded.tables ? loaded.tables->find("Skirmish_Test_Base") : nullptr;
        expect(at != std::string::npos && station && station->spawner
            && station->spawner->campaign_starting[3].size() == 1
            && station->spawner->campaign_starting[3][0].squadron == "Test_Squadron_Pair"
            && station->spawner->campaign_starting[3][0].count == 4
            && station->spawner->campaign_reserves[2].size() == 1
            && station->spawner->campaign_reserves[2][0].count == 7,
            "WSL-11: skirmish loading retains higher starting and reserve tiers independently");
        if (loaded.tables) {
            const auto motion = eawr::units::motion_table(*loaded.tables);
            const auto id = eawr::assets::object_type_crc("Skirmish_Test_Base");
            const auto* low = motion ? motion.value().squadrons.find_spawner_at_tech(id, 1, 1) : nullptr;
            const auto* high = motion ? motion.value().squadrons.find_spawner_at_tech(id, 1, 4) : nullptr;
            expect(low && low->entries.size() == 1 && low->entries[0].starting == 2
                && low->entries[0].reserve == -1,
                "WSL-11: tech 1 falls back to both tech 0 rows");
            expect(high && high->entries.size() == 1 && high->entries[0].squadron == eawr::assets::object_type_crc("Test_Squadron_Pair")
                && high->entries[0].starting == 4 && high->entries[0].reserve == 7,
                "WSL-11: tech 4 selects starting tier 3 and reserve tier 2 independently");
        }
    }
    {
        // A mobile carrier with no Tech_0 row must still bind and launch its Tech_1 complement.
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto replace = [&](const std::string& from, const std::string& to) {
            const auto at = xml.find(from);
            if (at == std::string::npos) return false;
            xml.replace(at, from.size(), to);
            return true;
        };
        const bool authored = replace("<Space_Model_Name>TEST_FRIGATE.ALO</Space_Model_Name>",
                "<Space_Model_Name>Test_Station.alo</Space_Model_Name>")
            && replace("<SpaceBehavior>TARGETING,REVEAL , HIDE_WHEN_FOGGED, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>",
                "<SpaceBehavior>SPAWN_SQUADRON, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>"
                "<Starting_Spawned_Units_Tech_1>Test_Squadron, 3</Starting_Spawned_Units_Tech_1>"
                "<Starting_Spawned_Units_Tech_1>Test_Squadron_Pair, 2</Starting_Spawned_Units_Tech_1>"
                "<Spawned_Squadron_Delay_Seconds>0</Spawned_Squadron_Delay_Seconds>")
            && replace("<HardPoints> HP_Test_Laser, HP_Test_Engine, HP_Test_Missing </HardPoints>",
                "<HardPoints>HP_Test_Bay</HardPoints>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* carrier = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        const auto id = eawr::assets::object_type_crc("Test_Frigate");
        const auto fighter = eawr::assets::object_type_crc("Test_Squadron");
        const auto bomber = eawr::assets::object_type_crc("Test_Squadron_Pair");
        expect(authored && carrier && carrier->spawner && carrier->spawner->starting.empty()
            && carrier->spawner->campaign_starting[1].size() == 2,
            "WSL-11: mobile carrier loads Tech_1-only starting rows");
        if (loaded.tables) {
            const auto motion = eawr::units::motion_table(*loaded.tables);
            const auto* profile = motion ? motion.value().squadrons.find_spawner_at_tech(id, 1, 1) : nullptr;
            expect(profile && profile->mobile && !profile->bays.empty() && profile->entries.size() == 2
                && profile->entries[0].squadron == fighter && profile->entries[0].starting == 3
                && profile->entries[1].squadron == bomber && profile->entries[1].starting == 2,
                "WSL-11: Tech_1-only carrier binds its complete mobile spawner");
            const std::array<unsigned, 2> higher_techs{2, 3};
            expect(std::all_of(higher_techs.begin(), higher_techs.end(), [&](const unsigned tech) {
                const auto* higher = motion ? motion.value().squadrons.find_spawner_at_tech(id, 1, tech) : nullptr;
                return higher && higher->mobile && !higher->bays.empty() && higher->entries.size() == 2
                    && higher->entries[0].squadron == fighter && higher->entries[0].starting == 3
                    && higher->entries[1].squadron == bomber && higher->entries[1].starting == 2;
            }), "WSL-11: tech 2 and 3 fall back to the complete Tech_1-only mobile carrier complement");
            if (profile && profile->entries.size() == 2) {
                namespace tactical = eawr::sim::tactical;
                tactical::TacticalSetup setup;
                setup.seed = 540;
                setup.players = {{1, 0, 100, 1}, {2, 1, 200, 1}};
                setup.units = {{1, id, 1, {}, eawr::sim::math::identity_quat(), {}}};
                tactical::EconomyRules economy;
                economy.players = {{1, {}, 25, false, {}, 1, 5}, {2, {}, 25, false, {}, 1, 5}};
                auto session = tactical::TacticalSession::create(setup, {}, {}, motion.value(), std::nullopt, {}, {}, {}, economy);
                expect(static_cast<bool>(session), "WSL-11: Tech_1-only carrier session creates");
                if (session) {
                    const eawr::sim::InlineExecutor executor;
                    bool stepped = true;
                    for (unsigned tick = 0; tick < 180 && stepped; ++tick)
                        stepped = static_cast<bool>(session.value().step(executor));
                    unsigned fighters = 0, bombers = 0;
                    for (const auto& squadron : session.value().squadrons()) {
                        const auto state = session.value().squadron_state(squadron.container);
                        if (!state || state->spawner != 1) continue;
                        fighters += state->squadron_type == fighter ? 1U : 0U;
                        bombers += state->squadron_type == bomber ? 1U : 0U;
                    }
                    expect(stepped && fighters == 3 && bombers == 2,
                        "WSL-11: Tech_1-only carrier launches three and two starting squadrons");
                }
            }
        }
    }
    // HD-06: omission, zero and malformed Health retain every authored hardpoint.
    for (const std::string_view value : {"", "<Health></Health>", "<Health>   </Health>",
            "<Health>0</Health>", "<Health>-1</Health>",
            "<Health>1e99</Health>", "<Health>broken</Health>", "<Health>42junk</Health>",
            // The Remake Viscount shield hardpoints: an invisible U+202C before ".0" (AU-82: leading number kept).
            "<Health>14500\xE2\x80\xAC.0</Health>",
            // A fractional leading number keeps its fraction (not truncated to "0" / "14500").
            "<Health>0.5x</Health>", "<Health>14500.5x</Health>"}) {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        auto points = std::string(hardpoints_xml);
        const std::string_view laser_health = "<Health>260.0</Health>";
        const std::string_view engine_health = "<Health>170.0</Health>";
        points.replace(points.find(laser_health), laser_health.size(), value);
        points.replace(points.find(engine_health), engine_health.size(), value);
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "HD-06: hardpoint Health fixture loads");
        if (!loaded.tables) continue;
        if (value.empty()) {
            auto missing = *loaded.tables;
            auto& scalars = missing.constants.scalars;
            scalars.erase(std::remove_if(scalars.begin(), scalars.end(), [](const auto& scalar) {
                return scalar.tag == "ShieldRechargeIntervalInSecs";
            }), scalars.end());
            eawr::core::LoadNoteBuffer load_notes;
            auto recovered = eawr::units::durability_table(missing, nullptr, &load_notes);
            eawr::core::LoadNoteCollector collector; collector.merge(load_notes);
            const auto captured = collector.snapshot();
            expect(recovered && recovered.value().damage && recovered.value().damage->shield_recharge_frames == 90,
                "missing shield recharge interval uses stock three seconds");
            expect(std::any_of(captured.begin(), captured.end(), [](const auto& note) {
                return note.code == "EAWR-LOAD-0120" && note.tag == "ShieldRechargeIntervalInSecs"
                    && note.action == eawr::core::LoadAction::default_used
                    && note.classification == eawr::core::LoadCategory::retail_accepts && note.retail == "LSA-P5";
            }), "missing shield recharge interval records a default note");
            if (recovered) {
                eawr::sim::tactical::TacticalSetup setup;
                setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                    {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
                auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, recovered.value());
                expect(static_cast<bool>(session), "stock fallback session creates");
                if (session) {
                    const eawr::sim::InlineExecutor executor;
                    expect(static_cast<bool>(session.value().step(executor)), "stock fallback session steps");
                }
            }
            auto extreme = *loaded.tables;
            for (auto& unit : extreme.units) {
                if (unit.hull && unit.hull->raw() > 0) {
                    unit.hull = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
                    break;
                }
            }
            for (auto& constant : extreme.constants.scalars) {
                if (constant.tag == "ShieldRechargeIntervalInSecs") {
                    constant.value = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
                    constant.text = "549755813887";
                }
            }
            eawr::core::LoadNoteBuffer extreme_notes;
            const auto bounded = eawr::units::durability_table(extreme, nullptr, &extreme_notes);
            eawr::core::LoadNoteCollector extreme_collector;
            extreme_collector.merge(extreme_notes);
            const auto extreme_captured = extreme_collector.snapshot();
            expect(static_cast<bool>(bounded), "extreme health and recharge load");
            expect(std::any_of(extreme_captured.begin(), extreme_captured.end(), [](const auto& note) {
                return note.code == "EAWR-LOAD-0121" && note.tag == "Tactical_Health";
            }), "health multiplication saturation records its tag");
            expect(std::any_of(extreme_captured.begin(), extreme_captured.end(), [](const auto& note) {
                return note.code == "EAWR-LOAD-0122" && note.authored_value == "549755813887";
            }), "service interval saturation records authored text");

        }
        std::vector<eawr::core::Diagnostic> notes;
        eawr::core::LoadNoteBuffer load_notes;
        const auto health = eawr::units::durability_table(*loaded.tables, &notes, &load_notes);
        const bool malformed = value == "<Health>1e99</Health>" || value == "<Health>broken</Health>"
            || value == "<Health>42junk</Health>" || value == "<Health>14500\xE2\x80\xAC.0</Health>"
            || value == "<Health>0.5x</Health>" || value == "<Health>14500.5x</Health>";
        // AU-82: trailing text after a leading number keeps the number; every other malformed value is zero.
        const bool keeps_number = value == "<Health>42junk</Health>" || value == "<Health>14500\xE2\x80\xAC.0</Health>"
            || value == "<Health>0.5x</Health>" || value == "<Health>14500.5x</Health>";
        const std::string_view expected_effective = value == "<Health>0.5x</Health>" ? "0.5"
            : value == "<Health>14500.5x</Health>" ? "14500.5" : value.starts_with("<Health>42") ? "42"
            : keeps_number ? "14500" : "0";
        eawr::core::LoadNoteCollector collector;
        collector.merge(load_notes);
        const auto captured = collector.snapshot();
        const auto recovered = std::count_if(captured.begin(), captured.end(), [](const auto& note) {
            return note.code == "EAWR-LOAD-0125";
        });
        expect(recovered == (malformed ? 2 : 0), "HD-06: only malformed authored Health emits a recovery LoadNote");
        if (malformed) {
            const auto authored = value.substr(std::string_view{"<Health>"}.size(),
                value.size() - std::string_view{"<Health></Health>"}.size());
            expect(std::all_of(captured.begin(), captured.end(), [&](const auto& note) {
                return note.code != "EAWR-LOAD-0125" || (note.diagnostic_code == "EAWR-LOAD-0125"
                    && note.tag == "Health" && note.authored_value == authored
                    && note.action == eawr::core::LoadAction::default_used
                    && note.effective_value == expected_effective
                    && (note.message.find("trailing text") != std::string::npos) == keeps_number
                    && note.classification == eawr::core::LoadCategory::unknown_degrade
                    && note.file == "data/xml/eawr_hardpoints.xml" && note.line && *note.line > 1
                    && note.layer.mod_id == "base" && note.object.starts_with("Test_Frigate/"));
            }), "HD-06: recovery notes retain authored text, object and source provenance");
            expect(!has_row(loaded.tables->unresolved, "HP_Test_Laser", "Health")
                && !has_row(loaded.tables->unresolved, "HP_Test_Engine", "Health"),
                "HD-06: accepted malformed Health is not an unresolved input");
            auto diagnostics_cleared = *loaded.tables;
            diagnostics_cleared.hardpoint_health_defaults.clear();
            expect(eawr::units::content_identity(diagnostics_cleared) == eawr::units::content_identity(*loaded.tables),
                "HD-06: recovery provenance is outside replay content identity");
        }
        expect(static_cast<bool>(health), "HD-06: omitted, zero, negative and malformed Health bind durability");
        if (!health) continue;
        const auto* ship = health.value().find(eawr::assets::object_type_crc("Test_Frigate"));
        expect(ship && ship->hardpoints.size() == 3 && ship->hardpoints[0].destroyable
            && ship->hardpoints[0].role == eawr::sim::tactical::HardpointRole::weapon
            && ship->hardpoints[1].destroyable
            && ship->hardpoints[1].role == eawr::sim::tactical::HardpointRole::engine
            && (ship->hardpoints[0].max_health.raw() == 0) == !keeps_number
            && (ship->hardpoints[1].max_health.raw() == 0) == !keeps_number,
            "HD-06: zero-health rows retain their flags, roles and authored order");
        if (keeps_number) {
            expect(notes.empty(), "AU-82: a Health that keeps its leading number is not a zero-health type");
            continue;
        }
        if (value == "<Health>-1</Health>") continue;
        expect(notes.size() == 1 && notes[0].code == "EAWR-UNITS-0003"
            && notes[0].severity == eawr::core::Severity::warning
            && notes[0].message.find("Test_Frigate") != std::string::npos,
            "HD-06: two zero-health hardpoints produce one load note for their type");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string_view token = "HP_Test_Laser, HP_Test_Engine";
        xml.replace(xml.find(token), token.size(), "HP_Test_Laser, HP_Test_Laser, HP_Test_Engine");
        write_fixture(tree.root, xml);
        auto points = std::string(hardpoints_xml);
        const std::string_view health = "<Health>260.0</Health>";
        points.replace(points.find(health), health.size(), "<Health>broken</Health>");
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables && loaded.tables->hardpoint_health_defaults.size() == 1,
            "HD-06: repeated hardpoint references retain one recovery provenance record");
        if (loaded.tables) {
            auto tables = *loaded.tables;
            constexpr std::string_view loser = "SYNTH_OIVRHNPFF4K4";
            constexpr std::string_view winner = "SYNTH_QLU1MOI5PTT3";
            expect(eawr::assets::object_type_crc(loser) == eawr::assets::object_type_crc(winner),
                "HD-06: dropped Health fixture uses colliding type IDs");
            const auto source = std::find_if(tables.units.begin(), tables.units.end(), [](const auto& unit) {
                return unit.id == "Test_Frigate";
            });
            expect(source != tables.units.end(), "HD-06: dropped Health fixture has its source unit");
            if (source != tables.units.end()) {
                source->id = loser;
                source->table_order = 9;
                auto selected = *source;
                selected.id = winner;
                selected.table_order = 2;
                selected.hardpoints.clear();
                tables.units.push_back(std::move(selected));
                for (auto& recovery : tables.hardpoint_health_defaults) {
                    if (recovery.object == "Test_Frigate") recovery.object = loser;
                }
                eawr::core::LoadNoteBuffer load_notes;
                const auto selected_health = eawr::units::durability_table(tables, nullptr, &load_notes);
                eawr::core::LoadNoteCollector collector;
                collector.merge(load_notes);
                const auto captured = collector.snapshot();
                expect(selected_health && selected_health.value().find(eawr::assets::object_type_crc(winner)),
                    "HD-06: the collision winner still binds durability");
                expect(std::any_of(captured.begin(), captured.end(), [&](const auto& note) {
                    return note.code == "EAWR-LOAD-0251" && note.object == loser;
                }), "HD-06: the malformed Health unit is dropped by type selection");
                expect(std::none_of(captured.begin(), captured.end(), [](const auto& note) {
                    return note.code == "EAWR-LOAD-0125";
                }), "HD-06: dropped malformed Health emits no recovery note");
            }
        }
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto opening = std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"};
        const auto begin = xml.find(opening);
        const auto end = xml.find("</SpaceUnit>", begin);
        xml.replace(end, std::string_view{"</SpaceUnit>"}.size(), "</SecondaryStructure>");
        xml.replace(begin, opening.size(), "<SecondaryStructure Name=\"Test_Frigate\"><Base_Level>4</Base_Level>");
        const auto behavior = std::string_view{"<Behavior>SELECTABLE</Behavior>"};
        xml.replace(xml.find(behavior), behavior.size(),
            "<Behavior>SELECTABLE, DUMMY_STAR_BASE, CAPTURE_POINT, UNIT_AI</Behavior>");
        const auto locomotion = std::string_view{"SIMPLE_SPACE_LOCOMOTOR"};
        xml.replace(xml.find(locomotion), locomotion.size(), "SPACE_OBSTACLE");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* depot = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        expect(depot && depot->xml_type == "SecondaryStructure" && depot->kind != UnitKind::station && depot->star_base
            && depot->capture_point && depot->unit_ai && depot->base_level == 4,
            "SAE-02: a capturable secondary structure carries its authored station behavior independently of class");
        if (loaded.tables) {
            const auto setup = eawr::skirmish::ai_setup({}, {}, *loaded.tables);
            const auto entry = std::find_if(setup.content.types.begin(), setup.content.types.end(),
                [](const auto& type) { return type.name == "TEST_FRIGATE"; });
            expect(entry != setup.content.types.end() && entry->star_base && entry->capture_point
                && entry->unit_ai && entry->base_level == 4 && !entry->locomotor,
                "SAE-02: the shared AI builder retains the secondary structure's station and capture roles");
        }
    }
    for (const int mode : {0, 1, 2}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto opening = std::string_view{"<StarBase Name=\"Test_Base\">"};
        xml.insert(xml.find(opening) + opening.size(),
            "<Behavior>DUMMY_STAR_BASE</Behavior><Base_Level>3</Base_Level>"
            "<Prev_Level_Base>Metadata_Base_Two</Prev_Level_Base>");
        const std::string last = mode == 1 ? "Missing_Base" : mode == 2 ? "Metadata_Base_Two" : "Metadata_Base_One";
        xml.insert(xml.rfind("</Objects>"),
            "<StarBase Name=\"Metadata_Base_Two\"><Base_Level>2</Base_Level><Prev_Level_Base>" + last +
            "</Prev_Level_Base></StarBase><StarBase Name=\"Metadata_Base_One\"><Base_Level>1</Base_Level></StarBase>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* station = loaded.tables ? loaded.tables->find("Skirmish_Test_Base") : nullptr;
        expect(station && station->previous_base_levels == (mode == 0
            ? std::vector<std::uint32_t>{2, 1} : std::vector<std::uint32_t>{2}),
            "SAE-02: station predecessor levels follow authored references and inherited tags");
        expect(loaded.tables && loaded.tables->find("Metadata_Base_Two") == nullptr
            && loaded.tables->find("Metadata_Base_One") == nullptr,
            "SAE-02: predecessor metadata does not add physical unit or model entries");
        if (loaded.tables) {
            const auto setup = eawr::skirmish::ai_setup({}, {}, *loaded.tables);
            const auto entry = std::find_if(setup.content.types.begin(), setup.content.types.end(),
                [](const auto& type) { return type.name == "SKIRMISH_TEST_BASE"; });
            expect(entry != setup.content.types.end() && entry->star_base
                && entry->previous_base_levels == station->previous_base_levels,
                "SAE-02: the shared tactical builder carries the authored station role and predecessor chain");
            auto changed = *loaded.tables;
            for (auto& unit : changed.units) unit.previous_base_levels.clear();
            expect(eawr::units::content_identity(changed) == eawr::units::content_identity(*loaded.tables),
                "SAE-02: AI predecessor metadata leaves physical replay content identity unchanged");
            for (auto& unit : changed.units) unit.star_base = false;
            const auto unqualified = eawr::skirmish::ai_setup({}, {}, changed);
            expect(std::none_of(unqualified.content.types.begin(), unqualified.content.types.end(),
                [](const auto& type) { return type.star_base; }),
                "SAE-02: a station class alone does not invent the authored starbase behavior");
        }
        if (mode != 0) expect(loaded.tables && has_row(loaded.tables->unresolved,
            "Skirmish_Test_Base", "Prev_Level_Base", last),
            "SAE-02: unresolved and cyclic station predecessors retain a diagnostic");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto begin = xml.find("<Unit_Abilities_Data");
        const auto end = xml.find("</Unit_Abilities_Data>", begin) + std::string_view{"</Unit_Abilities_Data>"}.size();
        xml.replace(begin, end - begin, R"xml(<Unit_Abilities_Data><Unit_Ability>
            <Type>TURBO</Type><Expiration_Seconds>10</Expiration_Seconds><Recharge_Seconds>40</Recharge_Seconds>
            <Mod_Multiplier>UNKNOWN_MULTIPLIER, 900</Mod_Multiplier>
            <Mod_Multiplier>speed_multiplier, 2</Mod_Multiplier>
            <Mod_Multiplier>target_speed_multiplier, 0.5</Mod_Multiplier>
            <Mod_Multiplier>TARGET_TAKE_DAMAGE_MULTIPLIER, 3</Mod_Multiplier>
            <Mod_Flag>weapons_enabled_flag, false</Mod_Flag>
            <Mod_Flag>CAN_BE_SQUASHED, true</Mod_Flag>
            <Mod_Flag>ALLOW_TARGET_DIMINISHING_FIREPOWER, false</Mod_Flag>
            <Mod_Flag>UNKNOWN_FLAG, true</Mod_Flag>
            <Mod_Flag>CAN_BE_SQUASHED, false</Mod_Flag>
            </Unit_Ability></Unit_Abilities_Data>)xml");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "AB-26: mixed modifier data loads");
        if (loaded.tables) {
            const auto& tables = *loaded.tables;
            expect(has_row(tables.notes, "Test_Frigate", "Mod_Multiplier", "UNKNOWN_MULTIPLIER, 900")
                && has_row(tables.notes, "Test_Frigate", "Mod_Flag", "UNKNOWN_FLAG, true")
                && has_row(tables.notes, "Test_Frigate", "Mod_Multiplier", "target_speed_multiplier, 0.5")
                && has_row(tables.notes, "Test_Frigate", "Mod_Flag", "weapons_enabled_flag, false"),
                "AB-26: unknown and retained unmodelled names have load diagnostics");
            const auto built = eawr::units::ability_table(tables);
            const auto* profile = built ? built.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(profile && profile->abilities.size() == 1, "AB-26: unknown names preserve the ability");
            if (profile && profile->abilities.size() == 1) {
                const auto& m = profile->abilities[0].modifiers;
                expect(m.speed == Fixed::from_raw(raw(2)) && m.target_speed == Fixed::from_decimal("0.5").value()
                    && m.target_take_damage == Fixed::from_raw(raw(3))
                    && m.weapons_enabled == false && m.can_be_squashed == false
                    && m.allow_target_diminishing_firepower == false,
                    "AB-26: known values retained case-insensitively, duplicate flag takes last value");
            }
            auto changed = tables;
            auto* unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
            expect(unit && unit->abilities[0].modifiers.size() == 3 && unit->abilities[0].modifier_flags.size() == 4,
                "AB-26: unknown values are not stored");
            if (unit) {
                unit->abilities[0].modifier_flags[0].value = true;
                expect(eawr::units::content_identity(changed) != eawr::units::content_identity(tables),
                    "AB-26: retained flags participate in content identity");
            }
        }
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        xml.insert(xml.find("</Objects>"), R"xml(
          <SpecialStructure Name="Test_Planetary_Source">
            <Affiliation>Rebel</Affiliation><SpaceBehavior>SPECIAL_WEAPON</SpaceBehavior>
            <Special_Weapon_Index>0</Special_Weapon_Index><Special_Weapon_Target_Action_Index>1</Special_Weapon_Target_Action_Index>
            <Special_Weapon_Valid_Targets>Transport | Frigate</Special_Weapon_Valid_Targets>
            <Projectile_Types>Proj_Test_Generic</Projectile_Types>
            <Projectile_Fire_Recharge_Seconds>12.52</Projectile_Fire_Recharge_Seconds>
            <Projectile_Fire_Pulse_Count>3</Projectile_Fire_Pulse_Count>
            <Projectile_Fire_Pulse_Delay_Seconds>0.52</Projectile_Fire_Pulse_Delay_Seconds>
          </SpecialStructure>)xml");
        const auto station = xml.find("<StarBase Name=\"Skirmish_Test_Base\">");
        expect(station != std::string::npos, "WSW: synthetic station opening exists");
        if (station != std::string::npos) xml.insert(station + std::string_view{"<StarBase Name=\"Skirmish_Test_Base\">"}.size(),
            "<Tactical_Buildable_Objects_Multiplayer>Rebel, Test_Use_Unlock</Tactical_Buildable_Objects_Multiplayer>");
        xml.insert(xml.find("</Objects>"), R"xml(<UpgradeObject Name="Test_Use_Unlock">
          <Behavior>DUMMY_UPGRADE, DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED</Behavior>
          <Affiliation>Rebel</Affiliation><CategoryMask>Upgrade</CategoryMask>
          <Tactical_Build_Cost_Multiplayer>1</Tactical_Build_Cost_Multiplayer>
          <Tactical_Build_Time_Seconds>1</Tactical_Build_Time_Seconds><Tactical_Production_Queue>Tactical_Upgrades</Tactical_Production_Queue>
          <Abilities SubObjectList="Yes"><Battlefield_Modifier_Ability Name="Test_Unlock_Effect">
            <Activation_Style>Space_Automatic</Activation_Style><Enable_Special_Weapons>Yes</Enable_Special_Weapons>
            <Apply_To_All_Allies>Yes</Apply_To_All_Allies>
          </Battlefield_Modifier_Ability></Abilities></UpgradeObject>)xml");
        write_fixture(tree.root, xml);
        write(tree.root / "XML" / "FactionFiles.xml", "<Faction_Files><File>test_factions.xml</File></Faction_Files>");
        write(tree.root / "XML" / "test_factions.xml", R"xml(<Factions><Faction Name="Rebel">
          <Is_Playable>yes</Is_Playable><Standalone_Space_Maps_Special_Weapon_A>Test_Planetary_Source</Standalone_Space_Maps_Special_Weapon_A>
          <Standalone_Space_Maps_Special_Weapon_B /></Faction></Factions>)xml");
        const auto loaded = load(tree.root, models(1.0F));
        expect(loaded.tables.has_value(), "WSW-01: faction mapping expands hidden source data without a space model");
        if (loaded.tables) {
            const auto* unlock = loaded.tables->find("Test_Use_Unlock");
            expect(unlock && unlock->consume_after_special_fire && unlock->special_unlocks.size() == 1
                && unlock->special_unlocks.front().enable && unlock->special_unlocks.front().all_allies,
                "WSW-07/08/46: active automatic unlock metadata and consume marker load independently");
            const auto* unit = loaded.tables->find("Test_Planetary_Source");
            expect(unit && unit->planetary_weapon && !unit->hull && unit->model_path.empty() && !unit->locomotion,
                "WSW-01: source remains separate from ground model and ship durability");
            expect(!has_row(loaded.tables->unresolved, "Test_Planetary_Source", "Space_Model_Name"),
                "WSW-01: no invented source space-model requirement");
            const auto combat = eawr::units::combat_table(*loaded.tables);
            const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Planetary_Source")) : nullptr;
            expect(profile && profile->planetary_weapon && profile->planetary_weapon->index == 0
                && profile->planetary_weapon->target_action == 1 && profile->planetary_weapon->valid_targets == 20
                && profile->planetary_weapon->recharge_frames == 376 && profile->planetary_weapon->pulse_delay_frames == 16,
                "WSW-14/19: loaded source carries authored selectors and nearest-frame timing");
            expect(profile && profile->weapons.size() == 1 && profile->weapons.front().shot
                && profile->weapons.front().cone_width == Fixed::from_raw(raw(360))
                && profile->weapons.front().cone_height == Fixed::from_raw(raw(180)),
                "WSW-21/45: shared projectile profile retains source cone defaults");
        }
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto marker = xml.find("<SpaceUnit Name=\"Test_Frigate\">") + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size();
        xml.insert(marker, "<Death_Projectiles>Proj_Test_Small, Proj_Test_Turbo</Death_Projectiles>"
            "<Ranged_Target_Z_Adjust>30</Ranged_Target_Z_Adjust>");
        const auto projectile = xml.find("<Projectile Name=\"Proj_Test_Generic\">")
            + std::string_view{"<Projectile Name=\"Proj_Test_Generic\">"}.size();
        xml.insert(projectile, "<Projectile_Damage_Delay_Secs>0.5</Projectile_Damage_Delay_Secs>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WNO-29/32: synthetic inherited payload fixture loads");
        if (loaded.tables) {
            const auto* unit = loaded.tables->find("Test_Frigate");
            const auto combat = eawr::units::combat_table(*loaded.tables);
            const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(unit && unit->death_projectiles == std::vector<std::string>{"Proj_Test_Small", "Proj_Test_Turbo"}
                && unit->death_projectile_indices.size() == 2 && profile && profile->death_projectiles.size() == 2,
                "WNO-29: loader resolves all authored entries in order");
            if (profile && profile->death_projectiles.size() == 2) {
                expect(profile->ranged_target_z_adjust == Fixed::from_raw(raw(30))
                    && profile->death_projectiles.front().damage_delay == Fixed::from_raw(raw(1) / 2),
                    "WNO-30/32: typed height and inherited explicit delay reach the sim");
            }
            auto changed = *loaded.tables;
            for (auto& ship : changed.units) if (ship.id == "Test_Frigate") ship.death_projectiles.clear();
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "WNO-29: death payload authoring changes replay content identity");
        }
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Create_Team_Type>Test_Squadron</Create_Team_Type><Redirect_Damage_To_Teammates>Yes</Redirect_Damage_To_Teammates>"
            "<Unit_Abilities_Data><Unit_Ability><Type>REPLENISH_WINGMEN</Type><Recharge_Seconds>60</Recharge_Seconds>"
            "<Particle_Effect>fixture_wingmen</Particle_Effect></Unit_Ability></Unit_Abilities_Data>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WHE-63/64 authored wingman fixture loads");
        if (loaded.tables) {
            const auto type = eawr::assets::object_type_crc("Test_Frigate");
            const auto abilities = eawr::units::ability_table(*loaded.tables);
            const auto combat = eawr::units::combat_table(*loaded.tables);
            const auto* bound = abilities ? abilities.value().find(type) : nullptr;
            const auto* damage = combat ? combat.value().find(type) : nullptr;
            expect(bound && bound->abilities.front().replenish_team == eawr::assets::object_type_crc("Test_Squadron")
                && bound->abilities.front().recharge_frames == 1800 && bound->abilities.front().replenish_particle == "fixture_wingmen",
                "WHE-63 creation type, recharge and member emitter are authored");
            expect(damage && damage->redirect_damage_to_teammates, "WHE-64 redirect flag reaches the ordinary combat profile");
            auto changed = *loaded.tables;
            for (auto& unit : changed.units) if (unit.id == "Test_Frigate") unit.redirect_damage_to_teammates = false;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "WHE-64 routing flag is replay content identity");
        }
    }
    for (const bool bomb : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        const std::string kind = bomb ? "HARMONIC_BOMB" : "WEAKEN_ENEMY";
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Unit_Abilities_Data><Unit_Ability><Type>" + kind + "</Type>"
            "<Spawned_Object_Type>Proj_Test_Small</Spawned_Object_Type><Bomb_Countdown_Seconds>1.5</Bomb_Countdown_Seconds>"
            "<Recharge_Seconds>20</Recharge_Seconds><Effective_Radius>1000</Effective_Radius>"
            "</Unit_Ability></Unit_Abilities_Data>");
        const std::string opening = "<Projectile Name=\"Proj_Test_Small\">";
        xml.insert(xml.find(opening) + opening.size(), R"(<Projectile_Weaken_Enemy_On_Detonation>Yes</Projectile_Weaken_Enemy_On_Detonation>
            <Projectile_Weaken_Enemy_Radius>300</Projectile_Weaken_Enemy_Radius>
            <Projectile_Weaken_Enemy_Duration_Seconds>15</Projectile_Weaken_Enemy_Duration_Seconds>
            <Projectile_Weaken_Enemy_Take_Damage_Increase_Percent>0</Projectile_Weaken_Enemy_Take_Damage_Increase_Percent>
            <Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent>0.2</Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent>
            <Projectile_Weaken_Enemy_Targets_Category_Mask>Frigate</Projectile_Weaken_Enemy_Targets_Category_Mask>
            <Projectile_Weaken_Enemy_Spawn_Effect>fixture_status</Projectile_Weaken_Enemy_Spawn_Effect>)");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "spawned hero authored fixture loads");
        if (!loaded.tables) continue;
        const auto abilities = eawr::units::ability_table(*loaded.tables);
        const auto* hero = abilities ? abilities.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(hero && hero->abilities.size() == 1 && hero->abilities.front().spawned.has_value(), "spawned type binds to ordinary ability slot");
        if (!hero || hero->abilities.empty() || !hero->abilities.front().spawned) continue;
        const auto& profile = *hero->abilities.front().spawned;
        expect(profile.type == eawr::assets::object_type_crc("Proj_Test_Small") && profile.damage == Fixed::from_raw(raw(5))
            && profile.blast.damage == Fixed::from_raw(raw(12)) && hero->abilities.front().recharge_frames == 600
            && profile.countdown_frames == (bomb ? 45U : 0U) && profile.reach == (bomb ? Fixed{} : Fixed::from_raw(raw(1000)))
            && profile.weaken.on_detonation && profile.weaken.radius == Fixed::from_raw(raw(300))
            && profile.weaken.duration_frames == 450 && profile.weaken.cause_damage_reduction == Fixed::from_decimal("0.2").value()
            && profile.weaken.status_effect == "fixture_status", "spawned profile preserves variant damage, countdown, reach and status data");
        auto changed = *loaded.tables;
        changed.projectiles.front().weaken.on_detonation = true;
        changed.projectiles.front().weaken.duration_frames = 1;
        expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables), "spawned status data participates in identity");
    }
    for (const auto energy : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        const std::string kind = energy ? "ENERGY_WEAPON" : "TRACTOR_BEAM";
        const std::string handler = energy ? "Energy_Weapon_Attack_Ability" : "Tractor_Beam_Attack_Ability";
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Unit_Abilities_Data><Unit_Ability><Type>" + kind + "</Type><GUI_Activated_Ability_Name>beam</GUI_Activated_Ability_Name>"
            "<Expiration_Seconds>5</Expiration_Seconds><Recharge_Seconds>20</Recharge_Seconds>"
            "<Mod_Multiplier>SPEED_MULTIPLIER,0.6</Mod_Multiplier></Unit_Ability></Unit_Abilities_Data>"
            "<Abilities><" + handler + " Name=\"beam\"><Activation_Style>User_Input</Activation_Style>"
            "<Activation_Min_Range>5</Activation_Min_Range><Activation_Max_Range>800</Activation_Max_Range>"
            "<Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories><Damage_Per_Frame>50</Damage_Per_Frame>"
            "<Stacking_Category>2</Stacking_Category><Target_Speed_Decrease_Percent>0.9</Target_Speed_Decrease_Percent>"
            "<Owner_Particle_Effect>invented_effect</Owner_Particle_Effect><Owner_Particle_Bone_Name>Engine</Owner_Particle_Bone_Name>"
            "</" + handler + "></Abilities>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WHE-26/27: synthetic authored beam data loads");
        if (!loaded.tables) continue;
        const auto table = eawr::units::ability_table(*loaded.tables);
        const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(profile && profile->abilities.size() == 1 && profile->special.size() == 1, "named beam binding produces ordinary timer and nested service");
        if (!profile || profile->abilities.empty() || profile->special.empty()) continue;
        const auto& nested = profile->special.front();
        expect(nested.beam_min_range == Fixed::from_raw(raw(5)) && nested.beam_max_range == Fixed::from_raw(raw(800))
            && nested.damage_per_frame == Fixed::from_raw(raw(50)) && nested.target_speed_decrease == Fixed::from_decimal("0.9").value()
            && nested.concentrate_stacking_category == 2 && nested.beam_owner_particle == "invented_effect"
            && nested.beam_owner_bone == "Engine" && profile->abilities.front().modifiers.speed == Fixed::from_decimal("0.6").value(),
            "beam ranges, damage, target reduction, category, particle, bone and source multiplier retain distinct XML values");
        auto changed = *loaded.tables;
        for (auto& unit : changed.units) if (unit.id == "Test_Frigate") unit.special_abilities.front().beam_max_range = Fixed::from_raw(raw(801));
        expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables), "beam content changes replay identity");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto begin = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        expect(begin != std::string::npos, "WHE-22: synthetic hero hull exists");
        const auto old = xml.find("<Unit_Abilities_Data", begin);
        const auto end = xml.find("</Unit_Abilities_Data>", old);
        expect(old != std::string::npos && end != std::string::npos, "WHE-22: existing fixture ability is found");
        if (old != std::string::npos && end != std::string::npos) xml.replace(old,
            end + std::string_view{"</Unit_Abilities_Data>"}.size() - old, R"(
          <Unit_Abilities_Data SubObjectList="Yes">
            <Unit_Ability><Type>INVULNERABILITY</Type><Expiration_Seconds>6</Expiration_Seconds>
              <Recharge_Seconds>60</Recharge_Seconds><Mod_Multiplier>TAKE_DAMAGE_MULTIPLIER, 0</Mod_Multiplier></Unit_Ability>
            <Unit_Ability><Type>POWER_TO_WEAPONS</Type><Expiration_Seconds>8</Expiration_Seconds>
              <Recharge_Seconds>50</Recharge_Seconds><Mod_Multiplier>CAUSE_DAMAGE_MULTIPLIER, 2.5</Mod_Multiplier></Unit_Ability>
          </Unit_Abilities_Data>)");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WHE-22: zero take-damage modifier parses");
        if (loaded.tables) {
            const auto table = eawr::units::ability_table(*loaded.tables);
            const auto* hero = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(hero && hero->abilities.size() == 2
                && hero->abilities[0].kind == eawr::sim::tactical::AbilityKind::invulnerability
                && hero->abilities[0].expiration_frames == 180 && hero->abilities[0].recharge_frames == 1800
                && hero->abilities[0].modifiers.take_damage == Fixed{}, "WHE-22: Falcon mode binds without loader rejection");
            expect(hero && hero->abilities.size() == 2 && hero->abilities[1].modifiers.cause_damage == Fixed::from_decimal("2.5").value(),
                "WHE-22: recognised POWER_TO_WEAPONS retains the cause-damage modifier");
        }
    }
    hero_company_tables();
    nested_special_tables();
    concentrate_fire_tables();
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string tags = "<Is_Asteroid_Field>Yes</Is_Asteroid_Field><Is_Ion_Storm>Yes</Is_Ion_Storm>"
            "<Is_Nebula>Yes</Is_Nebula><Is_Impassable_Asteroid>Yes</Is_Impassable_Asteroid>"
            "<Space_Obstacle_Offset>12 -7 3</Space_Obstacle_Offset>";
        const auto begin = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        expect(begin != std::string::npos, "WHZ-01: synthetic hazard profile target exists");
        if (begin != std::string::npos) {
            const std::string original = "<SpaceBehavior>TARGETING,REVEAL , HIDE_WHEN_FOGGED, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>";
            xml.replace(xml.find(original, begin), original.size(),
                "<SpaceBehavior>ASTEROID_FIELD_DAMAGE NEBULA SPACE_OBSTACLE</SpaceBehavior>");
            xml.insert(begin + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size(), tags);
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F));
            if (loaded.tables) {
                const auto* unit = loaded.tables->find("Test_Frigate");
                expect(unit && unit->footprint.space_obstacle, "WHZ-01: SpaceBehavior opts into tracking");
                if (unit) {
                    const auto& hazard = unit->footprint.hazard;
                    expect(hazard.asteroid_field && hazard.ion_storm && hazard.nebula && hazard.impassable_asteroid,
                        "WHZ-01: overlapping hazard flags remain independent");
                    expect(hazard.asteroid_damage && hazard.nebula_service,
                        "WHZ-01: affected-unit service attachment is authored");
                    expect(hazard.obstacle_offset == point(raw(12), raw(-7), raw(3)), "WHZ-01: raw offset retains all axes");
                    auto changed = *loaded.tables;
                    changed.units[static_cast<std::size_t>(unit - loaded.tables->units.data())]
                        .footprint.hazard.ion_storm = false;
                    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                        "WHZ-01: independent semantic flags bind content identity");
                }
            }
        }
    }
    // WST-09: optional GameConstants seconds truncate to logical frames.
    for (const std::string value : {"", "2.25", "0", "-1", "oops", "3601"}) {
        TempTree fixture;
        write_fixture(fixture.root, units_xml("3600", "Unit_Select_Test"));
        const auto path = fixture.root / "XML" / "GameConstants.xml";
        if (!value.empty()) {
            std::ifstream stream(path);
            std::string constants((std::istreambuf_iterator<char>(stream)), {});
            constants.insert(constants.find("</GameConstants>"),
                "<Stealth_Detection_Time>" + value + "</Stealth_Detection_Time>");
            write(path, constants);
        }
        const auto loaded = load(fixture.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WST-09: optional detection constant fixture loads");
        if (!loaded.tables) continue;
        const bool valid = value.empty() || value == "2.25" || value == "0";
        expect(has_row(loaded.tables->unresolved, "GameConstants", "Stealth_Detection_Time") == !valid,
            "WST-09: invalid detection seconds are reported");
        auto tables = *loaded.tables;
        auto& owner = tables.units.front();
        eawr::units::Ability cloak;
        cloak.type = "STEALTH";
        cloak.expiration_seconds = Fixed::from_raw(raw(10));
        cloak.gui_activated_ability_name = "Test_Stealth";
        owner.abilities = {cloak};
        eawr::sim::tactical::SpecialAbilityProfile handler;
        handler.name = "Test_Stealth";
        handler.kind = eawr::sim::tactical::SpecialAbilityKind::stealth;
        handler.style = eawr::sim::tactical::SpecialActivationStyle::user_input;
        handler.service_interval = 1;
        owner.special_abilities = {handler};
        const auto bound = eawr::units::ability_table(tables);
        expect(static_cast<bool>(bound), "WST-09: detection constant binds through ability table");
        const auto* profile = bound ? bound.value().find(eawr::assets::object_type_crc(owner.id)) : nullptr;
        expect(profile && profile->stealth_detection_frames == (value == "2.25" ? 67U : value == "0" ? 0U : 30U),
            "WST-09: authored 2.25 seconds truncate to 67 frames; absent defaults to 30");
        if (value == "2.25") {
            auto absent = tables;
            std::erase_if(absent.constants.scalars, [](const auto& constant) { return constant.tag == "Stealth_Detection_Time"; });
            expect(eawr::units::content_identity(absent) != eawr::units::content_identity(tables),
                "WST-09: authored detection time enters content identity");
        }
    }
    barrage_inputs();
    cluster_bomb_inputs();
    manual_weapon_loading();
    randomized_fire_bone_loading();
    hardpoint_turret_loading();
    special_weapon_classification();
    rocket_variant_inputs();
    unknown_inaccuracy_categories();
    unknown_restriction_categories();
    restriction_list_edges();
    ion_stun_content_errors();
    ability_limits();
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    const auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;

    expect(tables.units.size() == 5, "pinned types then the squadron craft");
    if (tables.units.size() != 5) return;
    expect(tables.units[4].id == "Test_Fighter" && tables.units[4].kind == UnitKind::craft, "craft follow the pinned types");

    const auto& station = tables.units[0];
    expect(station.kind == UnitKind::station && station.xml_type == "StarBase", "StarBase is a station");
    expect(station.station_community_property && !station.community_property && !station.capture_point,
        "WSU-16: an authored community station is shared without capture-point behaviour");
    expect(station.ai_community_property, "PE-34: AI community metadata is read for non-capture originals");
    auto capture_tables = tables;
    capture_tables.units[1].capture_point = true;
    auto selection_only = capture_tables;
    selection_only.units[0].ai_community_property = false;
    selection_only.units[0].station_community_property = false;
    expect(eawr::units::content_identity(selection_only) == eawr::units::content_identity(capture_tables),
        "WSU-16: station selection metadata leaves simulation identity unchanged");
    selection_only.units[1].community_property = true;
    expect(eawr::units::content_identity(selection_only) != eawr::units::content_identity(capture_tables),
        "WBP-01: capture-point community admission still binds simulation identity");
    expect(!station.mouse_sensitive && !station.bar_admitted && tables.units[1].mouse_sensitive
        && tables.units[1].bar_admitted,
        "WSU-13/50: mouse and bar admission follow behaviour/data, independent of positive hull");
    expect(!station.locomotion && tables.units[1].selectable && tables.units[1].locomotion,
           "WSU-21: selection eligibility follows behaviours, not the object category or speed");
    expect(tables.units[2].selectable && tables.units[2].locomotion,
           "WSU-19: squadron selection eligibility comes from its resolved team container");
    expect(station.variant_chain == std::vector<std::string>{"Skirmish_Test_Base", "Test_Base"}, "variant chain");
    expect(station.hull && station.hull->raw() == raw(1600), "station hull through the variant");
    expect(station.victory_relevant, "station is victory relevant");
    expect(station.fow_reveal_range && station.fow_reveal_range->raw() == raw(2000),
           "sensor range through the variant");
    expect(tables.units[1].fow_reveal_range && tables.units[1].fow_reveal_range->raw() == raw(1200)
               && tables.units[4].fow_reveal_range && tables.units[4].fow_reveal_range->raw() == raw(500)
               && !tables.units[2].fow_reveal_range,
           "ship and craft authored ranges; a squadron authors none");
    // #271: REVEAL decides; a squadron reveals through its team container, `Team` by
    // default, and a container variant keeps the last of its base's two authored ranges.
    expect(station.reveal && tables.units[1].reveal && !tables.units[4].reveal, "REVEAL on the station and ship only");
    expect(eawr::units::sensor_range(station) == std::optional(Fixed::from_raw(raw(2000)))
               && eawr::units::sensor_range(tables.units[1]) == std::optional(Fixed::from_raw(raw(1200)))
               && !eawr::units::sensor_range(tables.units[4]),
           "sensor ranges: REVEAL types only; the craft has none");
    expect(tables.units[2].team_type == "Team" && eawr::units::sensor_range(tables.units[2]) == std::optional(Fixed::from_raw(raw(800))),
           "a squadron without Create_Team_Type reveals through Team at 800");
    expect(tables.units[3].team_type == "Test_Pair_Container"
               && eawr::units::sensor_range(tables.units[3]) == std::optional(Fixed::from_raw(raw(1000))),
           "a named container reveals with its own (inherited, last) range");
    expect(station.inactive_abilities == std::vector<std::string>{"Income_Stream_Ability Test_Income"},
           "station ability sub-objects are listed as inactive");
    // #530 PU-02: the income stream through the variant.
    expect(station.production.income.size() == 1 && station.production.income[0].name == "Test_Income"
               && station.production.income[0].base_value == Fixed::from_raw(raw(30))
               && station.production.income[0].interval_seconds == Fixed::from_raw(raw(10)),
           "the station's income stream");
    expect(station.spawner.has_value(), "SPAWN_SQUADRON station has a spawner");
    if (station.spawner) {
        const auto& spawner = *station.spawner;
        expect(spawner.starting.size() == 1 && spawner.starting[0].squadron == "Test_Squadron" &&
                   spawner.starting[0].count == 2 && spawner.starting[0].squadron_index == 2,
               "starting list resolves to the squadron table row");
        expect(spawner.delay_seconds && spawner.delay_seconds->raw() == raw(10), "spawn delay");
        expect(spawner.reserves.size() == 1 && spawner.reserves[0].count == -1 && !spawner.reserves_used,
               "reserves are read but flagged unused (SK-36)");
    }
    expect(station.hardpoints.size() == 1 && station.hardpoints[0].attachment.position == point(0, 0, raw(-150)),
           "station bay position from its bone");

    const auto& frigate = tables.units[1];
    expect(frigate.kind == UnitKind::ship && frigate.model_path == "data/art/models/test_frigate.alo", "frigate model path");
    expect(frigate.scale_factor && frigate.scale_factor->raw() == Fixed::from_decimal("0.70").value().raw(), "scale factor");
    expect(frigate.movement.acceleration && frigate.movement.acceleration->raw() == Fixed::from_decimal(".04").value().raw(),
           "acceleration");
    expect(frigate.movement.space_layer == "Frigate", "space layer is trimmed");
    expect(frigate.targeting_max_attack_distance && frigate.targeting_max_attack_distance->raw() == raw(1000),
           "the last of a duplicated single-value tag wins");
    expect(has_row(tables.notes, "Test_Frigate", "targeting_max_attack_distance"), "the duplicate is noted");
    expect(frigate.category_mask == std::vector<std::string>{"Frigate", "AntiCorvette"}, "category mask splits on |");
    expect(frigate.targeting_priority_set_index == 0 && tables.priority_sets.size() == 2,
           "only referenced priority sets load, in first-reference order");
    expect(frigate.category_bits == 0x210 && frigate.property_flags.empty() && frigate.property_bits == 0,
           "category bits from the enum file; no property flags");
    expect(tables.units[4].property_flags == std::vector<std::string>{"SmallShip"} && tables.units[4].property_bits == 1,
           "Property_Flags names and bits");
    expect(tables.categories.size() == 9 && tables.categories[5].name == "Capital" && tables.categories[5].value == 32 &&
               tables.categories[8].value == ~std::uint64_t{0} &&
               tables.properties.size() == 2,
           "dynamic enums load in file order, hexadecimal or decimal, 64 bits");
    if (tables.priority_sets.size() == 2) {
        using eawr::units::PriorityMatch;
        const auto& set = tables.priority_sets[0];
        const auto& entries = set.attack_priorities;
        expect(entries.size() == 5 && entries[0].match == PriorityMatch::category && entries[0].bits == 4 &&
                   entries[3].name == "test_fighter" && entries[3].match == PriorityMatch::type && entries[3].bits == 0 &&
                   entries[4].match == PriorityMatch::property && entries[4].bits == 1,
               "entries are categories, then properties, then exact types");
        expect(set.category_exclusions == std::vector<std::string>{"Capital", "Frigate", "AntiBomber"} &&
                   set.category_exclusion_bits == 0x130 && set.property_exclusion_bits == 2,
               "exclusion masks split on commas and |");
        expect(set.hard_point_priorities == std::vector<std::string>{"Shield_Generator", "WEAPON_LASER"} &&
                   set.hard_point_exclusions == std::vector<std::string>{"Fighter_Bay"},
               "hardpoint names are checked; the unknown one is dropped");
    }
    expect(frigate.lua_script == "ObjectScript_Test", "Lua_Script reference");
    expect(frigate.hardpoints.size() == 3, "every listed hardpoint has a row");
    if (frigate.hardpoints.size() == 3) {
        const auto& laser = frigate.hardpoints[0];
        expect(laser.type == eawr::units::HardpointType::weapon_laser && laser.targetable && laser.destroyable,
               "laser hardpoint flags");
        expect(laser.health && laser.health->raw() == raw(260), "hardpoint health");
        expect(laser.attachment.position == point(raw(10), raw(20), raw(30)), "attachment bone position");
        expect(laser.fire_a.position == point(raw(10), raw(22), raw(30)) && laser.fire_a.from_attached_model,
               "fire bone from the attached model, placed through the rotated attachment frame");
        expect(laser.attachment.owner_bone_index.has_value() && !laser.fire_a.owner_bone_index,
               "BCAM-21: only owner-model bones expose an index for posed owner lookup");
        auto metadata_changed = tables;
        metadata_changed.units[1].hardpoints[0].attachment.owner_bone_index.reset();
        expect(eawr::units::content_identity(metadata_changed) == eawr::units::content_identity(tables),
               "BCAM-21: presentation bone indices do not change content identity");
        expect(!laser.fire_b.position, "missing fire bone stays unresolved");
        expect(laser.weapon.has_value(), "weapon hardpoint has a weapon");
        if (laser.weapon) {
            const auto& weapon = *laser.weapon;
            expect(weapon.projectile == "Proj_Test_Turbo" && weapon.projectile_index != eawr::units::no_index &&
                       tables.projectiles[weapon.projectile_index].id == "Proj_Test_Turbo",
                   "weapon projectile index");
            expect(weapon.pulse_count == 5U && weapon.range && weapon.range->raw() == raw(1100), "pulse count and range");
            expect(weapon.inaccuracy.size() == 2 && weapon.inaccuracy[0].category == "Fighter" &&
                       weapon.inaccuracy[0].distance.raw() == raw(70),
                   "every inaccuracy row is kept");
            expect(weapon.damage_type == "Damage_Test" && weapon.opportunity_fire_when_targeting, "weapon damage type and flags");
            expect(weapon.damage && weapon.damage->raw() == raw(40), "a hardpoint's own damage (DG-25)");
        }
        expect(frigate.hardpoints[1].attachment.position == point(raw(-40), 0, raw(5) / 2),
               "bone names match without case");
        expect(frigate.hardpoints[2].id == "HP_Test_Missing" && frigate.hardpoints[2].type_name.empty(),
               "an unknown hardpoint keeps its row");
    }
    expect(frigate.target_bones.size() == 1 && frigate.target_bones[0].position == point(raw(-39), raw(2), raw(11) / 2),
           "target bone composes its parent chain");
    expect(frigate.abilities.size() == 1, "one unit ability");
    if (frigate.abilities.size() == 1) {
        const auto& ability = frigate.abilities[0];
        expect(ability.type == "DEFEND" && ability.authored_type == "defend", "ability type is upper-cased");
        expect(ability.modifiers.size() == 2 && ability.modifiers[0].value.raw() == Fixed::from_decimal("0.8").value().raw() &&
                   ability.modifiers[1].value.raw() == raw(-3),
               "C float suffixes are accepted on modifiers");
        expect(ability.supports_autofire && ability.recharge_seconds && ability.expiration_seconds, "ability timing");
    }

    const auto& squadron = tables.units[2];
    expect(squadron.kind == UnitKind::squadron && squadron.members.size() == 3, "every Squadron_Units line contributes");
    if (squadron.members.size() == 3) {
        expect(squadron.members[2].craft_index == 4 && squadron.members[2].offset == point(0, raw(-15), raw(1) / 2),
               "member offsets by order");
    }
    expect(squadron.abilities.size() == 1 && squadron.abilities[0].type == "HUNT", "squadron ability");
    const auto& pair = tables.units[3];
    expect(pair.members.size() == 2 && pair.members[1].offset == point(raw(-10), 0, 0) &&
               pair.abilities.size() == 1,
           "ML-26: placeholder-first variant replaces the base and keeps every member occurrence");
    expect(has_row(tables.notes, "Test_Squadron_Pair", "Squadron_Offsets", "Test_Squadron"),
           "a base layer that also authors the offsets is noted");

    const auto& fighter = tables.units[4];
    expect(fighter.weapon.has_value(), "craft object weapon");
    if (fighter.weapon) {
        const auto index = fighter.weapon->projectile_index;
        expect(index != eawr::units::no_index && tables.projectiles[index].damage &&
                   tables.projectiles[index].damage->raw() == raw(5) && tables.projectiles[index].max_speed &&
                   tables.projectiles[index].max_speed->raw() == raw(11),
               "projectile values through its variant");
        if (index < tables.projectiles.size()) {
            const auto& blast = tables.projectiles[index].blast;
            expect(blast.damage.raw() == raw(12) && blast.radius.raw() == raw(80) && blast.dropoff
                && blast.tiers == 3 && blast.max_victims == 0 && blast.max_delay.raw() == 0,
                "WAD-01: blast tags inherit before a derived damage override; stock zero delay retained");
        }
        expect(fighter.weapon->min_recharge_seconds == fighter.weapon->max_recharge_seconds, "object weapon recharge");
        expect(fighter.weapon->inaccuracy.empty(),
               "the object weapon scatters by Targeting_Fire_Inaccuracy, not the unit's Fire_Inaccuracy_Distance (DG-24)");
    }

    expect(tables.constants.scalars.size() == 51, "every required combat scalar has a row");
    for (const auto& constant : tables.constants.scalars) {
        // Diminishing_Firepower is a list: it keeps its text (#74).
        expect(constant.value.has_value() || constant.tag == "Diminishing_Firepower", constant.tag);
    }
    expect(tables.constants.damage_to_armor.size() == 3, "only fleet damage/armor rows load");
    expect(has_row(tables.notes, "GameConstants", "Damage_To_Armor_Mod", "damage_fighter, armor_fighter"),
           "missing fleet pairs are noted");

    expect(tables.unresolved.size() == 3 && has_row(tables.unresolved, "Test_Frigate", "hardpoint", "HP_Test_Missing") &&
               has_row(tables.unresolved, "HP_Test_Laser", "Fire_Bone_B", "FP_Missing") &&
               has_row(tables.unresolved, "Frigate", "Hard_Point_Exclusions", "Engine_Typo"),
           "the scan lists exactly the missing hardpoint and bone and the unknown hardpoint name");
    if (tables.unresolved.size() != 3) print_rows("unresolved", tables.unresolved);
    expect(!tables.inputs.empty() && tables.inputs.front().logical_path < tables.inputs.back().logical_path,
           "input files are listed in path order");

    // #72 durability: hull and hardpoint health x Object_Max_Health_Multiplier_Space (1.5).
    expect(station.hardpoints.size() == 1 && station.hardpoints[0].repair_amount_per_frame &&
               station.hardpoints[0].repair_amount_per_frame->raw() == Fixed::from_decimal(".50").value().raw() &&
               station.hardpoints[0].repair_cost_per_frame &&
               station.hardpoints[0].repair_cost_per_frame->raw() == Fixed::from_decimal("1.5").value().raw(),
           "station hardpoint repair values");
    expect(frigate.hardpoints.size() == 3 && !frigate.hardpoints[0].repair_amount_per_frame &&
               frigate.destroyed_with_hardpoints && !station.destroyed_with_hardpoints,
           "the absent hardpoint-death tag defaults to yes; an authored No overrides it");
    const auto durability = eawr::units::durability_table(tables);
    expect(static_cast<bool>(durability), "durability table builds");
    if (!durability) return;
    const auto& table = durability.value();
    expect(table.rules.hull_vs_hardpoints.raw() == Fixed::from_decimal("0.2").value().raw() &&
               table.rules.engines_disabled_speed.raw() == Fixed::from_decimal("0.4").value().raw() &&
               table.rules.damaged_fraction.raw() == Fixed::from_decimal("0.33").value().raw(),
           "durability rules from gameconstants.xml");
    expect(table.profiles.size() == 3, "station, ship and craft have a hull; squadrons do not");
    if (table.damage) {
        // DG-12: an authored row, a negative row that reads as 1, and an unauthored pair at 1.
        const auto types = eawr::units::damage_type_index(tables);
        const auto multiplier = [&](const std::string_view damage, const std::string_view armor) {
            return eawr::sim::tactical::armor_multiplier(*table.damage, types.damage(damage), types.armor(armor)).raw();
        };
        expect(multiplier("Damage_Turbolaser", "Armor_Test") == Fixed::from_decimal("0.5").value().raw(),
               "an authored armor row keeps its multiplier");
        expect(multiplier("Damage_Turbolaser", "Armor_Fighter") == Fixed::scale, "a negative armor row reads as 1");
        expect(multiplier("Damage_Fighter", "Armor_Fighter") == Fixed::scale, "an unauthored armor pair multiplies by 1");
    } else {
        expect(false, "the fixture has damage rules");
    }
    const auto* frigate_profile = table.find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(frigate_profile != nullptr, "profiles are keyed by the object-type CRC");
    const auto* authored_no_profile = table.find(eawr::assets::object_type_crc("Skirmish_Test_Base"));
    if (authored_no_profile != nullptr) {
        auto health = eawr::sim::tactical::full_durability(*authored_no_profile);
        const auto hit = eawr::sim::tactical::apply_damage(*authored_no_profile, health, 0, Fixed::from_raw(raw(1500)));
        expect(!hit.unit_destroyed && health.hull.raw() > 0,
               "an authored No keeps the hull alive after the last hardpoint is destroyed");
    }
    if (frigate_profile != nullptr) {
        auto health = eawr::sim::tactical::full_durability(*frigate_profile);
        const auto first = eawr::sim::tactical::apply_damage(*frigate_profile, health, 0, Fixed::from_raw(raw(390)));
        const auto last = eawr::sim::tactical::apply_damage(*frigate_profile, health, 1, Fixed::from_raw(raw(255)));
        expect(!first.unit_destroyed && last.unit_destroyed && health.hull == Fixed{},
               "a ship without the tag dies when its last destroyable hardpoint is destroyed");
    }
    if (frigate_profile != nullptr) {
        using eawr::sim::tactical::HardpointRole;
        const auto& hp = frigate_profile->hardpoints;
        expect(frigate_profile->max_hull.raw() == raw(5400) && frigate_profile->max_speed &&
                   frigate_profile->max_speed->raw() == Fixed::from_decimal("2.2").value().raw(),
               "hull x 1.5 and the authored speed");
        expect(hp.size() == 3 && hp[0].role == HardpointRole::weapon && hp[0].destroyable && hp[0].max_health.raw() == raw(390) &&
                   hp[1].role == HardpointRole::engine && hp[1].max_health.raw() == raw(255) &&
                   hp[2].role == HardpointRole::other && !hp[2].destroyable && hp[2].max_health.raw() == 0,
               "hardpoint roles and health x 1.5");
        // DG-05 (#440): no unit XML tag turns this off; every M2 unit keeps FoC's default.
        expect(frigate_profile->allow_diminishing_firepower, "the diminishing-firepower flag defaults on");
    }
    const auto* station_profile = table.find(eawr::assets::object_type_crc("Skirmish_Test_Base"));
    expect(station_profile != nullptr && station_profile->max_hull.raw() == raw(2400) && !station_profile->max_speed &&
               station_profile->hardpoints.size() == 1 &&
               station_profile->hardpoints[0].role == eawr::sim::tactical::HardpointRole::fighter_bay &&
               station_profile->hardpoints[0].repair_cost_per_frame.raw() == Fixed::from_decimal("1.5").value().raw(),
           "station profile with its fighter bay and repair values");

    // #70 motion: speeds, accelerations and the rate of turn x Object_Max_Speed_Multiplier_Space (1.2).
    const auto motion = eawr::units::motion_table(tables);
    expect(static_cast<bool>(motion), "motion table builds");
    if (!motion) return;
    {
        TempTree authored_team;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string opening = "<Container Name=\"Test_Pair_Base_Container\">";
        xml.insert(xml.find(opening) + opening.size(),
            "<Max_Speed>4</Max_Speed><OverrideDeceleration>0.5</OverrideDeceleration>");
        write_fixture(authored_team.root, xml);
        const auto loaded_team = load(authored_team.root, models(-40.0F));
        const auto* parent = loaded_team.tables ? loaded_team.tables->find("Test_Squadron_Pair") : nullptr;
        expect(parent && parent->team_movement.max_speed == Fixed::from_raw(raw(4))
            && parent->team_movement.deceleration == Fixed::from_raw(raw(1) / 2),
            "WMV-44: inherited Create_Team_Type supplies the actual container movement inputs");
        expect(loaded_team.tables && eawr::units::content_identity(*loaded_team.tables) != eawr::units::content_identity(tables),
            "WMV-44: authored container formation inputs change content identity");
    }
    {
        auto changed = tables;
        for (auto& item : changed.units) {
            if (item.kind == UnitKind::craft) item.movement.deceleration = Fixed::from_raw(raw(7));
            if (item.kind == UnitKind::squadron) { item.team_movement.deceleration.reset(); item.movement.deceleration = Fixed::from_raw(raw(9)); }
        }
        const auto defaults = eawr::units::motion_table(changed);
        const auto parent_id = eawr::assets::object_type_crc("Test_Squadron");
        const auto* parent = defaults ? defaults.value().squadrons.find_squadron(parent_id) : nullptr;
        expect(parent && parent->formation_deceleration == Fixed{},
            "WMV-44: unauthored container deceleration stays zero despite child overrides");
        for (auto& item : changed.units) {
            if (item.id == "Test_Squadron") item.team_movement.deceleration = Fixed::from_raw(raw(1) / 4);
        }
        const auto authored = eawr::units::motion_table(changed);
        parent = authored ? authored.value().squadrons.find_squadron(parent_id) : nullptr;
        expect(parent && parent->formation_deceleration == Fixed::from_decimal("0.3").value(),
            "WMV-44: an omitted maximum retains the nonzero type default and speed ratio");
        for (auto& item : changed.units) {
            if (item.id == "Test_Squadron") item.team_movement.max_speed = Fixed{};
        }
        const auto zero_maximum = eawr::units::motion_table(changed);
        parent = zero_maximum ? zero_maximum.value().squadrons.find_squadron(parent_id) : nullptr;
        expect(parent && parent->formation_deceleration == Fixed::from_raw(raw(1) / 4),
            "WMV-44: an explicitly zero maximum returns its own override unscaled");
        for (auto& item : changed.units) {
            if (item.id == "Test_Squadron") item.team_movement.max_speed = Fixed::from_raw(raw(3));
        }
        const auto scaled_parent = eawr::units::motion_table(changed);
        parent = scaled_parent ? scaled_parent.value().squadrons.find_squadron(parent_id) : nullptr;
        expect(parent && parent->formation_deceleration == Fixed::from_decimal("0.3").value(),
            "WMV-44: a member with authored maximum speed scales its own override");
        const auto* craft = authored ? authored.value().squadrons.find_craft(eawr::assets::object_type_crc("Test_Fighter")) : nullptr;
        const auto* original = motion.value().squadrons.find_craft(eawr::assets::object_type_crc("Test_Fighter"));
        expect(craft && original && craft->thrust == original->thrust,
            "WMV-42: deceleration overrides do not replace craft flight thrust");
    }
    for (const auto& [general, space, station_role] : {
            std::tuple{"DUMMY_STAR_BASE", "SPAWN_SQUADRON", true},
            std::tuple{"SELECTABLE", "dummy_star_base", true},
            std::tuple{"STARBASE", "SPAWN_SQUADRON", false},
            std::tuple{"DUMMY_GROUND_STRUCTURE", "SPAWN_SQUADRON", false},
            std::tuple{"SELECTABLE", "SPAWN_SQUADRON", false}}) {
        auto changed = tables;
        for (auto& unit : changed.units) if (unit.id == "Skirmish_Test_Base") {
            unit.footprint.hazard.behavior = {general};
            unit.footprint.hazard.space_behavior = {space};
            unit.spawner->starting.clear();
            for (auto& tier : unit.spawner->campaign_starting) tier.clear();
        }
        const auto rebound = eawr::units::motion_table(changed);
        const auto* profile = rebound ? rebound.value().squadrons.find_spawner(
            eawr::assets::object_type_crc("Skirmish_Test_Base")) : nullptr;
        expect(rebound && (profile != nullptr) == station_role
            && (!profile || (profile->starbase && profile->entries.empty() && !profile->bays.empty())),
            "WSL-26: station behavior in either list admits an empty authored hangar; kind alone does not");
    }
    const auto times = [](const char* value) {
        return eawr::sim::math::multiply(Fixed::from_decimal(value).value(), Fixed::from_decimal("1.2").value()).value();
    };
    expect(motion.value().rules.arc_degrees == Fixed::from_raw(raw(15)) &&
               motion.value().rules.expansion_distance == Fixed::from_raw(raw(300)),
           "motion rules: 360 / MaxRotationsSpace and XYExpansionDistanceSpace");
    // space-targeting R-01: a weapon hardpoint whose authored maximum recharge is within
    // Hardpoint_Recharge_Cutoff_For_Opportunity_Fire is forced on for an AI owner; the comparison
    // uses the authored seconds, not the truncated hundredths (perturbation of both values).
    for (const auto& [recharge, cutoff, forced] : {std::tuple{"3.0", "3.0", true}, std::tuple{"3.005", "3.0", false},
                                                    std::tuple{"2.0", "1.5", false}, std::tuple{"2.0", "2.0", true}}) {
        auto changed = tables;
        for (auto& unit : changed.units) {
            for (auto& hardpoint : unit.hardpoints) {
                if (hardpoint.weapon) hardpoint.weapon->max_recharge_seconds = Fixed::from_decimal(recharge).value();
            }
        }
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag == "Hardpoint_Recharge_Cutoff_For_Opportunity_Fire") {
                constant.value = Fixed::from_decimal(cutoff).value();
            }
        }
        const auto rebound = eawr::units::combat_table(changed);
        bool any = false;
        bool all = true;
        if (rebound) {
            for (const auto& profile : rebound.value().profiles) {
                for (const auto& weapon : profile.weapons) {
                    if (weapon.hardpoint == eawr::sim::tactical::object_weapon) continue;
                    any = true;
                    all = all && weapon.ai_opportunity_forced == forced;
                }
            }
        }
        expect(rebound && any && all, "R-01: the AI opportunity cutoff compares the authored recharge seconds");
    }
    // OR-06, OR-14: the approach re-check interval and the guard range come from gameconstants.xml
    // (the fixture authors FoC's 10 frames and 750 units); each authored value moves the rule.
    expect(motion.value().rules.reevaluation_frames == 10 && motion.value().rules.guard_range == Fixed::from_raw(raw(750)) && motion.value().rules.max_waypoints == 6,
           "OR-06/OR-14: the fixture's stock values reach the motion rules");

    // HCB-58 (#4992): FoC reads the constant with atoi into 32 bits and compares the queue count as unsigned.
    for (const auto& [value, capacity] : {std::pair{"0", 0UL}, std::pair{"2", 2UL}, std::pair{"9", 9UL}, std::pair{"2.5", 2UL},
             std::pair{"-0.5", 0UL}, std::pair{"-1", 4294967295UL}, std::pair{"-2", 4294967294UL}, std::pair{"65536", 65536UL},
             std::pair{"65537", 65537UL}, std::pair{"70000", 70000UL}, std::pair{"3000000000", 2147483647UL}}) {
        auto changed = tables;
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag == "MaxWaypointsPerPath") constant.value = Fixed::from_decimal(value).value();
        }
        const auto rebound = eawr::units::motion_table(changed);
        expect(rebound && rebound.value().rules.max_waypoints == capacity,
            std::string{"HCB-58: authored queue capacity "} + value + " reads as the retail unsigned 32-bit value");
    }
    // #4992: the authored text reaches the motion rules the way FoC's atoi reads it (Alliance at War authors -1).
    for (const auto& [text, capacity] : {std::pair{" -1 ", 4294967295UL}, std::pair{"0", 0UL}, std::pair{"6", 6UL},
             std::pair{"65536", 65536UL}, std::pair{"65537", 65537UL}, std::pair{"2.9", 2UL}, std::pair{"lots", 0UL},
             std::pair{"7 paths", 7UL}}) {
        TempTree constants_tree;
        write_fixture(constants_tree.root, units_xml("3600", "Unit_Select_Test"));
        const auto path = constants_tree.root / "XML" / "GameConstants.xml";
        std::string authored;
        {
            std::ifstream in(path, std::ios::binary);
            authored.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string_view stock = "<MaxWaypointsPerPath> 6 </MaxWaypointsPerPath>";
        const auto at = authored.find(stock);
        expect(at != std::string::npos, "HCB-58: the fixture authors MaxWaypointsPerPath");
        if (at == std::string::npos) continue;
        authored.replace(at, stock.size(), "<MaxWaypointsPerPath>" + std::string{text} + "</MaxWaypointsPerPath>");
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << authored;
        }
        const auto reloaded = load(constants_tree.root, models(-40.0F));
        std::optional<std::uint64_t> got;
        if (reloaded.tables) {
            if (const auto rebound = eawr::units::motion_table(*reloaded.tables)) got = rebound.value().rules.max_waypoints;
        }
        expect(got == capacity, std::string{"HCB-58: authored text '"} + text + "' loads as the retail unsigned queue capacity");
    }
    // Zero wait legs remain valid; negative values degrade with a note.
    for (const auto& [frames, cost, valid] : {std::tuple{"0", "0.0", true}, std::tuple{"0", "0.8", true},
             std::tuple{"-1", "0.8", true}, std::tuple{"100", "-0.5", true}}) {
        auto changed = tables;
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag == "WaitOperatorBaseFrameTime") constant.value = Fixed::from_decimal(frames).value();
            if (constant.tag == "WaitOperatorCostCoefficient") constant.value = Fixed::from_decimal(cost).value();
        }
        expect(static_cast<bool>(eawr::units::motion_table(changed)) == valid,
            std::string{"PC-08: wait leg frame time "} + frames + " and cost " + cost + (valid ? " accepted" : " refused"));
    }
    const auto set_motion_constant = [](auto& changed, const std::string_view tag, const std::string_view text) {
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag != tag) continue;
            constant.value = Fixed::from_decimal(text).value();
            constant.text = std::string(text);
            return;
        }
        changed.constants.scalars.push_back({std::string(tag), Fixed::from_decimal(text).value(), std::string(text)});
    };
    const auto count_motion_notes = [](const auto& captured, const std::string_view code, const std::string_view tag) {
        return std::count_if(captured.begin(), captured.end(), [&](const auto& note) {
            return note.code == code && note.tag == tag;
        });
    };
    for (const auto tag : {"MovementReevaluationFrameCount", "Space_Guard_Range", "Nebula_Ability_Disable_Time"}) {
        auto changed = tables;
        auto& constants = changed.constants.scalars;
        constants.erase(std::remove_if(constants.begin(), constants.end(), [&](const auto& entry) {
            return entry.tag == tag;
        }), constants.end());
        eawr::core::LoadNoteBuffer notes;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
        eawr::core::LoadNoteCollector collector;
        collector.merge(notes);
        const auto captured = collector.snapshot();
        expect(recovered && recovered.value().rules.reevaluation_frames == 10
            && recovered.value().rules.guard_range == Fixed::from_raw(raw(750))
            && recovered.value().nebula_disable_seconds == Fixed::from_raw(raw(5)),
            "omitted interval, guard and nebula globals use their stock values");
        expect(count_motion_notes(captured, "EAWR-LOAD-0150", tag) == 1
            && std::any_of(captured.begin(), captured.end(), [&](const auto& note) {
                return note.tag == tag && note.classification == eawr::core::LoadCategory::retail_accepts
                    && note.action == eawr::core::LoadAction::default_used && note.retail == "LSA-P5";
            }), "each missing global emits exactly one class B stock-default note");
        if (!recovered) continue;
        eawr::sim::tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
            {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
        auto live = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, recovered.value());
        expect(static_cast<bool>(live), "missing-global motion session creates");
        if (live) {
            const eawr::sim::InlineExecutor executor;
            expect(static_cast<bool>(live.value().step(executor)), "missing-global motion session steps");
        }
    }
    for (const auto& [text, effective, changed_value] : {std::tuple{"-1", "2", true},
             std::tuple{"400", "360", true}, std::tuple{"2", "2", false}, std::tuple{"360", "360", false}}) {
        auto changed = tables;
        set_motion_constant(changed, "MaxRotationsSpace", text);
        eawr::core::LoadNoteBuffer notes;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
        eawr::core::LoadNoteCollector collector;
        collector.merge(notes);
        const auto captured = collector.snapshot();
        const auto rotations = Fixed::from_decimal(effective).value();
        expect(recovered && recovered.value().avoidance->max_rotations == rotations
            && recovered.value().rules.arc_degrees == eawr::sim::math::divide(Fixed::from_raw(raw(360)), rotations).value(),
            "0151: rotations clamp before division at their exact bounds");
        expect(count_motion_notes(captured, "EAWR-LOAD-0151", "MaxRotationsSpace") == (changed_value ? 1 : 0),
            "0151: rotations emit one note only when changed");
    }
    for (const auto& [text, effective, changed_value] : {std::tuple{"0", 2U, true}, std::tuple{"2.5", 2U, true},
             std::tuple{"70000", 65536U, true}, std::tuple{"-3", 2U, true}, std::tuple{"65536", 65536U, false}}) {
        auto changed = tables;
        set_motion_constant(changed, "MovementReevaluationFrameCount", text);
        eawr::core::LoadNoteBuffer notes;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
        eawr::core::LoadNoteCollector collector;
        collector.merge(notes);
        const auto captured = collector.snapshot();
        expect(recovered && recovered.value().rules.reevaluation_frames == effective,
            "0152: interval truncation and bounds have exact effective frame counts");
        expect(count_motion_notes(captured, "EAWR-LOAD-0152", "MovementReevaluationFrameCount") == (changed_value ? 1 : 0),
            "0152: interval conversion emits one note only when changed");
        for (const auto& note : captured)
            if (note.code == "EAWR-LOAD-0152") expect(note.authored_value == text
                && note.effective_value == std::to_string(effective), "0152: record keeps authored spelling and final frames");
    }
    for (const auto& [text, effective, changed_value] : {std::tuple{"16", 16U, false}, std::tuple{"17", 16U, true},
             std::tuple{"6.9", 6U, true}, std::tuple{"4294967297", 16U, true}}) {
        auto changed = tables;
        set_motion_constant(changed, "SpacePathingTries", text);
        eawr::core::LoadNoteBuffer notes;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
        eawr::core::LoadNoteCollector collector;
        collector.merge(notes);
        const auto captured = collector.snapshot();
        expect(recovered && recovered.value().avoidance->tries == effective, "0158: count truncation and saturation are bounded");
        expect(count_motion_notes(captured, "EAWR-LOAD-0158", "SpacePathingTries") == (changed_value ? 1 : 0),
            "0158: count conversion emits exactly one note on change");
        for (const auto& note : captured)
            if (note.tag == "SpacePathingTries") expect(note.effective_value == std::to_string(effective),
                "0158: even a count above uint32 reports the final effective count");
    }
    for (const auto extent : {Fixed::from_raw(raw(262144)), Fixed::from_raw(raw(300000)), Fixed::from_raw(INT64_MAX)}) {
        auto changed = tables;
        for (auto& unit : changed.units) {
            if (unit.id != "Test_Frigate") continue;
            unit.scale_factor = Fixed::from_raw(raw(extent.raw() == INT64_MAX ? 2 : 1));
            unit.footprint.custom_hard_x = extent;
            unit.footprint.custom_hard_y = Fixed::from_raw(raw(40));
        }
        for (const bool combat_path : {false, true}) {
            eawr::core::LoadNoteBuffer notes;
            bool accepted = false;
            Fixed effective;
            if (combat_path) {
                const auto recovered = eawr::units::combat_table(changed, eawr::units::ContentErrors::strict, nullptr, &notes);
                accepted = static_cast<bool>(recovered);
                if (recovered) for (const auto& footprint : recovered.value().footprints)
                    if (footprint.type_id == eawr::assets::object_type_crc("Test_Frigate")) effective = footprint.x_extent;
            } else {
                const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
                accepted = static_cast<bool>(recovered);
                if (recovered) effective = recovered.value().footprint(eawr::assets::object_type_crc("Test_Frigate"))->x_extent;
            }
            eawr::core::LoadNoteCollector collector;
            collector.merge(notes);
            const auto captured = collector.snapshot();
            expect(accepted && effective == Fixed::from_raw(raw(262144)), "0157: combat and motion accept bounded target geometry");
            expect(count_motion_notes(captured, "EAWR-LOAD-0157", "Custom_Hard_XExtent")
                == (extent == Fixed::from_raw(raw(262144)) ? 0 : 1), "0157: combat and motion retain exactly one extent note");
        }
    }
    // Missing globals are class B; unsupported authored motion is class C. Both bind and step.
    for (const bool missing : {true, false}) {
        auto changed = tables;
        if (missing) {
            auto& constants = changed.constants.scalars;
            constants.erase(std::remove_if(constants.begin(), constants.end(), [](const auto& entry) {
                return entry.tag == "Object_Max_Speed_Multiplier_Space" || entry.tag == "MaxRotationsSpace"
                    || entry.tag == "XYExpansionDistanceSpace" || entry.tag.starts_with("TurnInPlaceSlowdown")
                    || entry.tag == "WaitOperatorSpeedCoefficient";
            }), constants.end());
        } else {
            for (auto& constant : changed.constants.scalars) {
                if (constant.tag == "MaxRotationsSpace" || constant.tag == "XYExpansionDistanceSpace"
                    || constant.tag == "Space_Guard_Range" || constant.tag == "WaitOperatorSpeedCoefficient"
                    || constant.tag == "TurnInPlaceSlowdownFrigate" || constant.tag == "Nebula_Ability_Disable_Time") {
                    constant.value = Fixed::from_decimal("-1").value();
                    constant.text = "-1";
                }
                if (constant.tag == "MovementReevaluationFrameCount" || constant.tag == "SpacePathingTries") {
                    constant.value = Fixed::from_decimal("2.5").value();
                    constant.text = "2.5";
                }
            }
            for (auto& unit : changed.units) {
                if (unit.id != "Test_Frigate") continue;
                unit.movement.max_speed = Fixed::from_raw(INT64_MAX);
                unit.movement.acceleration = Fixed{};
                unit.movement.deceleration = Fixed{};
                unit.movement.max_rate_of_turn = Fixed{};
                unit.movement.max_rate_of_roll = Fixed::from_raw(-Fixed::scale);
                unit.footprint.custom_soft_radius = Fixed::from_raw(1);
                unit.footprint.custom_hard_x = Fixed::from_raw(INT64_MAX);
                unit.footprint.custom_hard_y = Fixed::from_raw(INT64_MAX);
                unit.scale_factor = Fixed::from_raw(2 * Fixed::scale);
            }
        }
        eawr::core::LoadNoteBuffer buffer;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &buffer);
        eawr::core::LoadNoteCollector collector;
        collector.merge(buffer);
        const auto captured = collector.snapshot();
        expect(static_cast<bool>(recovered), "motion loader normalizes before its former abort paths");
        expect(std::any_of(captured.begin(), captured.end(), [&](const auto& note) {
            return note.code == (missing ? "EAWR-LOAD-0150" : "EAWR-LOAD-0155")
                && note.classification == (missing ? eawr::core::LoadCategory::retail_accepts
                                                  : eawr::core::LoadCategory::unknown_degrade)
                && (missing ? note.retail == "LSA-P5" : note.object == "Test_Frigate");
        }), "motion family notes carry classification and source object");
        if (!recovered) continue;
        if (missing) {
            expect(recovered.value().rules.arc_degrees == Fixed::from_raw(15 * Fixed::scale),
                "missing rotations use the stock default");
        } else {
            const auto* profile = recovered.value().find(eawr::assets::object_type_crc("Test_Frigate"));
            expect(profile && profile->max_speed == Fixed::from_raw(1024 * Fixed::scale)
                && profile->acceleration == Fixed::from_raw(Fixed::scale / 4096),
                "overflow saturates and profile fields clamp to supported bounds");
            expect(recovered.value().rules.guard_range == Fixed{} && recovered.value().rules.reevaluation_frames == 2,
                "negative guard range clamps and fractional frame count truncates");
        }
        eawr::sim::tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
            {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
        auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, recovered.value());
        expect(static_cast<bool>(session), "degraded motion session creates");
        if (session) {
            const eawr::sim::InlineExecutor executor;
            expect(static_cast<bool>(session.value().step(executor)), "degraded motion session steps");
        }
    }
    const auto with_constants = [&](const char* interval, const char* guard) {
        auto changed = tables;
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag == "MovementReevaluationFrameCount") constant.value = Fixed::from_decimal(interval).value();
            if (constant.tag == "Space_Guard_Range") constant.value = Fixed::from_decimal(guard).value();
        }
        return eawr::units::motion_table(changed);
    };
    for (const auto& [interval, guard] : {std::pair{"25", "1200.5"}, std::pair{"12", "0"}}) {
        const auto rebound = with_constants(interval, guard);
        expect(rebound && rebound.value().rules.reevaluation_frames == static_cast<std::uint32_t>(std::stoul(interval))
                   && rebound.value().rules.guard_range == Fixed::from_decimal(guard).value(),
               std::string{"OR-06/OR-14: authored MovementReevaluationFrameCount and Space_Guard_Range reach the motion rules ("}
                   + interval + ", " + guard + (rebound ? ")" : "): " + rebound.error().message));
    }
    for (const char* bad : {"0", "2.5", "70000", "-3"}) {
        expect(static_cast<bool>(with_constants(bad, "750")), "OR-06: unsupported intervals degrade before validation");
    }
    expect(static_cast<bool>(with_constants("10", "-1")), "OR-14: a negative guard range degrades to zero");
    expect(motion.value().profiles.size() == 1, "only the ship moves by motion profile; craft and stations do not");
    const auto* moving = motion.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(moving != nullptr && moving->max_speed == times("2.2") && moving->acceleration == times(".04") &&
               moving->deceleration == moving->max_speed && moving->rate_of_turn == times("0.7") &&
               moving->turn_in_place_slowdown == Fixed::from_raw(raw(3)),
           "frigate: values x 1.2, deceleration defaults to the maximum speed, the frigate slowdown");
    // #351 BK-01: without Max_Rate_Of_Roll and Bank_Turn_Angle the engine defaults apply.
    expect(moving != nullptr && moving->roll_rate == times("2") && moving->bank_angle == Fixed::from_raw(raw(70)),
           "frigate without roll tags: roll rate 2 x 1.2, bank angle 70");
    for (const auto& [name, layer, slowdown] : {
            std::tuple{"corvette", eawr::sim::tactical::SpaceLayer::corvette, 2},
            std::tuple{"fRiGaTe", eawr::sim::tactical::SpaceLayer::frigate, 3},
            std::tuple{" CAPITAL ", eawr::sim::tactical::SpaceLayer::capital, 4},
            std::tuple{"supercapital", eawr::sim::tactical::SpaceLayer::super_capital, 4}}) {
        auto mixed = tables;
        for (auto& unit : mixed.units) if (unit.id == "Test_Frigate") unit.movement.space_layer = name;
        const auto rebound = eawr::units::motion_table(mixed);
        const auto id = eawr::assets::object_type_crc("Test_Frigate");
        const auto* profile = rebound ? rebound.value().find(id) : nullptr;
        const auto* shape = rebound ? rebound.value().footprint(id) : nullptr;
        expect(profile && shape && profile->turn_in_place_slowdown == Fixed::from_raw(raw(slowdown))
            && shape->layer == layer, "layer spelling uses the same slowdown and footprint layer");
    }
    auto delayed = motion.value();
    delayed.rules.reevaluation_frames = 10;
    delayed.avoidance->search_delay = 10;
    expect(!eawr::sim::tactical::validate_motion(delayed), "PC-08: equal landing and reevaluation frames rejected");
    delayed.avoidance->search_delay = 11;
    expect(!eawr::sim::tactical::validate_motion(delayed), "PC-08: late landing rejected");
    delayed.avoidance->search_delay = 9;
    expect(static_cast<bool>(eawr::sim::tactical::validate_motion(delayed)), "PC-08: landing before reevaluation accepted");
    {
        auto collision = tables;
        auto unit = std::find_if(collision.units.begin(), collision.units.end(), [](const auto& item) {
            return item.id == "Test_Frigate";
        });
        if (unit != collision.units.end()) {
            unit->id = "SYNTH_QLU1MOI5PTT3";
            unit->table_order = 10;
            eawr::units::ObstacleType obstacle;
            obstacle.id = "SYNTH_OIVRHNPFF4K4";
            obstacle.table_order = 1;
            obstacle.space_layer = "Capital";
            collision.obstacles.push_back(obstacle);
            eawr::core::LoadNoteBuffer notes;
            const auto result = eawr::units::motion_table(collision, {}, nullptr, &notes);
            eawr::core::LoadNoteCollector collector;
            collector.merge(notes);
            const auto captured = collector.snapshot();
            const auto* shape = result ? result.value().footprint(eawr::assets::object_type_crc(unit->id)) : nullptr;
            expect(shape && shape->layer == eawr::sim::tactical::SpaceLayer::capital,
                "earlier obstacle definition wins a unit footprint CRC collision");
            expect(std::any_of(captured.begin(), captured.end(), [&](const auto& note) {
                return note.code == "EAWR-LOAD-0454" && note.object == unit->id && note.effective_value == obstacle.id;
            }), "footprint collision reports the omitted definition and winner");
            if (result) {
                eawr::sim::tactical::TacticalSetup setup;
                setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                    {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
                auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, result.value());
                expect(static_cast<bool>(session), "collision-degraded motion session creates");
                if (session) {
                    const eawr::sim::InlineExecutor executor;
                    expect(static_cast<bool>(session.value().step(executor)), "collision-degraded motion session steps");
                }
            }
            auto second_obstacle = obstacle;
            second_obstacle.id = unit->id;
            second_obstacle.table_order = 2;
            second_obstacle.space_layer = "Corvette";
            collision.obstacles.push_back(second_obstacle);
            const eawr::units::FootprintTypeSelection selection(collision);
            expect(selection.find(unit->id) && selection.find(unit->id)->obstacle == &collision.obstacles.front(),
                "shared consumer selection retains the same earliest definition");
            const auto obstacle_pair = eawr::units::motion_table(collision);
            shape = obstacle_pair ? obstacle_pair.value().footprint(eawr::assets::object_type_crc(unit->id)) : nullptr;
            expect(shape && shape->layer == eawr::sim::tactical::SpaceLayer::capital,
                "earlier obstacle wins an obstacle footprint CRC collision");
            collision.obstacles.pop_back();
            collision.obstacles.back().space_layer.clear();
            const auto no_shape = eawr::units::motion_table(collision);
            expect(no_shape && !no_shape.value().footprint(eawr::assets::object_type_crc(unit->id)),
                "a winning definition without a space layer does not admit the losing footprint");
            collision.obstacles.back().space_layer = "Capital";
            collision.obstacles.back().table_order = 20;
            const auto unit_first = eawr::units::motion_table(collision);
            shape = unit_first ? unit_first.value().footprint(eawr::assets::object_type_crc(unit->id)) : nullptr;
            expect(shape && shape->layer == eawr::sim::tactical::SpaceLayer::frigate,
                "earlier unit definition wins a unit obstacle CRC collision");
        }
    }
    // #71: the path finder's constants and a footprint per type with a Space_Layer.
    const auto& avoidance = motion.value().avoidance;
    expect(avoidance && avoidance->max_expansions == 3500 && avoidance->tries == 6 && avoidance->tracking_interval == 90
               && avoidance->tracking_windows == 45 && avoidance->wait_frames == Fixed::from_raw(raw(100))
               && avoidance->destination_search_increment == Fixed::from_raw(raw(50)),
           "avoidance rules from gameconstants.xml");
    const auto* footprint = motion.value().footprint(eawr::assets::object_type_crc("Test_Frigate"));
    expect(footprint != nullptr && footprint->layer == eawr::sim::tactical::SpaceLayer::frigate && !footprint->obstacle
               && footprint->radius == eawr::sim::math::multiply(Fixed::from_decimal("40").value(), Fixed::from_decimal("0.70").value()).value(),
           "the frigate is tracked in the frigate layer, soft radius 40 x 0.7");
}


// #4985: community selection follows the object's behaviour, not its XML class. A mod's shipyard is a
// SpaceBuildable; a mover or a capture point does not take the station flag.
void community_structure_tables() {
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string closing = "</Objects>";
    xml.replace(xml.rfind(closing), closing.size(), R"xml(
<SpaceBuildable Name="Test_Mod_Shipyard">
  <Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>
  <Is_Community_Property>Yes</Is_Community_Property>
</SpaceBuildable>
<SpaceBuildable Name="Test_Mod_Shipyard_Variant">
  <Variant_Of_Existing_Type>Test_Mod_Shipyard</Variant_Of_Existing_Type>
</SpaceBuildable>
<SpaceBuildable Name="Test_Mod_Private_Shipyard">
  <Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>
</SpaceBuildable>
<SpaceBuildable Name="Test_Mod_Capture_Shipyard">
  <Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>
  <Behavior>CAPTURE_POINT</Behavior>
  <Is_Community_Property>Yes</Is_Community_Property>
</SpaceBuildable>
<SpaceUnit Name="Test_Mod_Community_Mover">
  <Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>
  <SpaceBehavior>SPACE_OBSTACLE, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>
  <Is_Community_Property>Yes</Is_Community_Property>
</SpaceUnit>
</Objects>)xml");
    TempTree tree;
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F), "Normal_Default", eawr::sim::tactical::BattleDomain::space,
        {"Test_Mod_Shipyard", "Test_Mod_Shipyard_Variant", "Test_Mod_Private_Shipyard", "Test_Mod_Capture_Shipyard",
         "Test_Mod_Community_Mover"});
    if (!loaded.tables) return;
    const auto flag = [&](const std::string_view id) {
        const auto* unit = loaded.tables->find(id);
        expect(unit != nullptr, "community structure fixture type loads");
        return unit != nullptr && unit->station_community_property;
    };
    const auto* shipyard = loaded.tables->find("Test_Mod_Shipyard");
    expect(shipyard != nullptr && shipyard->kind != UnitKind::station && shipyard->xml_type == "SpaceBuildable",
        "#4985: a mod shipyard is a SpaceBuildable, not a station");
    expect(flag("Test_Mod_Shipyard"), "#4985: an immobile SpaceBuildable with Is_Community_Property is ally-selectable");
    expect(flag("Test_Mod_Shipyard_Variant"), "#4985: the community flag survives a variant of the shipyard");
    expect(!flag("Test_Mod_Private_Shipyard"), "#4985: an immobile structure without the tag stays private");
    expect(!flag("Test_Mod_Community_Mover"), "#4985: a mover with the tag and SPACE_OBSTACLE is not an ally-selectable structure");
    const auto* capture = loaded.tables->find("Test_Mod_Capture_Shipyard");
    expect(capture != nullptr && capture->capture_point && capture->community_property && !capture->station_community_property,
        "#4985: a capture point keeps its own community flag, not the station flag");
}

} // namespace unit_tables_test_support

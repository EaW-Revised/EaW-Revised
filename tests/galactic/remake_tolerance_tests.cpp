#include "galactic_support.hpp"
#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/campaign/setup_plan.hpp"
#include "eawr/sim/galactic/hero_return.hpp"
#include "eawr/sim/galactic/production.hpp"

// Rules the debug build applies to data that names things that do not exist or authors values a stock campaign
// never does (docs/behaviour/campaign-setup.md, CGS-05..12). Each case uses renamed synthetic data.
namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using Fixed = eawr::sim::math::Fixed;
Fixed f(std::int64_t n) { return Fixed::from_integer(n).value(); }

eawr::units::GalacticType type(const char* name, const char* behavior) {
    eawr::units::GalacticType result;
    result.name = name;
    if (*behavior) result.behavior = {behavior};
    return result;
}

void production_duration_has_only_the_arithmetic_limit() {
    // Mods author information cards with a build time no session reaches.
    const auto frames = gc::production_frames(f(9999999));
    expect(frames && frames.value() == 9999999ULL * gc::galactic_frames_per_second, "a 9999999 second duration converts to frames");
    expect(!gc::production_frames(Fixed::from_raw(-1)), "a negative duration still refuses");
    const auto zero = gc::production_frames({});
    expect(zero && zero.value() == 0, "a zero duration stays valid");
}

void composites_need_their_members() {
    // CGS-08.
    eawr::units::GalacticTypeTable table;
    table.types.push_back(type("Carrier", "TRANSPORT"));
    table.types.push_back(type("Troop", "LAND_UNIT"));
    table.types.push_back(type("Ship", "DUMMY_STARSHIP"));
    auto company = type("Company", "DUMMY_GROUND_COMPANY");
    company.company_transport = "Carrier";
    company.company_units = {"Troop"};
    table.types.push_back(company);
    table.types.push_back(type("Hollow", "DUMMY_GROUND_COMPANY"));
    auto orphan = type("Orphan", "DUMMY_GROUND_COMPANY");
    orphan.company_transport = "Missing";
    orphan.company_units = {"Troop"};
    table.types.push_back(orphan);
    auto squadron = type("Squadron", "DUMMY_SPACE_FIGHTER_SQUADRON");
    squadron.squadron_units = {"Ship", "Ship"};
    table.types.push_back(squadron);
    auto broken = type("Broken", "DUMMY_SPACE_FIGHTER_SQUADRON");
    broken.squadron_units = {"Ship", "Missing"};
    table.types.push_back(broken);
    auto nested = type("Nested", "DUMMY_GROUND_COMPANY");
    nested.company_transport = "Carrier";
    nested.company_units = {"Hollow"};
    table.types.push_back(nested);
    expect(cp::campaign_type_creatable(table, "Ship") && cp::campaign_type_creatable(table, "Company")
        && cp::campaign_type_creatable(table, "squadron"), "a resolved composite and a plain ship are creatable");
    expect(!cp::campaign_type_creatable(table, "Hollow"), "a company without transport or members is not creatable");
    expect(!cp::campaign_type_creatable(table, "Orphan"), "a company whose transport resolves to no type is not creatable");
    expect(!cp::campaign_type_creatable(table, "Broken"), "a squadron with an unresolved member is not creatable");
    expect(!cp::campaign_type_creatable(table, "Nested"), "a composite is not creatable when a member is itself uncreatable");
    expect(!cp::campaign_type_creatable(table, "NoSuchType"), "a name that is no type is not creatable");
}

void campaign_names_its_types_once() {
    // CGS-05: the type table must also hold types that carry no tracked galactic tag.
    eawr::data::galactic::GalacticData data;
    eawr::data::galactic::Campaign campaign;
    campaign.name = "Synthetic_Campaign";
    campaign.starting_forces = {{"FactionA", "PlanetA", "Alpha"}, {"FactionB", "PlanetB", "alpha"}, {"FactionB", "PlanetC", "Beta"}};
    campaign.special_case_production = {{"FactionA", "PlanetA", "Gamma"}, {"FactionA", "PlanetA", "BETA"}};
    data.campaigns.push_back(campaign);
    const auto names = cp::campaign_referenced_types(data, "synthetic_campaign");
    expect((names == std::vector<std::string>{"Alpha", "Beta", "Gamma"}), "starting and special-production types appear once, in authored order");
    expect(cp::campaign_referenced_types(data, "Other").empty(), "an unknown campaign names no types");
}

void null_structure_requirements_are_ignored() {
    // CGS-07: the eligibility check skips a null entry and a null alternative satisfies its group.
    const auto resolve = [](const char* text) {
        return cp::resolve_structure_requirements(text, [](const std::string_view name) -> gc::TypeId {
            return name == "Factory" ? 1 : name == "Research" ? 2 : 0;
        });
    };
    // RGC-09, one row per line of the retail table.
    const auto undefined_only = resolve("Missing");
    expect(undefined_only && undefined_only.value().groups.empty() && undefined_only.value().ignored.size() == 1,
        "an undefined name alone passes: the null outer entry is skipped");
    const auto absent_only = resolve("Factory");
    expect(absent_only && absent_only.value().groups == std::vector<std::vector<gc::TypeId>>{{1}},
        "a defined structure alone is required");
    const auto absent_or_undefined = resolve("Factory OR Missing");
    expect(absent_or_undefined && absent_or_undefined.value().groups.empty(),
        "a defined structure OR an undefined name passes: the null alternative satisfies the chain");
    const auto undefined_or_absent = resolve("Missing OR Factory");
    expect(undefined_or_absent && undefined_or_absent.value().groups == std::vector<std::vector<gc::TypeId>>{{1}}
        && undefined_or_absent.value().ignored == std::vector<std::string>{"Missing"},
        "an undefined name OR a defined structure fails to pass: the leading null is skipped and the structure stands");
    const auto led_chain = resolve("Missing OR Factory OR Research");
    expect(led_chain && led_chain.value().groups == std::vector<std::vector<gc::TypeId>>{{1, 2}},
        "the entry after a skipped leading null leads the rest of the chain");
    const auto both = resolve("Factory Research");
    expect(both && both.value().groups.size() == 2 && both.value().ignored.empty(), "resolved entries are separate AND groups");
    const auto alternatives = resolve("Factory | Research");
    expect(alternatives && alternatives.value().groups.size() == 1 && alternatives.value().groups.front().size() == 2,
        "resolved alternatives share a group");
    const auto lone = resolve("Missing Factory");
    expect(lone && lone.value().groups.size() == 1 && lone.value().groups.front() == std::vector<gc::TypeId>{1}
        && lone.value().ignored == std::vector<std::string>{"Missing"}, "a lone null entry is skipped and listed");
    const auto either = resolve("Factory OR Missing Research");
    expect(either && either.value().groups == std::vector<std::vector<gc::TypeId>>{{2}},
        "a satisfied chain is dropped; the next entry stays required");
    const auto all_null = resolve("Missing Absent");
    expect(all_null && all_null.value().groups.empty() && all_null.value().ignored.size() == 2, "all-null requirements need nothing");
    expect(!resolve("Factory OR") && !resolve("OR Factory") && !resolve("Factory OR OR Research") && !resolve("Missing OR"),
        "a malformed OR chain is still refused");
}
void undefined_spawn_names_stay_null_references() {
    // RGC-21..23: names are kept in authored rows; an undefined one is null (0) and listed, None is null without a line.
    eawr::units::GalacticType owner; owner.name = "Carrier";
    owner.starting_spawned_units[0] = {{"Fighter", 2}};
    owner.starting_spawned_units[2] = {{"Ghost", 1}};
    owner.starting_spawned_units[3] = {{"Missing", 4}, {"None", 1}, {"Fighter", 3}};
    std::vector<std::string> diagnostics;
    const auto rows = cp::resolve_starting_spawns(owner, [](const std::string_view name) -> std::uint64_t { return name == "Fighter" ? 7 : 0; },
        diagnostics);
    expect(rows[0].size() == 1 && rows[0][0].type == 7 && rows[0][0].count == 2, "a defined name resolves to its type and count");
    expect(rows[3].size() == 3 && rows[3][0].type == 0 && rows[3][0].count == 4 && rows[3][1].type == 0 && rows[3][2].type == 7,
        "undefined names stay in the row as null references and keep their counts and order");
    expect(rows[2].size() == 1 && rows[2][0].type == 0, "a row of only null references is still a nonempty row (no fallback to a lower tier)");
    expect(rows[1].empty() && rows[4].empty(), "rows that author nothing stay empty");
    expect(diagnostics.size() == 2 && diagnostics[0].find("Ghost") != std::string::npos && diagnostics[1].find("Missing") != std::string::npos,
        "each undefined name is listed once, None is not");
}
void defeated_hero_respawn_accepts_an_elapsed_default() {
    // CGS-12: Default_Hero_Respawn_Time -1 schedules an already elapsed creation.
    std::vector<gc::HeroReturnRecord> records;
    gc::HeroDefeat defeat;
    defeat.id = 50;
    defeat.type = defeat.respawn_type = 7;
    defeat.owner = 2;
    defeat.planet = 1;
    defeat.named = true;
    defeat.default_seconds = f(-1);
    const std::vector<gc::Planet> planets{{1, {}, 2}};
    const auto scheduled = gc::schedule_hero_return(records, defeat, 600, planets);
    expect(scheduled && scheduled.value().scheduled && records.size() == 1 && records.front().deadline == 600,
        "a negative default time schedules the return for the same frame");
    defeat.seconds = f(2);
    defeat.id = 51;
    defeat.type = defeat.respawn_type = 8;
    const auto authored = gc::schedule_hero_return(records, defeat, 600, planets);
    expect(authored && records.size() == 2 && records.back().deadline == 600 + 2 * gc::galactic_frames_per_second,
        "a positive authored absence still wins over the default");
}
} // namespace

int case_galactic_remake_tolerance_tests() {
    production_duration_has_only_the_arithmetic_limit();
    composites_need_their_members();
    campaign_names_its_types_once();
    null_structure_requirements_are_ignored();
    undefined_spawn_names_stay_null_references();
    defeated_hero_respawn_accepts_an_elapsed_default();
    return galactic_test::failures ? 1 : 0;
}

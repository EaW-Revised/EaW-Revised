// #4589: a start that places a unit type the lenient load skipped fails with a diagnostic naming the type,
// the build menus drop skipped types, and the skip set binds into the match identity. No game data.
#include "../support/check.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/skirmish/start.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

eawr::core::Diagnostic skip(const std::string& id, const std::string& why) {
    eawr::core::Diagnostic diagnostic;
    diagnostic.code = "EAWR-UNITS-0005";
    diagnostic.message = "skipped unit type '" + id + "': " + why;
    return diagnostic;
}

eawr::battle::StartUnit unit(const std::string& type, std::uint32_t entity) {
    eawr::battle::StartUnit placed;
    placed.type = type;
    placed.state.entity_id = entity;
    return placed;
}

} // namespace

int main() {
    eawr::testing::Checker checker;
    const auto check = [&](bool condition, std::string_view message) { checker.check(condition, message, std::cerr); };

    const std::vector<eawr::core::Diagnostic> diagnostics{
        skip("Republic_Light_Assault_Cruiser", "bad rows"), skip("Republic_Light_Assault_Cruiser", "again"),
        skip("Another_Type", "worse")};
    auto note = diagnostics.front();
    note.message = "projectile 'P' authors an ion stun reduction beyond +-1024; it saturates there (IS-10)";
    auto with_note = diagnostics;
    with_note.push_back(note);
    const auto ids = eawr::skirmish::skipped_type_ids(with_note);
    check(ids.size() == 2 && ids[0] == "Another_Type" && ids[1] == "Republic_Light_Assault_Cruiser",
        "skip ids are sorted and unique; warnings are not types");
    auto warning = note;
    warning.severity = eawr::core::Severity::warning;
    warning.message = "unit 'Retained_Type' ability 'HARMONIC_BOMB': creation skipped; ability retained";
    const std::vector<eawr::core::Diagnostic> warnings_only{warning, warning};
    check(eawr::skirmish::content_load_summary(warnings_only) == "0 unit types skipped, 1 unit with content warnings"
            && eawr::skirmish::skipped_type_ids(warnings_only).empty(),
        "#5064: warning-only content retains its unit and counts it once");
    with_note.push_back(warning);
    with_note.push_back(warning);
    warning.message = "unit type 'Retained_Type' authors a repeated ability";
    with_note.push_back(warning);
    warning.message = "unit 'Another_Type' ability warning before its rejection";
    with_note.push_back(warning);
    check(eawr::skirmish::content_load_summary(with_note) == "2 unit types skipped, 1 unit with content warnings",
        "#5064: duplicate diagnostics count types once; rejected types are excluded from retained warnings");
    warning.message = "projectile 'P' has a content warning";
    with_note.push_back(warning);
    check(eawr::skirmish::content_load_summary(with_note)
            == "2 unit types skipped, 1 unit with content warnings, 1 other content warning",
        "#5064: warnings without a unit owner remain visible separately");
    check(eawr::skirmish::content_load_summary({}) == "0 unit types skipped, 0 units with content warnings",
        "#5064: clean content has zero rejected and warned types");

    // B1: a placed skipped type fails the start, naming the type and the reason.
    eawr::battle::BattleStart start;
    start.units.push_back(unit("Victory_Class_Star_Destroyer", 1));
    check(static_cast<bool>(eawr::skirmish::refuse_skipped_starts(start, diagnostics)),
        "a start without skipped types is accepted");
    start.units.push_back(unit("republic_light_assault_cruiser", 2)); // case-insensitive like type names
    const auto refused = eawr::skirmish::refuse_skipped_starts(start, diagnostics);
    check(!refused && refused.error().message.find("Republic_Light_Assault_Cruiser") != std::string::npos
            && refused.error().message.find("bad rows") != std::string::npos,
        "a starting unit of a skipped type refuses the start and names type and reason");
    check(static_cast<bool>(eawr::skirmish::refuse_skipped_starts(start, {})), "nothing skipped: nothing refused");
    eawr::battle::BattleStart craft;
    craft.units.push_back(unit("Squadron_A", 3));
    craft.units.back().craft_type = "Another_Type";
    check(!eawr::skirmish::refuse_skipped_starts(craft, diagnostics), "a squadron of a skipped craft refuses the start");
    eawr::battle::BattleStart launching;
    launching.launches.push_back({});
    launching.launches.back().squadron = "Another_Type";
    check(!eawr::skirmish::refuse_skipped_starts(launching, diagnostics), "a launch of a skipped squadron refuses the start");

    // A recorded setup (a replay) that places a skipped type is refused the same way.
    eawr::sim::tactical::TacticalSetup recorded;
    recorded.units.resize(1);
    recorded.units[0].entity_id = 9;
    recorded.units[0].type_id = eawr::battle::type_id("Another_Type");
    const auto replay_refused = eawr::skirmish::refuse_skipped_setup(recorded, diagnostics);
    check(!replay_refused && replay_refused.error().message.find("Another_Type") != std::string::npos,
        "a recorded unit of a skipped type refuses the replay");
    recorded.units[0].type_id = eawr::battle::type_id("Victory_Class_Star_Destroyer");
    check(static_cast<bool>(eawr::skirmish::refuse_skipped_setup(recorded, diagnostics)),
        "a recording without skipped types is accepted");

    // B1: purchasable types of a skipped type leave the menus, with a log line each.
    eawr::sim::tactical::EconomyRules rules;
    rules.menus.resize(1);
    rules.menus[0].station = 7;
    for (const char* name : {"Victory_Class_Star_Destroyer", "Another_Type", "Republic_Light_Assault_Cruiser"}) {
        eawr::sim::tactical::BuildOption option;
        option.type = eawr::battle::type_id(name);
        rules.menus[0].options.push_back(option);
    }
    std::vector<std::string> log;
    check(eawr::skirmish::drop_skipped_menu_options(rules, ids, &log) == 2 && log.size() == 2
            && rules.menus[0].options.size() == 1
            && rules.menus[0].options[0].type == eawr::battle::type_id("Victory_Class_Star_Destroyer"),
        "menu entries of skipped types are dropped and logged");
    check(eawr::skirmish::drop_skipped_menu_options(rules, {}, &log) == 0, "no skip set drops nothing");

    // N3: the skip set binds into the match identity; none leaves it unchanged.
    eawr::battle::BattleStart bound;
    bound.identity = eawr::sim::compute_match_identity({});
    bound.setup.match_identity = bound.identity;
    const auto before = bound.identity.sha256;
    eawr::skirmish::bind_skipped_types(bound, {});
    check(bound.identity.sha256 == before, "an empty skip set leaves the identity");
    eawr::skirmish::bind_skipped_types(bound, ids);
    const auto first = bound.identity.sha256;
    check(first != before && bound.setup.match_identity && bound.setup.match_identity->sha256 == first,
        "a skip set changes the start's and the setup's identity alike");
    eawr::battle::BattleStart other;
    other.identity = eawr::sim::compute_match_identity({});
    eawr::skirmish::bind_skipped_types(other, std::vector<std::string>{"Another_Type"});
    check(other.identity.sha256 != first, "peers with different skip sets get different identities");
    eawr::skirmish::bind_skipped_types(bound, ids);
    check(bound.identity.sha256 == first, "binding the same set again is stable");

    return checker.finish();
}

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>
#include "../support/check.hpp"
#include "eawr/sim/galactic/hero_actions.hpp"

#include <algorithm>
#include <iostream>
#include <exception>

namespace h = eawr::sim::galactic::hero_actions;
using h::Fixed;
using h::Id;
using h::Effect;
using h::Refusal;
namespace {
void check(bool value, const char* message) {
    if (!value) eawr::testing::require(value, message);
}
Fixed f(const char* value) { return Fixed::from_decimal(value).value(); }
bool near(Fixed a, Fixed b) {
    const auto d = eawr::sim::math::subtract(a, b).value().raw();
    return d > -2000 && d < 2000; // Fixed quantization, below 0.00012 credit.
}
bool has(const h::Outcome& r, Effect e) {
    return std::any_of(r.events.begin(), r.events.end(), [e](const auto& v) { return v.effect == e; });
}
const h::Event& event(const h::Outcome& r, Effect e) {
    const auto it = std::find_if(r.events.begin(), r.events.end(), [e](const auto& v) { return v.effect == e; });
    check(it != r.events.end(), "expected effect missing"); return *it;
}
std::size_t position(const h::Outcome& r, Effect e) {
    return static_cast<std::size_t>(&event(r, e) - r.events.data());
}
h::Fixture fixture() {
    h::Fixture w;
    for (Id id : {1U, 2U, 3U}) {
        h::Account a; a.base.player.id = id; a.base.player.faction = id;
        a.base.player.credits = f("100000"); a.base.local = id == 1; a.tech = 3;
        w.accounts.push_back(a);
    }
    h::Object actor; actor.id = 10; actor.owner = 1; actor.current_type = 100;
    actor.original_type = 100; actor.hero = true; actor.named = true; actor.parent = 11;
    w.objects.push_back(actor);
    h::Object transport; transport.id = 11; transport.owner = 1; transport.transport = true;
    transport.original_type = 101; transport.contents = {10}; w.objects.push_back(transport);
    h::Object victim = actor; victim.id = 20; victim.owner = 2; victim.current_type = 200;
    victim.original_type = 201; victim.parent = 21; victim.neutralization_cost = f("100"); w.objects.push_back(victim);
    transport.id = 21; transport.owner = 2; transport.original_type = 202; transport.contents = {20}; w.objects.push_back(transport);
    h::Object fleet; fleet.id = 22; fleet.owner = 2; fleet.fleet = true; fleet.contents = {21}; w.objects.push_back(fleet);
    h::Planet p; p.id = 30; p.current_allegiance = 2; p.model.target_allegiance = 2;
    p.model.transition.current = eawr::sim::galactic::corruption_fixture::Type::piracy;
    p.ground_base = 2; p.model.starbase_level = 5; p.land_capacity = 10;
    p.orbit = {22}; p.landed = {11}; p.ground = {{40, f("100"), f("25")}};
    p.space = {{41, f("100"), f("30")}}; w.planets.push_back(p);
    return w;
}
h::NeutralizationAbility neutral_ability() {
    // Stock named neutralizer ranges and curves, WGCH catalog/21..23.
    h::NeutralizationAbility a; a.major = true; a.minor = true;
    a.actor_absence = {f("120"), f("150")}; a.target_absence = {f("240"), f("300")};
    a.default_absence = f("360");
    a.base_curve = [](unsigned level) { return Fixed::from_ratio(level, 5).value(); };
    a.history_curve = [](unsigned count) { return Fixed::from_ratio(static_cast<std::int64_t>(count) * 3, 4).value(); };
    a.corruption_curve = [](unsigned corrupted) { return Fixed::from_ratio(corrupted, 4).value(); };
    return a;
}
h::NeutralizationPreparation prepare(const h::Fixture& w, const h::NeutralizationAbility& a = neutral_ability(), std::size_t count = 1) {
    std::vector<std::pair<Fixed, Fixed>> draws(count, {f("0.5"), f("0.5")});
    return h::prepare_neutralization(w, 10, 30, 999, a, draws).value();
}
void neutralization() {
    auto w = fixture(); auto a = neutral_ability();
    w.planets[0].protection = {{20, 1, f("100")}, {80, 1, f("1.5")}, {81, 1, f("2")}, {82, 2, f("1.25")}};
    w.neutralizations[{202, 1}] = 2;
    auto prepared = prepare(w, a); const auto choice = prepared.choices[0];
    // 100 + 100 base + 150 protection + 150 history - 25 corruption.
    check(choice.price == f("475") && choice.valid, "additive cost, category max/product, self source removed, company history");
    check(choice.actor_absence == f("135") && choice.target_absence == f("270"), "separate stock range samples");
    w.planets[0].current_allegiance = 3; check(prepare(w).choices[0].price == f("375"), "nonallied controller uses level zero");
    w.accounts[1].base.allies.insert(3); check(prepare(w).choices[0].price == f("475"), "controller alliance restores orbital base");
    w = fixture(); w.planets[0].orbit.clear(); w.planets[0].landed = {21};
    check(near(prepare(w).choices[0].price, f("115")), "ground base for landed victim");
    a.corruption_curve = [](unsigned) { return f("999"); }; check(prepare(w, a).choices[0].price == h::one, "one credit floor");
    w = fixture(); a = neutral_ability();
    for (bool actor_named : {false, true}) for (bool target_named : {false, true})
        for (bool ability_flag : {false, true}) for (bool permission : {false, true}) {
            w.objects[0].named = actor_named; w.objects[2].named = target_named;
            a.major = target_named ? ability_flag : true; a.minor = target_named ? true : ability_flag;
            w.objects[2].major_allowed = actor_named ? permission : true;
            w.objects[2].minor_allowed = actor_named ? true : permission;
            check(prepare(w, a).choices[0].valid == (ability_flag && permission), "two-sided major/minor matrix");
        }
    w = fixture(); a = neutral_ability(); a.actor_absence.low = {}; a.target_absence.high = f("-1");
    auto defaults = prepare(w, a).choices[0];
    check(defaults.actor_absence == f("360") && defaults.target_absence == f("360"), "nonpositive range endpoint defaults");
    w = fixture(); a = neutral_ability(); a.major = false;
    auto disabled = prepare(w, a); check(!h::select_neutralization_ai(w, disabled, 0), "disabled AI option excluded after preparation");
    w = fixture(); auto c = prepare(w).choices[0];
    check(h::select_neutralization_ai(w, prepare(w), 0).has_value() && !h::select_neutralization_ai(w, prepare(w), 1), "uniform neutralization choice index bounded");
    w.objects[0].owner = 3; w.objects[2].owner = 1; w.objects[2].current_type = 250;
    w.planets[0].protection.clear(); w.planets[0].model.transition.current = {};
    auto r = h::neutralize(w, c).value();
    check(r.success && w.accounts[2].base.player.credits == f("99825") && w.accounts[0].base.player.credits == f("100000"), "current actor owner pays prepared price, stale hostility/type not rechecked");
    check(event(r, Effect::attribution).type == 250 && event(r, Effect::schedule).bypass_parent, "current victim attribution and target scheduling bypass");
    check(!w.objects[0].present && !w.objects[1].present && !w.objects[2].present && !w.objects[3].present && w.objects[4].present, "immediate transports removed, enclosing fleet retained");
    check(position(r, Effect::neutralized) < position(r, Effect::debit) && w.neutralizations[{202, 3}] == 1, "scoring before debit and current attacker history");
    w = fixture(); c = prepare(w).choices[0]; w.objects[0].named = false; w.planets[0].auto_capture_minor = true;
    r = h::neutralize(w, c).value(); check(!r.success && r.refusal == Refusal::captured && w.objects[2].present && w.objects[1].present && !has(r, Effect::debit), "generic capture false, direct actor removal only");
    for (auto reason : {Refusal::actor, Refusal::planet, Refusal::target, Refusal::invalid_option, Refusal::credits}) {
        w = fixture(); c = prepare(w).choices[0];
        if (reason == Refusal::actor) w.objects[0].present = false;
        if (reason == Refusal::planet) w.planets[0].planetary = false;
        if (reason == Refusal::target) w.objects[2].present = false;
        if (reason == Refusal::invalid_option) c.valid = false;
        if (reason == Refusal::credits) w.accounts[0].base.player.credits = {};
        r = h::neutralize(w, c).value(); check(r.refusal == reason && r.events.empty(), "neutralization stale/refusal before mutation");
    }
    w = fixture(); auto companion = w.objects[2]; companion.id = 23; w.objects.push_back(companion);
    w.objects[3].contents.push_back(23); w.objects[2].hidden_details = true;
    check(prepare(w, neutral_ability(), 0).refusal == Refusal::no_choices, "rejected first transport hero hides later hero");
    w = fixture(); w.objects[4].current_type = 999;
    check(prepare(w, neutral_ability(), 0).refusal == Refusal::superweapon, "hackable exact-type weapon blocks ordinary neutralization");
    w = fixture(); w.planets[0].orbit.clear(); auto& s = w.planets[0].siphon;
    s.fiscal = {true, 2, f("0.5"), 100}; s.source_type = 201; s.company_type = 202; s.neutralization_cost = f("100");
    c = prepare(w).choices[0]; check(c.siphon, "active siphon target outlives deployment hero");
    s.source_type = 300; s.company_type = 301; s.fiscal.recipient = 3;
    r = h::neutralize(w, c).value();
    check(r.success && event(r, Effect::neutralized).type == 202 && event(r, Effect::end_siphon).type == 300 && !s.fiscal.active, "replacement siphon allowed, prepared score/company and current source feedback");
    w = fixture(); w.planets[0].siphon.fiscal.active = false;
    check(h::neutralize(w, c).value().refusal == Refusal::target, "vanished siphon refusal before payment");
}
void sabotage() {
    auto w = fixture();
    auto choices = h::prepare_sabotage(w, 10, 30, f("2"), false).value();
    check(choices.size() == 2 && choices[0].structure == 40 && choices[1].structure == 41 && choices[0].absence == Fixed{}, "ground then space stock menu and zero absence");
    auto r = h::sabotage(w, choices[0], h::SabotageRoute::prepared).value();
    check(r.success && w.planets[0].sabotage_count == 1 && has(r, Effect::reevaluate_ground) && event(r, Effect::schedule).value == Fixed{}, "first ordinary sabotage and default timer request");
    w.objects[0].present = true; w.objects[1].present = true;
    choices = h::prepare_sabotage(w, 10, 30, f("2"), false).value();
    check(choices.size() == 1 && choices[0].price == f("200"), "second sabotage escalates prior count");
    r = h::sabotage(w, choices[0], h::SabotageRoute::prepared).value();
    check(r.success && has(r, Effect::reevaluate_space) && w.accounts[0].base.player.credits == f("99700"), "second space sabotage debit");
    check(h::sabotage_price(f("1.25"), f("2"), 1).value() == f("3"), "nearest integer half upward");
    check(!h::sabotage_price(f("0.9"), f("2"), 0) && !h::sabotage_price(f("1"), f("-2"), 1), "ordinary invalid costs rejected");
    w = fixture(); w.planets[0].sabotage_count = 9;
    check(h::prepare_sabotage(w, 10, 30, f("2"), true).value()[0].price == f("25"), "tutorial direct authored price ignores escalation");
    w = fixture(); auto c = h::prepare_sabotage(w, 10, 30, f("2"), false).value()[0];
    w.objects[0].owner = 3; w.planets[0].model.transition.current = {}; c.absence = f("-1");
    r = h::sabotage(w, c, h::SabotageRoute::prepared).value();
    check(r.success && w.accounts[2].base.player.credits == f("99900") && !has(r, Effect::schedule), "current owner, no corruption recheck, negative timer unscheduled");
    w = fixture(); w.planets[0].ground[0].removal_accepts = false;
    r = h::sabotage(w, c, h::SabotageRoute::prepared).value();
    check(!r.success && r.refusal == Refusal::removal && has(r, Effect::reevaluate_ground) && !has(r, Effect::debit) && w.planets[0].sabotage_count == 0, "failed removal reevaluates layer without success mutations");
    w.planets[0].space.push_back(w.planets[0].ground[0]); w.planets[0].space.back().removal_accepts = true;
    check(!h::sabotage(w, c, h::SabotageRoute::prepared).value().success && w.planets[0].space.back().present, "ground membership suppresses space fallback");
    c.structure = 900; r = h::sabotage(w, c, h::SabotageRoute::prepared).value();
    check(r.refusal == Refusal::target && r.events.empty(), "exact structure identity missing, no reevaluation");
    w = fixture(); c.structure = 40; c.actor = 900; c.price = f("1000000");
    r = h::sabotage(w, c, h::SabotageRoute::free_script).value();
    check(r.success && w.objects[0].present && !has(r, Effect::debit) && event(r, Effect::sabotage_story).player == 0 && w.planets[0].sabotage_count == 1, "free script no actor lookup/payment, unattributed history");
    c.price = f("-1"); check(h::sabotage(w, c, h::SabotageRoute::free_script).value().refusal == Refusal::cost, "free action still rejects negative supplied price");
    w = fixture(); w.accounts[0].base.player.credits = f("99.6");
    check(h::menu_affordable(f("100"), f("99.6")).value(), "fractional local menu rounds up");
    c = {10, 30, 40, f("100"), {}};
    check(h::sabotage(w, c, h::SabotageRoute::prepared).value().refusal == Refusal::credits, "execution uses exact current credits");
    check(h::sabotage(w, {10, 30, 40, f("100"), {}}, h::SabotageRoute::prepared).value().events.empty(), "sabotage credit refusal has no success effects");
    w = fixture(); w.planets[0].ground[0].deletion_pending = true; w.planets[0].revealed = false;
    r = h::sabotage_ai(w, 10, 30, f("2"), 0).value();
    check(r.success && w.planets[0].ground[0].present && !w.planets[0].space[0].present && w.objects[0].present && w.accounts[0].base.player.credits == f("100000") && w.accounts[0].frame_credit_income == f("-100") && !has(r, Effect::sabotage_effect), "AI skips pending, posts fiscal debit, hidden feedback suppressed");
    h::consume_success(w, 10, true, f("360"), r); check(r.consumed && !w.objects[0].present && w.objects[1].present, "generic host consumes only actor after AI success");
    w = fixture(); w.objects[0].named = false; w.planets[0].auto_capture_minor = true;
    r = h::sabotage_ai(w, 10, 30, f("2"), 0).value(); check(r.success && r.refusal == Refusal::captured && w.planets[0].ground[0].present, "legacy sabotage auto-capture returns true");
}
h::TheftAbility theft_ability() {
    // R2-D2 stock authored curves/timers, WGCH-33/36 and catalog.
    h::TheftAbility a; a.availability = [](unsigned x) { return eawr::sim::math::subtract(f("1.75"), Fixed::from_ratio(x, 4).value()).value(); };
    a.ground = [](unsigned) { return Fixed{}; }; a.space = a.ground; a.difficulty = a.ground;
    a.affinity_price = f("-0.25"); a.affinity_chance = f("0.25");
    a.absence_min = {f("60"), f("60"), f("60"), f("70"), f("70")}; a.absence_max = a.absence_min;
    return a;
}
void theft() {
    auto w = fixture(); auto a = theft_ability();
    std::vector<h::Technology> types{{50, 1, 3, true, true, false, f("100")}, {51, 1, 2, true, true, true, f("200")}};
    w.planets[0].tech_rating = 3; a.affinity_types.insert(50);
    auto p = h::prepare_theft(w, 10, 30, types, a, f("0.5")).value();
    check(p.choices.size() == 2 && p.choices[0].price == f("75") && p.choices[1].price == f("200") && p.choices[0].chance == h::one && p.choices[0].absence == f("70") && !p.choices[0].advance, "stock registered order, exact affinity, chance clamp, per-tech timers");
    for (unsigned tech = 0; tech < 6; ++tech) {
        w.accounts[0].tech = tech; types[0].tech = 0;
        auto options = h::prepare_theft(w, 10, 30, std::span<const h::Technology>(types.data(), 1), a, {}).value();
        check(options.choices[0].absence == (tech < 3 ? f("60") : tech < 5 ? f("70") : Fixed{}), "timer index 0..4 only");
    }
    w = fixture(); auto filtered = types;
    auto extra = types[0]; extra.id = 60; extra.faction = 2; filtered.push_back(extra);
    extra.id = 61; extra.faction = 1; extra.tech = 4; filtered.push_back(extra);
    extra.id = 62; extra.tech = 1; extra.initially_locked = false; filtered.push_back(extra);
    extra.id = 63; extra.initially_locked = true; extra.slicer_unlock = false; filtered.push_back(extra);
    extra.id = 64; extra.slicer_unlock = true; filtered.push_back(extra); w.accounts[0].locked.insert(64);
    extra.id = 65; filtered.push_back(extra); w.accounts[0].unlocked.insert(65);
    check(h::prepare_theft(w, 10, 30, filtered, a, {}).value().choices.size() == 2, "all registered-type filters");
    w.accounts[0].unlocked.insert(50); w.accounts[0].unlocked.insert(51);
    check(h::prepare_theft(w, 10, 30, filtered, a, {}).value().refusal == Refusal::no_choices && w.accounts[0].tech == 3, "initial empty list rejects without free tech advancement");
    w = fixture(); w.planets[0].model.target_allegiance = 1;
    check(h::prepare_theft(w, 10, 30, types, a, {}).value().refusal == Refusal::opposing, "target allegiance hostility");
    w.planets[0].model.target_allegiance = 2; w.accounts[1].playable = false;
    check(h::prepare_theft(w, 10, 30, types, a, {}).value().refusal == Refusal::playable, "current controller playable faction");
    w = fixture(); a = theft_ability(); a.ground = [](unsigned level) { return level == 2 ? f("-0.5") : Fixed{}; };
    a.space = [](unsigned level) { return level == 5 ? f("-0.75") : Fixed{}; };
    a.difficulty = [](unsigned) { return f("-0.1"); };
    p = h::prepare_theft(w, 10, 30, types, a, {}).value();
    check(p.choices[0].chance == eawr::sim::math::add(f("0.5"), f("-0.1")).value() && p.choices[1].chance == eawr::sim::math::add(f("0.25"), f("-0.1")).value(), "layer-specific bases and difficulty");
    a.availability = [](unsigned) { return f("-1"); };
    check(h::prepare_theft(w, 10, 30, types, a, {}).value().choices[0].price == Fixed{}, "zero-priced options retained");
    auto c = p.choices[0]; c.price = f("100"); c.chance = f("0.5"); c.advance = true; c.absence = f("60");
    for (unsigned failure = 1; failure <= 3; ++failure) {
        w = fixture(); auto r = h::steal(w, c, f("0.5"), failure).value();
        check(!r.success && r.consumed && !w.objects[1].present && !w.accounts[0].unlocked.contains(50), "strict success boundary, failure transport removal");
        check(event(r, Effect::schedule).value == (failure == 1 ? f("120") : f("60")) && event(r, Effect::schedule).bypass_parent, "failure timers and parent bypass");
        check(w.accounts[0].base.player.credits == (failure == 3 ? f("99900") : f("100000")), "three failure payments");
    }
    w = fixture(); w.objects[0].owner = 3; w.accounts[2].unlocked.insert(50);
    auto r = h::steal(w, c, f("0.499"), 1).value();
    check(r.success && w.accounts[2].base.player.credits == f("99900") && w.accounts[2].tech == 4, "current owner stale already-unlocked prepared theft still debits/advances");
    for (unsigned level : {4U, 5U}) {
        w = fixture(); w.accounts[0].tech = level; w.accounts[0].maximum_tech = 4;
        r = h::steal(w, c, {}, 1).value();
        check(r.success && r.consumed && w.accounts[0].tech == level && w.accounts[0].unlocked.contains(50)
            && w.accounts[0].base.player.credits == f("99900") && !has(r, Effect::advance_tech),
            "WGEP-148 at/above maximum still debits, unlocks and consumes without tech increment");
    }
    w = fixture(); w.objects[0].named = false; w.planets[0].auto_capture_minor = true;
    r = h::steal(w, c, {}, 1).value(); check(!r.success && !has(r, Effect::debit) && event(r, Effect::schedule).value == Fixed{}, "slicer capture zero default request without purchase");
    w = fixture(); a = theft_ability(); p = h::prepare_theft(w, 10, 30, std::span<const h::Technology>(types.data(), 1), a, {}).value();
    check(p.choices[0].advance && h::select_theft_ai(w, p, 0)->advance, "whole prepared list one-item advance flag");
    w.accounts[0].base.player.credits = {}; check(!h::select_theft_ai(w, p, 0), "no affordable AI theft leaves actor");
    check(h::steal(w, c, {}, 1).value().refusal == Refusal::credits && w.objects[0].present, "stale theft insufficient credits before consumption");
    w = fixture(); w.planets[0].planetary = false;
    check(h::steal(w, c, {}, 1).value().refusal == Refusal::planet, "theft command target re-resolved");
    w = fixture(); a.absence_min.clear();
    check(h::prepare_theft(w, 10, 30, types, a, {}).value().choices[0].absence == Fixed{}, "empty respawn array uses default-time request");
}
void siphon_and_spy() {
    auto w = fixture(); h::SiphonAbility a{{f("0.5"), f("0.5")}, 5};
    w.objects[0].neutralization_cost = f("100"); w.objects[0].major_allowed = false;
    w.planets[0].siphon.neutralization_cost = f("999");
    auto r = h::deploy_siphon(w, 10, 30, a, {}, 100, 1350).value();
    check(r.success && w.objects[0].present && !has(r, Effect::debit), "stock smuggler five-cycle deployment no action debit");
    auto& s = w.planets[0].siphon;
    check(s.fiscal.end == 6850 && s.fiscal.recipient == 1 && s.fiscal.fraction == f("0.5") && s.source_type == 100 && s.company_type == 101 && s.accumulated == Fixed{} && !s.discovered, "copied source/transport/fiscal metadata");
    check(s.neutralization_cost == f("100") && s.named && !s.major_allowed, "deployment replaces prior neutralization source metadata");
    h::consume_success(w, 10, true, f("300"), r); check(r.consumed && s.fiscal.active && s.company_type == 101, "siphon identity outlives consumption");
    w = fixture(); a = {{f("0.45"), f("0.65")}, 2};
    r = h::deploy_siphon(w, 10, 30, a, f("0.5"), 100, 1350).value();
    check(r.success && w.planets[0].siphon.fiscal.end == 2800 && w.planets[0].siphon.fiscal.fraction == eawr::sim::math::add(f("0.45"), eawr::sim::math::multiply(eawr::sim::math::subtract(f("0.65"), f("0.45")).value(), f("0.5")).value()).value(), "Han authored range/two cycles");
    check(h::deploy_siphon(w, 10, 30, a, f("2"), 0, 1).value().refusal == Refusal::siphon, "existing siphon refusal before invalid draw");
    w.objects[0].owner = 3; r = h::remove_siphon(w, 10, 30);
    check(!r.success && w.planets[0].siphon.fiscal.active && has(r, Effect::deregister_target), "owner mismatch deregisters without terminating");
    w.objects[0].owner = 1; check(h::remove_siphon(w, 10, 30).success && !w.planets[0].siphon.fiscal.active, "recipient identity removal");
    w = fixture(); w.objects[0].named = false; w.planets[0].prevent_minor_siphon = true;
    check(h::deploy_siphon(w, 10, 30, a, f("2"), 0, 1).value().refusal == Refusal::prevented, "minor prevention before sampling");
    w.planets[0].prevent_minor_siphon = false; w.planets[0].auto_capture_minor = true;
    r = h::deploy_siphon(w, 10, 30, a, f("2"), 0, 1).value(); check(r.refusal == Refusal::captured && !has(r, Effect::register_target), "ground siphon capture precedes sample/helper");
    w = fixture(); w.objects[0].parent = 0;
    r = h::deploy_siphon(w, 10, 30, a, {}, 0, 1).value();
    check(!r.success && r.refusal == Refusal::transport && has(r, Effect::register_target) && !has(r, Effect::ability_effect), "uncontained source helper fails but registers target");
    h::consume_success(w, 10, true, f("300"), r); check(w.objects[0].present, "failed service cannot authorize consumption");
    w = fixture(); check(!h::deploy_siphon(w, 10, 30, a, {}, std::numeric_limits<h::Frame>::max(), 1), "siphon deadline overflow diagnostic");
    w = fixture(); h::Spy probe{h::Activation::ground, 7, f("60"), true};
    r = h::spy(w, 10, 30, probe);
    check(r.success && has(r, Effect::hero_land) && has(r, Effect::register_target) && event(r, Effect::spy_insert).player == 1 && event(r, Effect::spy_insert).value == f("60") && w.objects[0].present, "probe source/current owner/duration, invocation separated from consumption");
    check(std::count_if(r.events.begin(), r.events.end(), [](const auto& e) { return e.effect == Effect::refresh_visibility; }) == 3, "all player visibility refreshed");
    r = h::spy(w, 10, 30, probe); check(r.success && w.planets[0].spy_sources.count(10) == 2 && has(r, Effect::refresh_visibility), "WGUI-23 repeated source insertion appends and refreshes");
    probe.unique_source = 600; r = h::spy(w, 10, 30, probe);
    check(r.success && event(r, Effect::spy_insert).source == 600 && w.planets[0].spy_sources.contains(600), "source-keyed sensor invocation carries distinct ability identity");
    w = fixture(); probe.insertion_accepts = false;
    r = h::spy(w, 10, 30, probe); check(!r.success && has(r, Effect::register_target) && !has(r, Effect::refresh_visibility) && w.objects[0].present, "external visibility service refusal still registers without consuming/refreshing");
    w = fixture(); w.planets[0].model.target_allegiance = 1;
    check(h::spy(w, 10, 30, probe).refusal == Refusal::opposing, "ground spy opposing gate");
    probe = {h::Activation::automatic, 7, f("-1"), true};
    r = h::spy(w, 10, 30, probe); check(r.success && !has(r, Effect::hero_land) && event(r, Effect::spy_insert).value == f("-1"), "passive spy indefinite duration on friendly target");
    w.objects[0].planetary = true; w.objects[0].destroyed = true;
    check(h::spy(w, 10, 30, probe).refusal == Refusal::destroyed, "destroyed planetary automatic source refused");
    w.objects[0].id = 30; check(h::spy(w, 30, 30, probe).success, "planetary self spy passes special gate");
    w = fixture(); check(h::galactic_stealth(w, 10, true).success && w.objects[0].stealth, "stealth apply");
    check(h::galactic_stealth(w, 10, false).success && !w.objects[0].stealth, "stealth removal");
}
void hacking_and_destruction() {
    auto with_weapon = [] {
        auto w = fixture(); h::Object weapon; weapon.id = 60; weapon.current_type = 999; weapon.owner = 2;
        w.objects.push_back(weapon); w.objects[4].contents.insert(w.objects[4].contents.begin(), 60); return w;
    };
    auto w = with_weapon(); h::Hack c{10, 30, 999, f("50000")};
    auto r = h::invoke_hack(w, c, 30, 100, 3).value();
    check(!r.success && has(r, Effect::prepare_menu) && w.objects[0].present, "local human opens hack menu without action");
    w.accounts[0].base.local = false;
    check(h::invoke_hack(w, c, 30, 100, 3).value().refusal == Refusal::remote, "remote non-AI no action");
    w.accounts[0].base.ai = true; w.accounts[0].human = false; w.accounts[0].base.player.credits = {};
    r = h::invoke_hack(w, c, 30, 100, 3).value();
    check(r.success && w.accounts[0].base.player.credits == Fixed{} && !has(r, Effect::debit) && !has(r, Effect::hack_sound) && !has(r, Effect::hack_particle) && !has(r, Effect::schedule), "AI hacking no affordability/debit or human emissions or respawn");
    check(w.accounts[1].locked.contains(999) && !w.objects[0].present && !w.objects[1].present && w.planets[0].destroyed && w.planets[0].orbit.empty() && w.planets[0].model.starbase_level == 0 && w.planets[0].current_allegiance == 3, "hack lock, actor transport and orbital/neutral mutations");
    check(has(r, Effect::corruption_removal) && !w.planets[0].model.transition.active() && w.planets[0].model.transition.current == eawr::sim::galactic::corruption_fixture::Type::none && position(r, Effect::corruption_removal) < position(r, Effect::abort_corruption), "five-second removal immediately aborted by ruins");
    w = with_weapon(); r = h::hack(w, c, h::HackRoute::human_prepared, 30, 100, 3).value();
    check(r.success && w.accounts[0].base.player.credits == f("50000") && position(r, Effect::debit) < position(r, Effect::build_lock) && position(r, Effect::build_lock) < position(r, Effect::hack_sound) && position(r, Effect::hack_sound) < position(r, Effect::remove_object) && position(r, Effect::neutral_ownership) < position(r, Effect::hack_particle), "human hack ordered payment/sound/particle");
    w = with_weapon(); w.objects.back().owner = 900;
    r = h::hack(w, c, h::HackRoute::human_prepared, 30, 0, 3).value();
    check(!r.success && r.refusal == Refusal::owner && w.accounts[0].base.player.credits == f("50000") && w.objects[0].present, "missing weapon owner fails after human debit without refund");
    w = with_weapon(); w.accounts[0].base.player.credits = f("49999");
    check(h::hack(w, c, h::HackRoute::human_prepared, 30, 0, 3).value().refusal == Refusal::credits && w.objects[0].present, "human hack insufficient funds before payment");
    w = with_weapon(); w.objects.back().present = false;
    check(h::hack(w, c, h::HackRoute::human_prepared, 30, 0, 3).value().refusal == Refusal::superweapon && w.accounts[0].base.player.credits == f("100000"), "missing exact match before payment");
    h::Object replacement; replacement.id = 61; replacement.current_type = 999; replacement.owner = 3;
    w.objects.push_back(replacement); w.objects[4].contents.push_back(61);
    r = h::hack(w, c, h::HackRoute::human_prepared, 30, 0, 900).value();
    check(r.success && w.accounts[2].locked.contains(999) && !has(r, Effect::neutral_ownership), "replacement current exact type qualifies; absent Neutral does not undo success");
    w = with_weapon(); w.planets[0].destroyed = true; w.planets[0].model.transition.current = {};
    r = h::hack(w, c, h::HackRoute::direct_ai, 30, 0, 3).value();
    check(r.success && !has(r, Effect::corruption_removal), "hack does not use ruins/common-opposing predicate");
    w = fixture(); w.objects[0].contested = true;
    check(h::destroy_planet(w, 10, 30, true).refusal == Refusal::contested, "contested visible hostile orbit fleet blocks");
    w.objects[4].stealth = true; w.accounts[0].leader_kill_victory = true; w.accounts[1].leader_type = 100;
    w.planets[0].model.political_modifiers = {h::one}; w.planets[0].siphon.fiscal.active = true;
    r = h::destroy_planet(w, 10, 30, true);
    check(r.success && event(r, Effect::pending_victory).value == f("30") && event(r, Effect::pending_victory).type == 100, "exact current leader in landed enemy-owner transport, logical frame request");
    check(w.planets[0].destroyed && w.planets[0].land_capacity == 0 && !w.planets[0].siphon.fiscal.active && w.planets[0].model.political_modifiers.empty() && !w.planets[0].ground[0].present && w.planets[0].space[0].present, "shared ruins ground/siphon/political cleanup");
    check(w.objects[0].present && !w.objects[1].present && w.objects[4].present && w.planets[0].model.starbase_level == 5 && w.planets[0].current_allegiance == 2 && !has(r, Effect::debit) && !has(r, Effect::build_lock), "destruction handler no hack-specific orbital/payment/actor operations; landed host handles companion cleanup");
    w = fixture(); w.accounts[0].leader_kill_victory = true; w.accounts[1].leader_type = 101;
    r = h::destroy_planet(w, 10, 30, true); check(r.success && !has(r, Effect::pending_victory), "original/company type is not leader current type");
    w = fixture(); w.planets[0].model.target_allegiance = 1;
    check(h::destroy_planet(w, 10, 30, true).refusal == Refusal::opposing, "destruction target allegiance ally gate");
    w.planets[0].model.target_allegiance = 900;
    r = h::destroy_planet(w, 10, 30, false); check(r.success && !has(r, Effect::pending_victory) && !has(r, Effect::terminate_abilities), "invalid target allegiance accepted, nongalactic victory/service omitted");
    w = fixture(); r = h::political_protection(w, 10, 30, true);
    check(r.success && event(r, Effect::political_protection).value == f("-1"), "unique indefinite true political modifier interface");
    check(h::political_protection(w, 20, 30, true).success && w.planets[0].political_sources.size() == 2, "distinct sources coexist");
    check(h::political_protection(w, 10, 30, false).success && !w.planets[0].political_sources.contains(10) && w.planets[0].political_sources.contains(20), "remove only matching protection source");
    check(h::political_protection(w, 10, 30, true, 700).success && h::political_protection(w, 10, 30, true, 701).success && w.planets[0].political_sources.contains(700) && w.planets[0].political_sources.contains(701), "distinct abilities on one actor retain unique sources");
    check(h::political_protection(w, 10, 30, false, 700).success && w.planets[0].political_sources.contains(701), "ability source removal preserves sibling");
    check(!h::political_protection(w, 10, 30, true, 702, false).success, "host appropriation refusal");
}
} // namespace
int case_galactic_hero_actions_tests() {
    try {
        neutralization(); sabotage(); theft(); siphon_and_spy(); hacking_and_destruction();
        std::cout << "galactic hero action contracts passed (stock-derived WGCH fixtures)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

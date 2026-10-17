#include "eawr/sim/galactic/ai_funding.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace f = eawr::sim::galactic::funding;
using A = f::Amount;
int failures{};
A n(std::int64_t value) { return A::from_integer(value).value(); }
A ratio(std::int64_t a, std::int64_t b) { return A::from_ratio(a, b).value(); }
void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class T> T must(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
template<class F> void rejects(F call, std::string_view code, const char* message) {
    const auto result = call();
    expect(!result && result.error().code == code && !result.error().message.empty(), message);
}
f::Ledger seeded(A credits = n(100)) {
    auto l = must(f::set_equation({}, 1, n(1), 1));
    l = must(f::set_equation(l, 2, n(1), 1));
    return must(f::initialize(l, credits));
}
f::GoalRequest request(f::GoalId id, A cost, A seconds = n(10)) {
    return {id, 1, cost, seconds, std::nullopt, {}};
}
void allocation() {
    auto l = seeded();
    expect(l.categories.at(1).cash == n(50) && l.categories.at(2).available_cash == n(50), "WGA-11 initial allocation");
    l = must(f::set_equation(l, 1, n(-4), 1));
    l = must(f::set_equation(l, 2, n(0), 1));
    l = must(f::set_allowance(l, 3, n(0), 1));
    l = must(f::initialize(l, n(120)));
    expect(l.categories.size() == 3 && l.categories.at(3).cash.raw() > 0, "WGA-68/69 zero weights include allowance-only category");
    auto sum = eawr::sim::math::add(l.categories.at(1).cash, l.categories.at(2).cash).value();
    sum = eawr::sim::math::add(sum, l.categories.at(3).cash).value();
    expect(sum == n(120), "fixed-point residual conserves capital");
    const auto all_zero = l;
    const auto mixed = must(f::set_equation(l, 1, n(1), 2));
    const auto mixed_before = mixed;
    const auto maintained = must(f::begin_maintenance(mixed));
    expect(maintained.categories.at(1).share == n(1) && maintained.categories.at(2).share == n(0)
        && maintained.categories.at(3).share == n(0) && !maintained.categories.at(3).evaluated_weight,
        "WGA-421 maintenance resets allowance-only share before normalization without inventing an equation");
    expect(l == all_zero && mixed == mixed_before, "maintenance leaves immutable inputs intact");
    for (const auto& [id, category] : maintained.categories)
        expect(category.cash == mixed.categories.at(id).cash && category.previous_share == mixed.categories.at(id).previous_share,
            "share reevaluation preserves retained cash and prior income share");
    l = must(f::set_equation(l, 3, n(0), 1));
    auto empty = must(f::initialize({}, n(100)));
    expect(empty.categories.empty(), "WGA-69 empty map creates no category");
    rejects([&] { return f::set_equation(empty, 9, std::nullopt, 1); }, f::diagnostic_codes::missing_category, "WGA-68 null cannot invent category");
    l = must(f::set_equation(l, 1, n(5), 4));
    l = must(f::set_equation(l, 1, n(7), 3));
    expect(l.categories.at(1).evaluated_weight == n(5), "WGA-68 lower equation priority rejected");
    l = must(f::set_equation(l, 1, n(8), 4));
    l = must(f::set_allowance(l, 1, n(2), 2));
    expect(l.categories.at(1).evaluated_weight == n(8) && l.categories.at(1).allowance == n(2), "WGA-68 equal overwrites, independent unclamped allowance");
    l = must(f::begin_maintenance(l));
    expect(must(f::category_order(l)).front() == 1, "WGA-75 category share ordering");
}
void reservations() {
    auto l = seeded();
    l.last_interval = n(10);
    l.categories.at(1).income = l.categories.at(1).available_income = n(5);
    const auto r = request(1, n(100));
    expect(must(f::feasible(l, r, false)) && must(f::feasible(l, r, true)), "WGA-21 total and available initial feasibility");
    l = must(f::reserve(l, r));
    const auto g = l.goals.at(1);
    expect(g.capital == n(50) && g.reserved_income == n(5), "WGA-22 reserves capital before income");
    expect(must(f::feasible(l, request(2, n(50)), false)) && !must(f::feasible(l, request(2, n(50)), true)), "WGA-21 oversubscribed proposals differ from activation");
    rejects([&] { return f::reserve(l, request(2, n(50))); }, f::diagnostic_codes::insufficient_funding, "oversubscribed activation rejected");
    expect(must(f::reserve(l, r)) == l, "WGA-22 once per reassessment pass");
    auto fiscal = must(f::fiscal(l, n(200), n(100), n(10)));
    expect(fiscal.goals.at(1).accumulated == n(50) && fiscal.goals.at(1).remaining == n(0), "WGA-14 fiscal accumulation and time clamp");
    expect(fiscal.categories.at(1).cash == n(100) && fiscal.categories.at(1).available_cash == n(0), "WGA-14/15 funded cash protected");
    fiscal = must(f::begin_maintenance(fiscal));
    expect(fiscal.goals.at(1).capital == n(0) && fiscal.goals.at(1).accumulated == n(50), "WGA-12 savings survive reassessment");
    fiscal = must(f::reserve(fiscal, request(1, n(40))));
    expect(fiscal.goals.at(1).accumulated == n(40) && fiscal.categories.at(1).available_cash == n(60), "WGA-22 surplus returned");
    auto horizon = seeded(); horizon.last_interval = n(10);
    horizon = must(f::reserve(horizon, {7, 1, n(20), n(90), n(0), {}}));
    expect(horizon.goals.at(7).remaining == n(0), "WGA-346 constructor zero limit is distinct from feasibility interval floor");
    auto existing = must(f::begin_maintenance(l));
    auto changed = r; changed.unallocated_resources = n(100);
    expect(!must(f::feasible(existing, changed, false)), "WGA-21 positive unallocated resources included");
    auto short_horizon = seeded(); short_horizon.last_interval = n(45);
    short_horizon.categories.at(1).cash = short_horizon.categories.at(1).available_cash = n(2508);
    short_horizon.categories.at(1).income = short_horizon.categories.at(1).available_income = ratio(34479, 1000);
    f::GoalRequest retained{17, 1, n(655), n(3), {}, n(2190)};
    retained.proposal_budget = n(2845);
    short_horizon = must(f::reserve(short_horizon, retained));
    short_horizon = must(f::begin_maintenance(short_horizon));
    expect(must(f::feasible(short_horizon, retained, true)) && must(f::feasible(short_horizon, retained, false)),
        "WGA-21/346 existing goal with three seconds remaining can use the last 45-second fiscal interval");
    expect(short_horizon.goals.at(17).remaining == n(3), "WGA-346 feasibility floor preserves retained funding time");
    short_horizon.categories.at(1).available_cash = n(100);
    expect(!must(f::feasible(short_horizon, retained, true)) && must(f::feasible(short_horizon, retained, false)),
        "WGA-21 existing-goal interval floor preserves available versus total resources and unallocated cost");
    short_horizon.categories.at(1).available_cash = n(2508);
    short_horizon.goals.at(17).remaining = n(0);
    expect(must(f::feasible(short_horizon, retained, true)), "WGA-21 existing goal with zero remaining time uses fiscal interval");
    auto proportional = seeded(); proportional.last_interval = n(10);
    proportional.categories.at(1).income = proportional.categories.at(1).available_income = n(5);
    proportional = must(f::reserve(proportional, request(1, n(75))));
    proportional = must(f::reserve(proportional, request(2, n(25))));
    proportional = must(f::fiscal(proportional, n(200), n(100), n(10)));
    expect(proportional.goals.at(1).accumulated == n(25) && proportional.goals.at(2).accumulated == n(25), "WGA-14 proportional reserved income, distinct from capital");
    auto ordered = seeded(); ordered.last_interval = n(10);
    ordered = must(f::reserve(ordered, request(9, n(40))));
    expect(!must(f::feasible(ordered, request(1, n(40)), true)) && ordered.goals.contains(9), "WGA-75 caller goal order determines successful oversubscribed reservation");
    ordered = must(f::fiscal(ordered, n(100), n(0), n(10)));
    expect(ordered.goals.at(9).remaining == n(0), "WGA-14 remaining time advances even with zero reserved income");
}
void spending() {
    {
        auto resources = seeded(); auto r = request(11, n(20)); r.proposal_budget = n(30);
        resources = must(f::reserve(resources, r));
        expect(resources.goals.at(11).unallocated == n(10), "WGA-345 computed proposal budget minus initial roster allocation");
        const auto cash = resources.categories.at(1).cash;
        resources = must(f::allocate_resources(resources, 11, n(15)));
        expect(resources.goals.at(11).unallocated == n(-5) && resources.goals.at(11).cost == n(35), "allocation retains negative unallocated without clamping");
        resources = must(f::mark_spending(resources, 11, n(12)));
        resources = must(f::release_resources(resources, 11, n(100)));
        expect(resources.goals.at(11).cost == n(0) && resources.goals.at(11).unallocated == n(-5)
            && resources.goals.at(11).marked == n(12) && resources.categories.at(1).cash == cash, "release floors completion only; marks and cash remain distinct");
        const auto before = resources;
        expect(!f::allocate_resources(resources, 11, n(-1)) && resources == before, "negative allocation refuses atomically");
        resources = must(f::remove_goal_salvage(resources, 11, n(7)));
        expect(resources.goals.empty() && resources.dead_goals.empty() && resources.dead_budget == n(7)
            && resources.categories.at(1).cash == n(43) && resources.categories.at(1).available_cash == n(43), "WGA-349 exact released capital pool selects aggregate salvage without residual keyed marks");
    }
    auto l = seeded(); l.last_interval = n(10);
    l = must(f::reserve(l, request(1, n(40))));
    l = must(f::mark_spending(l, 1, n(10)));
    l = must(f::spend(l, 1, n(4)));
    expect(l.goals.at(1).marked == n(6) && l.goals.at(1).cost == n(36) && l.goals.at(1).capital == n(36), "WGA-23 partial capital purchase");
    expect(l.categories.at(1).cash == n(46) && l.categories.at(1).available_cash == n(10), "WGA-23 no second reservation debit");
    l = must(f::remove_goal(l, 1));
    expect(l.dead_goals.at(1).marked == n(6) && l.categories.at(1).available_cash == n(40), "WGA-23 removal retains only marked obligation");
    l = must(f::begin_maintenance(l));
    expect(l.categories.at(1).available_cash == n(40), "dead obligations remain protected during maintenance");
    l = must(f::spend(l, 1, n(6)));
    expect(l.dead_goals.empty() && l.categories.at(1).cash == n(40), "WGA-23 dead-goal billing completes once");
    rejects([&] { return f::spend(l, 1, n(6)); }, f::diagnostic_codes::missing_goal, "dead-goal repeated compatibility spend refused");
    auto during_reset = seeded(); during_reset.last_interval = n(10);
    during_reset = must(f::reserve(during_reset, request(3, n(40))));
    during_reset = must(f::mark_spending(during_reset, 3, n(10)));
    during_reset = must(f::begin_maintenance(during_reset));
    during_reset = must(f::remove_goal(during_reset, 3));
    expect(during_reset.dead_goals.at(3).capital == n(10) && during_reset.categories.at(1).available_cash == n(40), "WGA-23 removal before reassessment re-protects marked obligations");
    expect(!must(f::feasible(during_reset, request(4, n(50)), true)), "outstanding dead purchase cannot be promised to a new goal");
    auto savings = seeded(); savings.last_interval = n(10);
    savings = must(f::reserve(savings, request(2, n(40))));
    savings.goals.at(2).accumulated = n(10); savings.goals.at(2).capital = n(30);
    savings = must(f::mark_spending(savings, 2, n(15)));
    savings = must(f::spend(savings, 2, n(15)));
    expect(savings.goals.at(2).accumulated == n(0) && savings.goals.at(2).capital == n(25), "WGA-23 accumulated income consumed first");
    expect(must(f::purchase_wait(savings, 999, n(1))) == n(-1), "WGA-59 absent budget wait sentinel");
    savings.goals.at(2).cost = n(100); savings.goals.at(2).remaining = n(10);
    expect(must(f::purchase_wait(savings, 2, ratio(2599, 100))) == n(0), "WGA-24 shortage below one credit is zero wait");
    expect(must(f::purchase_wait(savings, 2, n(26))).raw() > 0, "WGA-24 one-credit boundary is positive wait");
    expect(must(f::purchase_wait(savings, 2, n(100))) == n(10), "WGA-59 implied funding rate");
}
void fiscal_and_templates() {
    auto l = seeded();
    l.categories.at(1).cash = n(80); l.categories.at(1).available_cash = n(80);
    l.categories.at(2).cash = n(20); l.categories.at(2).available_cash = n(20);
    l = must(f::fiscal(l, n(120), n(20), n(10)));
    expect(l.categories.at(1).cash == n(90) && l.categories.at(2).cash == n(30), "WGA-15 income change preserves category savings");
    l = must(f::set_equation(l, 1, n(0), 2));
    l = must(f::set_equation(l, 2, n(1), 2));
    l = must(f::begin_maintenance(l));
    expect(l.categories.at(1).cash == n(90), "WGA-24/82 zero share alone does not erase saved cash");
    l = must(f::fiscal(l, n(140), n(20), n(10)));
    expect(l.categories.at(1).cash == n(0) && l.categories.at(2).cash == n(140), "WGA-15 changed equations redistribute capital at fiscal boundary");
    const auto before = l;
    l = must(f::windfall(l, n(20)));
    expect(l.categories.at(2).cash == n(160) && l.categories.at(2).available_cash == before.categories.at(2).available_cash, "WGA-85 windfall changes only totals");
    l = must(f::flush_category(l, 2));
    expect(l.categories.at(2).available_cash == n(0) && l.categories.at(2).cash == n(160), "WGA-24 flush is no cash grant");
    expect(must(f::fiscal(l, n(999), n(999), n(0))) == l, "WGA-13 nonpositive cycle ignored");
    auto tiny = seeded(n(0)); tiny.last_interval = n(10);
    tiny.categories.at(1).income = tiny.categories.at(1).available_income = n(1);
    tiny = must(f::reserve(tiny, request(1, n(1))));
    tiny = must(f::fiscal(tiny, ratio(1, 100000), ratio(1, 100000), n(10)));
    expect(tiny.goals.at(1).accumulated == ratio(1, 10000), "WGA-14 minimum positive funding increment");
    auto protected_l = seeded(); protected_l.last_interval = n(10);
    protected_l = must(f::reserve(protected_l, request(1, n(40))));
    protected_l = must(f::mark_spending(protected_l, 1, n(10)));
    protected_l = must(f::remove_goal(protected_l, 1));
    protected_l = must(f::set_equation(protected_l, 1, n(0), 2));
    protected_l = must(f::set_equation(protected_l, 2, n(1), 2));
    protected_l = must(f::begin_maintenance(protected_l));
    protected_l = must(f::fiscal(protected_l, n(100), n(0), n(10)));
    expect(protected_l.categories.at(1).cash == n(10) && protected_l.categories.at(2).cash == n(90), "WGA-15 dead obligations protected across template redistribution");
}
void tactical() {
    for (const auto allowance : {n(0), ratio(1, 2), n(2), n(-1)}) {
        auto l = must(f::set_allowance(seeded(), 1, allowance, 1));
        const auto entered = must(f::enter_tactical(l, {false, n(100)}));
        const auto expected = allowance <= n(0) ? n(0) : allowance > n(1) ? n(50) : n(25);
        expect(entered.transferred == expected && entered.credits == expected, "WGA-53 positive allowance clamp and zero transfer switch");
        rejects([&] { return f::enter_tactical(entered.ledger, {false, entered.credits}); }, f::diagnostic_codes::invalid_state, "double switch refused");
        auto checkpoint = entered; // Complete value DTO copy stands in for fixture persistence.
        auto released = must(f::release_tactical(checkpoint.ledger, {n(10), checkpoint.credits}));
        released = must(f::release_tactical(released.ledger, {n(-1), released.credits}));
        expect(released.credits == n(100), "WGA-54 additional releases exhaust saved strategic balance");
        auto exhausted = must(f::release_tactical(released.ledger, {n(999), released.credits}));
        expect(exhausted.transferred == n(0), "WGA-54 extra release clamped across multiple requests");
        auto restored = must(f::restore_strategic(exhausted.ledger, exhausted.credits));
        expect(restored.credits == n(100), "WGA-54 round trip with extra release conserves credits");
        expect(must(f::restore_strategic(restored.ledger, restored.credits)).credits == n(100), "WGA-54 restoration idempotent");
        expect(must(f::restore_strategic(entered.ledger, entered.credits)).credits == n(100), "WGA-54 round trip without extra release");
        if (expected > n(0)) {
            const auto spent = eawr::sim::math::subtract(entered.credits, n(5)).value();
            expect(must(f::restore_strategic(entered.ledger, spent)).credits == n(95), "WGA-54 tactical purchase survives restoration");
        }
    }
    const auto l = seeded();
    const auto human = must(f::enter_tactical(l, {true, n(100)}));
    expect(human.ledger == l && human.credits == n(100), "WGA-53 human bypass");
    auto reserved = l; reserved.last_interval = n(10);
    reserved = must(f::reserve(reserved, request(1, n(40))));
    reserved = must(f::set_allowance(reserved, 1, n(1), 1));
    expect(must(f::enter_tactical(reserved, {false, n(100)})).transferred == n(10), "WGA-70 tactical transfer reads live available cash after reservations");
    auto overflow = seeded(); overflow.categories.at(1).cash = A::from_raw(std::numeric_limits<std::int64_t>::max());
    const auto saved = overflow;
    rejects([&] { return f::windfall(overflow, n(100)); }, f::diagnostic_codes::overflow, "fixed-point overflow rejected");
    expect(overflow == saved, "failed pure operation leaves input unchanged");
}
template<class F> void failure_unchanged(const f::Ledger& input, F call,
    std::string_view code, const char* message) {
    const auto before = input;
    rejects(call, code, message);
    expect(input == before, "failed Result leaves the input ledger unchanged");
}
void diagnostics() {
    auto l = seeded(); l.last_interval = n(10);
    failure_unchanged(l, [&] { return f::initialize(l, n(-1)); }, f::diagnostic_codes::negative_input, "negative initial credits diagnostic");
    failure_unchanged(l, [&] { return f::feasible(l, request(1, n(-1)), true); }, f::diagnostic_codes::negative_input, "negative goal cost diagnostic");
    failure_unchanged(l, [&] { return f::reserve(l, request(1, n(-1))); }, f::diagnostic_codes::negative_input, "reserve propagates feasibility diagnostic");
    failure_unchanged(l, [&] { return f::feasible(l, {1, 99, n(1), n(1), {}, {}}, true); }, f::diagnostic_codes::missing_category, "missing feasibility category diagnostic");
    failure_unchanged(l, [&] { return f::flush_category(l, 99); }, f::diagnostic_codes::missing_category, "missing flush category diagnostic");
    failure_unchanged(l, [&] { return f::remove_goal(l, 99); }, f::diagnostic_codes::missing_goal, "missing removal goal diagnostic");
    failure_unchanged(l, [&] { return f::mark_spending(l, 99, n(1)); }, f::diagnostic_codes::missing_goal, "missing spending goal diagnostic");
    failure_unchanged(l, [&] { return f::reserve(l, request(1, n(51))); }, f::diagnostic_codes::insufficient_funding, "insufficient activation diagnostic");
    l = must(f::reserve(l, request(1, n(40))));
    expect(must(f::spend(l, 1, n(1))).goals.at(1).marked == n(0), "approved live bill clamps exhausted mark");
    failure_unchanged(l, [&] { return f::spend(l, 1, n(-1)); }, f::diagnostic_codes::negative_input, "negative spending diagnostic");
    failure_unchanged(l, [&] { return f::mark_spending(l, 1, n(-1)); }, f::diagnostic_codes::negative_input, "negative marking diagnostic");
    expect(must(f::mark_spending(l, 1, n(41))).goals.at(1).marked == n(41), "producer mark can exceed computed cost without changing cash");
    failure_unchanged(l, [&] { return f::purchase_wait(l, 1, n(-1)); }, f::diagnostic_codes::negative_input, "negative wait request diagnostic");
    failure_unchanged(l, [&] { return f::fiscal(l, n(-1), n(1), n(10)); }, f::diagnostic_codes::negative_input, "negative fiscal balance diagnostic");
    failure_unchanged(l, [&] { return f::enter_tactical(l, {false, n(-1)}); }, f::diagnostic_codes::negative_input, "negative tactical entry diagnostic");
    failure_unchanged(l, [&] { return f::release_tactical(l, {n(1), n(-1)}); }, f::diagnostic_codes::negative_input, "negative tactical release balance diagnostic");
    failure_unchanged(l, [&] { return f::restore_strategic(l, n(-1)); }, f::diagnostic_codes::negative_input, "negative restoration balance diagnostic");
    l = must(f::mark_spending(l, 1, n(10)));
    l = must(f::remove_goal(l, 1));
    failure_unchanged(l, [&] { return f::spend(l, 1, n(11)); }, f::diagnostic_codes::unmarked_spend, "keyed compatibility dead-goal spending diagnostic");
    auto invalid = seeded(); invalid.goals[1].category = 99;
    failure_unchanged(invalid, [&] { return f::begin_maintenance(invalid); }, f::diagnostic_codes::invalid_state, "dangling category state diagnostic");
    failure_unchanged(invalid, [&] { return f::category_order(invalid); }, f::diagnostic_codes::invalid_state, "category ordering also returns a Result");
    auto overflow = seeded();
    overflow.categories.at(2).cash = A::from_raw(std::numeric_limits<std::int64_t>::max());
    failure_unchanged(overflow, [&] { return f::windfall(overflow, n(100)); }, f::diagnostic_codes::overflow, "original math overflow diagnostic after earlier category staging");
    auto summed = must(f::set_equation({}, 1, A::from_raw(std::numeric_limits<std::int64_t>::max()), 1));
    summed = must(f::set_equation(summed, 2, A::from_raw(std::numeric_limits<std::int64_t>::max()), 1));
    failure_unchanged(summed, [&] { return f::initialize(summed, n(1)); }, f::diagnostic_codes::overflow, "normalization preserves math failure diagnostic");
    auto small = seeded(); small.last_interval = n(10);
    small = must(f::reserve(small, request(1, n(40))));
    small.goals.at(1).capital = {};
    small.goals.at(1).remaining = A::from_raw(1);
    small.goals.at(1).cost = n(40000);
    failure_unchanged(small, [&] { return f::purchase_wait(small, 1, n(40000)); }, f::diagnostic_codes::overflow, "purchase wait propagates implied-rate overflow");
}
}
int main() {
    try { allocation(); reservations(); spending(); fiscal_and_templates(); tactical(); diagnostics(); }
    catch (const std::exception& e) { std::cerr << "Unexpected: " << e.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}

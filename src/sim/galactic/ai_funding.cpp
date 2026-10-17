#include "eawr/sim/galactic/ai_funding.hpp"

#include <algorithm>
#include <utility>

namespace eawr::sim::galactic::funding {
namespace {
const Amount zero{};
// Arithmetic within a value transaction retains the first math diagnostic.
// Failed intermediates use zero only inside the disposable working copy; finish
// never exposes that copy when any calculation failed.
struct Arithmetic {
    std::optional<core::Diagnostic> error;
    Amount checked(core::Result<Amount> result) {
        if (!result) { if (!error) error = result.error(); return {}; }
        return result.value();
    }
    Amount plus(Amount a, Amount b) { return checked(math::add(a, b)); }
    Amount minus(Amount a, Amount b) { return checked(math::subtract(a, b)); }
    Amount times(Amount a, Amount b) { return checked(math::multiply(a, b)); }
    Amount quotient(Amount a, Amount b) { return checked(math::divide(a, b)); }
    template<class T> core::Result<T> fail(std::string_view code, std::string message) const {
        if (error) return core::Result<T>::failure(*error);
        core::Diagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.message = std::move(message);
        return core::Result<T>::failure(std::move(diagnostic));
    }
    template<class T> core::Result<T> finish(T value) const {
        if (error) return core::Result<T>::failure(*error);
        return core::Result<T>::success(std::move(value));
    }
};
Amount positive(Amount value) { return std::max(zero, value); }
bool references_valid(const Ledger& l) {
    if (l.dead_budget < zero || l.last_service_income < zero) return false;
    for (const auto& [id, g] : l.goals) {
        if (!l.categories.contains(g.category) || l.dead_goals.contains(id)) return false;
    }
    for (const auto& [id, g] : l.dead_goals) {
        (void)id;
        if (!l.categories.contains(g.category)) return false;
    }
    return true;
}
Amount horizon(const Ledger& l, const GoalRequest& r) {
    auto h = r.estimated_seconds;
    if (r.time_limit) h = std::min(h, *r.time_limit);
    return std::max(h, l.last_interval); // WGA-21
}
bool shares(Ledger& l, Arithmetic& a) {
    Amount sum{};
    for (auto& [id, c] : l.categories) {
        (void)id;
        c.share = positive(c.evaluated_weight.value_or(c.share));
        if (c.evaluated_weight) sum = a.plus(sum, c.share); // WGA-69: evaluated weights only
    }
    if (l.categories.empty()) return true;
    if (sum > zero) for (const auto& [id, c] : l.categories) {
        (void)id;
        if (!c.evaluated_weight && c.share > zero)
            return false;
    }
    const auto count = a.checked(Amount::from_integer(static_cast<std::int64_t>(l.categories.size())));
    Amount used{};
    for (auto& [id, c] : l.categories) {
        (void)id;
        c.share = sum > zero ? a.quotient(c.share, sum) : a.quotient(a.checked(Amount::from_integer(1)), count);
        used = a.plus(used, c.share);
    }
    // Deterministic project rounding policy: preserve exactly one whole share.
    auto largest = std::max_element(l.categories.begin(), l.categories.end(),
        [](const auto& left, const auto& right) { return left.second.share < right.second.share; });
    largest->second.share = a.plus(largest->second.share,
        a.minus(a.checked(Amount::from_integer(1)), used));
    return true;
}
// Exact total conservation despite rounded fixed-point products.
std::map<CategoryId, Amount> distribute(const Ledger& l, Amount total, Arithmetic& a, bool previous = false) {
    std::map<CategoryId, Amount> out;
    Amount used{};
    for (const auto& [id, c] : l.categories) {
        const auto value = a.times(total, previous ? c.previous_share : c.share);
        out[id] = value; used = a.plus(used, value);
    }
    if (!out.empty()) {
        const auto largest = std::max_element(l.categories.begin(), l.categories.end(),
            [previous](const auto& left, const auto& right) {
                return (previous ? left.second.previous_share : left.second.share)
                    < (previous ? right.second.previous_share : right.second.share);
            });
        out.at(largest->first) = a.plus(out.at(largest->first), a.minus(total, used));
    }
    return out;
}
void consume(Amount& accumulated, Amount& capital, Amount amount, Arithmetic& a) {
    const auto from_income = std::min(accumulated, amount);
    accumulated = a.minus(accumulated, from_income);
    capital = positive(a.minus(capital, a.minus(amount, from_income)));
}
}

core::Result<Ledger> set_equation(Ledger l, CategoryId id, std::optional<Amount> weight, int priority) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    // WGA-68: null clears an existing evaluator, never invents a category.
    if (!weight && !l.categories.contains(id)) return a.fail<Ledger>(diagnostic_codes::missing_category, "missing category equation");
    const bool fresh = !l.categories.contains(id);
    auto& c = l.categories[id];
    if (fresh || priority >= c.equation_priority) {
        c.evaluated_weight = weight; c.equation_priority = priority;
    }
    l.equations_changed = true;
    return a.finish(std::move(l));
}
core::Result<Ledger> set_allowance(Ledger l, CategoryId id, Amount allowance, int priority) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    const bool fresh = !l.categories.contains(id);
    auto& c = l.categories[id];
    if (fresh || priority >= c.allowance_priority) {
        c.allowance = allowance; c.allowance_priority = priority;
    }
    l.equations_changed = true; // WGA-68; clamp only at actual tactical release
    return a.finish(std::move(l));
}
core::Result<Ledger> initialize(Ledger l, Amount credits) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (credits < zero) return a.fail<Ledger>(diagnostic_codes::negative_input, "negative credits");
    if (!l.goals.empty() || !l.dead_goals.empty() || l.tactical.switched)
        return a.fail<Ledger>(diagnostic_codes::invalid_state, "initialization requires a fresh ledger");
    if (!shares(l, a)) return a.fail<Ledger>(diagnostic_codes::unverified_rule, "unverified AllowanceOnlyReevaluation seam");
    const auto cash = distribute(l, credits, a);
    for (auto& [id, c] : l.categories) {
        c.cash = c.available_cash = cash.at(id); c.previous_share = c.share;
    }
    l.equations_changed = false;
    l.last_interval = l.last_service_income = zero; // WGA-294
    return a.finish(std::move(l)); // WGA-11/69
}
core::Result<Ledger> begin_maintenance(Ledger l) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    // WGA-421: maintenance writes zero for categories without an equation.
    // Startup's constructor-zero state and failed *present* equations are
    // handled separately by the retained funding consumer.
    for (auto& [id, category] : l.categories) {
        (void)id;
        if (!category.evaluated_weight) category.share = zero;
    }
    if (!shares(l, a)) return a.fail<Ledger>(diagnostic_codes::unverified_rule, "unverified AllowanceOnlyReevaluation seam");
    for (auto& [id, c] : l.categories) { (void)id; c.available_cash = c.cash; c.available_income = c.income; }
    for (auto& [id, g] : l.goals) {
        (void)id;
        auto& c = l.categories.at(g.category);
        c.available_cash = a.minus(c.available_cash, g.accumulated);
        g.capital = zero; g.reserved_income = zero; g.reassessed = false;
    }
    for (const auto& [id, g] : l.dead_goals) {
        (void)id;
        auto& c = l.categories.at(g.category);
        c.available_cash = a.minus(c.available_cash, a.plus(g.accumulated, g.capital));
    }
    return a.finish(std::move(l)); // WGA-12/23/75
}
core::Result<std::vector<CategoryId>> category_order(const Ledger& l) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<std::vector<CategoryId>>(diagnostic_codes::invalid_state, "invalid goal/category references");
    std::vector<CategoryId> order;
    for (const auto& [id, c] : l.categories) { (void)c; order.push_back(id); }
    std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
        return l.categories.at(left).share > l.categories.at(right).share;
    });
    return a.finish(std::move(order)); // WGA-75; equal-share ID order is project policy
}
core::Result<bool> feasible(const Ledger& l, const GoalRequest& r, bool activation) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<bool>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (r.cost < zero || r.estimated_seconds < zero || (r.time_limit && *r.time_limit < zero))
        return a.fail<bool>(diagnostic_codes::negative_input, "negative goal request");
    if (!l.categories.contains(r.category)) return a.fail<bool>(diagnostic_codes::missing_category, "missing category");
    const auto& c = l.categories.at(r.category);
    auto h = horizon(l, r);
    auto cost = r.cost;
    if (const auto it = l.goals.find(r.id); it != l.goals.end()) {
        if (it->second.category != r.category) return a.fail<bool>(diagnostic_codes::invalid_input, "goal category changed");
        h = std::max(it->second.remaining, l.last_interval); // WGA-21/346: feasibility floor, not retained time
        cost = positive(a.minus(a.plus(std::max(cost, it->second.marked), positive(r.unallocated_resources)), it->second.accumulated));
    }
    return a.finish(cost <= positive(a.plus(activation ? c.available_cash : c.cash,
        a.times(activation ? c.available_income : c.income, h)))); // WGA-21
}
core::Result<Ledger> reserve(Ledger l, const GoalRequest& r) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (r.cost < zero || r.estimated_seconds < zero || (r.time_limit && *r.time_limit < zero))
        return a.fail<Ledger>(diagnostic_codes::negative_input, "negative goal request");
    if (!l.categories.contains(r.category)) return a.fail<Ledger>(diagnostic_codes::missing_category, "missing category");
    if (l.dead_goals.contains(r.id)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "goal has retained obligations");
    const auto it = l.goals.find(r.id);
    if (it != l.goals.end() && it->second.reassessed) return a.finish(std::move(l)); // WGA-22
    const auto affordable = feasible(l, r, true);
    if (!affordable) return core::Result<Ledger>::failure(affordable.error());
    if (!affordable.value()) return a.fail<Ledger>(diagnostic_codes::insufficient_funding, "insufficient available funding");
    const bool fresh = it == l.goals.end();
    auto& g = l.goals[r.id];
    auto& c = l.categories.at(r.category);
    if (fresh) {
        g.category = r.category;
        g.remaining = r.time_limit ? std::min(r.estimated_seconds, *r.time_limit) : r.estimated_seconds; // WGA-346
        g.unallocated = a.minus(r.proposal_budget, r.cost); // WGA-345: negative values remain observable
    }
    g.cost = std::max(r.cost, g.marked);
    if (g.accumulated > g.cost) {
        c.available_cash = a.plus(c.available_cash, a.minus(g.accumulated, g.cost));
        g.accumulated = g.cost; // WGA-22 retained savings and surplus
    }
    const auto needed = positive(a.minus(g.cost, g.accumulated));
    g.capital = std::min(positive(c.available_cash), needed);
    const auto remainder = a.minus(needed, g.capital);
    const auto duration = g.remaining > zero ? g.remaining : l.last_interval;
    if (remainder > zero && duration <= zero) return a.fail<Ledger>(diagnostic_codes::invalid_state, "missing fiscal interval");
    g.reserved_income = remainder > zero ? a.quotient(remainder, duration) : zero;
    if (g.reserved_income > positive(c.available_income)) {
        g.reserved_income = positive(c.available_income);
        g.capital = a.minus(needed, a.times(g.reserved_income, duration));
    }
    c.available_cash = a.minus(c.available_cash, g.capital);
    c.available_income = a.minus(c.available_income, g.reserved_income);
    g.reassessed = true;
    return a.finish(std::move(l));
}
core::Result<Ledger> mark_spending(Ledger l, GoalId id, Amount amount) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (amount < zero) return a.fail<Ledger>(diagnostic_codes::negative_input, "negative spending");
    if (!l.goals.contains(id)) return a.fail<Ledger>(diagnostic_codes::missing_goal, "missing goal");
    auto& g = l.goals.at(id);
    g.marked = a.plus(g.marked, amount);
    return a.finish(std::move(l)); // WGA-23/24: purchase host marks accepted outstanding obligations
}
core::Result<Ledger> spend(Ledger l, GoalId id, Amount amount) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (amount < zero) return a.fail<Ledger>(diagnostic_codes::negative_input, "negative spending");
    if (id == 0) {
        l.dead_budget = positive(a.minus(l.dead_budget, amount));
        return a.finish(std::move(l)); // WGA-160: no category debit or keyed-row mutation
    }
    if (auto it = l.goals.find(id); it != l.goals.end()) {
        auto& g = it->second;
        auto& c = l.categories.at(g.category);
        c.cash = a.minus(c.cash, amount); g.marked = positive(a.minus(g.marked, amount)); g.cost = positive(a.minus(g.cost, amount));
        consume(g.accumulated, g.capital, amount, a);
    } else {
        if (!l.dead_goals.contains(id)) return a.fail<Ledger>(diagnostic_codes::missing_goal, "missing goal");
        auto& g = l.dead_goals.at(id);
        if (amount > g.marked) return a.fail<Ledger>(diagnostic_codes::unmarked_spend, "unmarked dead-goal spend");
        l.categories.at(g.category).cash = a.minus(l.categories.at(g.category).cash, amount);
        g.marked = a.minus(g.marked, amount); consume(g.accumulated, g.capital, amount, a);
        if (g.marked == zero) l.dead_goals.erase(id);
    }
    return a.finish(std::move(l)); // WGA-23; actual player billing is external, never a second debit
}
core::Result<Ledger> remove_goal(Ledger l, GoalId id) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (!l.goals.contains(id)) return a.fail<Ledger>(diagnostic_codes::missing_goal, "missing goal");
    const auto g = l.goals.at(id);
    auto& c = l.categories.at(g.category);
    const auto held = a.plus(g.capital, g.accumulated);
    // WGA-23: maintenance can already have cleared capital for reassessment.
    // Marked purchases still retain their whole obligation, including any amount
    // temporarily returned to the category's available cash by that reset.
    const auto retained = g.marked;
    c.available_cash = a.plus(c.available_cash, a.minus(held, retained));
    c.available_income = a.plus(c.available_income, g.reserved_income);
    if (g.marked > zero) {
        l.dead_budget = a.plus(l.dead_budget, g.marked);
        const auto savings = std::min(g.accumulated, retained);
        l.dead_goals[id] = {g.category, g.marked, a.minus(retained, savings), savings};
    }
    l.goals.erase(id);
    return a.finish(std::move(l)); // WGA-23: unused funds released, outstanding billing retained
}
core::Result<Ledger> allocate_resources(Ledger l, GoalId id, Amount amount) {
    Arithmetic a;
    if (!references_valid(l) || !l.goals.contains(id)) return a.fail<Ledger>(diagnostic_codes::missing_goal, "allocation requires a retained goal");
    if (amount < zero) return a.fail<Ledger>(diagnostic_codes::negative_input, "negative allocation");
    auto& g = l.goals.at(id);
    g.unallocated = a.minus(g.unallocated, amount);
    g.cost = a.plus(g.cost, amount);
    return a.finish(std::move(l));
}
core::Result<Ledger> release_resources(Ledger l, GoalId id, Amount amount) {
    Arithmetic a;
    if (!references_valid(l) || !l.goals.contains(id)) return a.fail<Ledger>(diagnostic_codes::missing_goal, "release requires a retained goal");
    if (amount < zero) return a.fail<Ledger>(diagnostic_codes::negative_input, "negative release");
    auto& g = l.goals.at(id);
    g.cost = a.minus(g.cost, std::min(g.cost, amount));
    return a.finish(std::move(l));
}
core::Result<Ledger> remove_goal_salvage(Ledger l, GoalId id, Amount selected) {
    Arithmetic a;
    if (!references_valid(l) || !l.goals.contains(id)) return a.fail<Ledger>(diagnostic_codes::missing_goal, "salvage requires a retained goal");
    const auto g = l.goals.at(id);
    const auto held = a.plus(g.capital, g.accumulated);
    if (selected < zero || held < selected) return a.fail<Ledger>(diagnostic_codes::invalid_input, "salvage exceeds released resources");
    auto& c = l.categories.at(g.category);
    c.available_cash = a.minus(a.plus(c.available_cash, held), selected);
    c.cash = a.minus(c.cash, selected);
    c.available_income = a.plus(c.available_income, g.reserved_income);
    l.dead_budget = a.plus(l.dead_budget, selected);
    l.goals.erase(id);
    return a.finish(std::move(l));
}
core::Result<Amount> purchase_wait(const Ledger& l, GoalId id, Amount requested) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Amount>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (requested < zero) return a.fail<Amount>(diagnostic_codes::negative_input, "negative purchase request");
    const auto it = l.goals.find(id);
    if (it == l.goals.end()) return a.finish(a.checked(Amount::from_integer(-1))); // WGA-59
    const auto& g = it->second;
    const auto available = std::min(a.plus(g.capital, g.accumulated), g.cost);
    const auto shortage = a.minus(a.plus(requested, g.marked), available);
    if (shortage.trunc_to_integer() < 1) return a.finish(zero);
    auto denominator = std::max(a.minus(g.cost, a.plus(g.capital, g.accumulated)), shortage);
    if (g.remaining > zero) denominator = a.quotient(denominator, g.remaining);
    return a.finish(a.quotient(shortage, denominator)); // WGA-24/59 fractional-credit boundary
}
core::Result<Ledger> flush_category(Ledger l, CategoryId id) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (!l.categories.contains(id)) return a.fail<Ledger>(diagnostic_codes::missing_category, "missing category");
    l.categories.at(id).available_cash = zero; return a.finish(std::move(l));
}
core::Result<Ledger> fiscal(Ledger l, Amount actual_credits, Amount income, Amount seconds) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (seconds <= zero) return a.finish(std::move(l)); // WGA-13
    if (actual_credits < zero || income < zero) return a.fail<Ledger>(diagnostic_codes::negative_input, "negative fiscal input");
    l.last_interval = seconds;
    l.last_service_income = a.quotient(income, seconds); // WGA-293
    const auto funding = distribute(l, income, a, true);
    for (auto& [id, c] : l.categories) {
        Amount reserved{};
        for (const auto& [gid, g] : l.goals) { (void)gid; if (g.category == id) reserved = a.plus(reserved, g.reserved_income); }
        if (reserved > zero) for (auto& [gid, g] : l.goals) {
            (void)gid;
            if (g.category != id) continue;
            auto increment = a.times(funding.at(id), a.quotient(g.reserved_income, reserved));
            if (increment > zero) increment = std::max(increment, a.checked(Amount::from_ratio(1, 10000)));
            g.accumulated = a.plus(g.accumulated, increment);
            g.cost = std::max(g.cost, g.marked);
        }
        c.cash = a.plus(c.cash, funding.at(id));
        c.income = a.quotient(funding.at(id), seconds);
    }
    for (auto& [id, g] : l.goals) {
        (void)id;
        g.remaining = positive(a.minus(g.remaining, seconds));
    }
    Amount total{}, protected_total{};
    std::map<CategoryId, Amount> protected_cash;
    for (const auto& [id, c] : l.categories) { total = a.plus(total, c.cash); protected_cash[id] = zero; }
    for (const auto& [id, g] : l.goals) { (void)id; protected_cash.at(g.category) = a.plus(protected_cash.at(g.category), g.accumulated); }
    for (const auto& [id, g] : l.dead_goals) { (void)id; protected_cash.at(g.category) = a.plus(protected_cash.at(g.category), a.plus(g.accumulated, g.capital)); }
    for (const auto& [id, value] : protected_cash) { (void)id; protected_total = a.plus(protected_total, value); }
    // WGA-15/83: protected obligations stay local; changed equations distribute
    // unprotected capital, then reconcile actual-credit residual by current shares.
    const auto capital = distribute(l, a.minus(total, protected_total), a);
    const auto residual = distribute(l, a.minus(actual_credits, total), a);
    for (auto& [id, c] : l.categories) {
        if (l.equations_changed) c.cash = a.plus(protected_cash.at(id), capital.at(id));
        c.cash = a.plus(c.cash, residual.at(id)); c.previous_share = c.share;
    }
    l.equations_changed = false;
    for (auto& [id, c] : l.categories) {
        c.available_cash = a.minus(c.cash, protected_cash.at(id));
        c.available_income = c.income;
    }
    for (const auto& [id, g] : l.goals) {
        (void)id;
        auto& c = l.categories.at(g.category);
        c.available_cash = a.minus(c.available_cash, g.capital);
        c.available_income = a.minus(c.available_income, g.reserved_income);
    }
    // WGA-12 reservation reassessment is a separate completed-pass consumer.
    return a.finish(std::move(l));
}
core::Result<Ledger> windfall(Ledger l, Amount delta) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<Ledger>(diagnostic_codes::invalid_state, "invalid goal/category references");
    const auto money = distribute(l, delta, a);
    for (auto& [id, c] : l.categories) c.cash = a.plus(c.cash, money.at(id));
    return a.finish(std::move(l)); // WGA-85 deliberately leaves available cash unchanged
}
core::Result<TransferResult> enter_tactical(Ledger l, TacticalTransferRequest r) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<TransferResult>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (r.strategic_credits < zero) return a.fail<TransferResult>(diagnostic_codes::negative_input, "negative strategic credits");
    if (r.human) return a.finish(TransferResult{std::move(l), r.strategic_credits, zero});
    if (l.tactical.switched) return a.fail<TransferResult>(diagnostic_codes::invalid_state, "balance already switched");
    Amount transferred{};
    for (auto& [id, c] : l.categories) {
        (void)id;
        if (c.allowance <= zero || c.available_cash <= zero) continue;
        const auto amount = std::clamp(a.times(c.available_cash, c.allowance), zero, c.available_cash);
        c.available_cash = a.minus(c.available_cash, amount); c.cash = a.minus(c.cash, amount);
        transferred = a.plus(transferred, amount);
    }
    if (transferred > r.strategic_credits) return a.fail<TransferResult>(diagnostic_codes::invalid_state, "ledger exceeds strategic balance");
    l.tactical = {true, a.minus(r.strategic_credits, transferred), zero};
    return a.finish(TransferResult{std::move(l), transferred, transferred}); // WGA-53/70/84
}
core::Result<TransferResult> release_tactical(Ledger l, TacticalReleaseRequest r) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<TransferResult>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (r.tactical_credits < zero) return a.fail<TransferResult>(diagnostic_codes::negative_input, "negative tactical credits");
    if (!l.tactical.switched) return a.finish(TransferResult{std::move(l), r.tactical_credits, zero});
    const auto amount = r.requested < zero ? positive(a.minus(l.tactical.saved, l.tactical.extra_released))
        : std::min(r.requested, positive(a.minus(l.tactical.saved, l.tactical.extra_released)));
    l.tactical.extra_released = a.plus(l.tactical.extra_released, amount);
    return a.finish(TransferResult{std::move(l), a.plus(r.tactical_credits, amount), amount}); // WGA-54
}
core::Result<TransferResult> restore_strategic(Ledger l, Amount credits) {
    Arithmetic a;
    if (!references_valid(l)) return a.fail<TransferResult>(diagnostic_codes::invalid_state, "invalid goal/category references");
    if (credits < zero) return a.fail<TransferResult>(diagnostic_codes::negative_input, "negative tactical credits");
    if (!l.tactical.switched) return a.finish(TransferResult{std::move(l), credits, zero});
    const auto restored = a.plus(l.tactical.saved, a.minus(credits, l.tactical.extra_released));
    l.tactical = {};
    return a.finish(TransferResult{std::move(l), restored, zero}); // WGA-54: once only, DTO survives fixture save/load
}
} // namespace eawr::sim::galactic::funding

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include "../support/result.hpp"
// Supporting live-session contracts. The installed stock plan/build/battle/load
// primary proof waits for the lifecycle, execution and persistence providers.
#include "galactic_support.hpp"
#include "../../src/script/foc/galactic_ai_funding.hpp"
#include "../../src/script/foc/galactic_ai_world.hpp"
#include "../../src/script/foc/tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <array>
#include <limits>
#include <stdexcept>

namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static void maintain(Engine& engine, tactical::PlayerId player,
        std::shared_ptr<GalacticFunding> funding, galactic_ai::TemplatePolicy policy) {
        PlayerAi selected; selected.player = player;
        selected.galactic_funding = std::move(funding);
        selected.galactic_policy = std::move(policy);
        engine.begin_galactic_maintenance(selected);
    }
};
}
namespace {
using namespace galactic_test;
namespace ai = eawr::script::foc::ai;
using eawr::sim::math::Fixed;
using eawr::testing::result_code_colon::take;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
ai::EvaluatedFundingTemplate policy() {
    return {1, {{1, f(1)}, {2, f(1)}}, {{1, take(Fixed::from_ratio(1, 2))}}};
}
galactic::GalacticSetup setup() {
    galactic::SyntheticGalaxyOptions options;
    options.players = 1; options.planets = 2;
    options.fleets_per_player = 0;
    auto config = galactic::synthetic_galaxy(options);
    config.rules.fiscal_cycle_frames = 3;
    config.player_economy = galactic::PlayerEconomySetup{{{1, f(90), f(100), 0, 0}}};
    config.fiscal = galactic::FiscalSetup{f(50), {{1, true, f(2)}}, {}};
    for (const auto& planet : config.planets) config.fiscal->planets.push_back({planet.id, f(10)});
    return config;
}
Fixed cash(const ai::GalacticFunding& manager, eawr::sim::galactic::PlayerId player = 1) {
    Fixed sum;
    const auto snapshot = manager.ledger(player);
    for (const auto& [id, category] : snapshot->categories) {
        (void)id; sum = take(eawr::sim::math::add(sum, category.cash));
    }
    return sum;
}
void live_fiscal() {
    std::optional<std::string> hash;
    std::optional<ai::GalacticFunding::State> state;
    for (const auto count : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter workers(count);
        auto world = take(galactic::GalacticSession::create(setup()));
        ai::GalacticFunding manager;
        const std::array templates{policy()};
        take(manager.initialize(1, templates, f(90)));
        const auto available = manager.ledger(1)->categories.at(1).available_cash;
        take(manager.reserve(1, {7, 1, f(20), f(1), {}, {}}));
        unsigned notifications{};
        world.set_credit_notification_handler([&](const auto& input) {
            ++notifications;
            expect(input.kind == galactic::GalacticSession::CreditNotification::Kind::fiscal,
                "fiscal keeps its notification kind");
            expect(input.credits == f(100) && input.amount == f(20),
                "fiscal passes gross income even when capped account gain becomes zero");
            expect(input.seconds == take(Fixed::from_ratio(3, 30)), "configured frame window supplies seconds");
            expect(world.clock().fiscal_cycle == notifications, "notification precedes fiscal cycle increment");
            return manager.notify(input);
        });
        for (unsigned i = 0; i < 7; ++i) (void)take(world.step(workers));
        expect(notifications == 2 && cash(manager) == f(100), "capped zero delta still delivers fiscal callback");
        const auto category_rate = take(eawr::sim::math::divide(f(10), take(Fixed::from_ratio(3, 30))));
        expect(manager.ledger(1)->categories.at(1).income == category_rate
            && manager.ledger(1)->categories.at(2).income == category_rate,
            "category income uses gross aggregate and duration independently of account cap");
        expect(manager.ledger(1)->goals.at(7).capital == f(20), "fiscal keeps existing capital reservation");
        expect(manager.ledger(1)->goals.at(7).remaining < f(1), "reservation time ages each fiscal interval");
        expect(manager.ledger(1)->categories.at(1).available_cash < available,
            "reconciliation retains reservation in available balance");
        if (!hash) { hash = world.state_sha256(); state = manager.state(); }
        expect(world.state_sha256() == *hash && manager.state() == *state, "live fiscal and retained ledger 1/2/4/8 equality");
        const auto before = notifications;
        const std::array pause{galactic::GameCommand{{7, {1}, 1}, galactic::PauseGalaxy{}}};
        take(world.apply_session_controls(pause));
        for (unsigned i = 0; i < 6; ++i) (void)take(world.step(workers));
        expect(notifications == before, "paused galaxy delivers no fiscal notifications");
    }
}
void windfalls_and_failure() {
    auto world = take(galactic::GalacticSession::create(setup()));
    ai::GalacticFunding manager;
    const std::array templates{policy()};
    take(manager.initialize(1, templates, f(90)));
    const auto available = manager.ledger(1)->categories.at(1).available_cash;
    unsigned notified{};
    world.set_credit_notification_handler([&](const auto& input) {
        ++notified;
        expect(input.kind == galactic::GalacticSession::CreditNotification::Kind::windfall,
            "explicit addition delivers only windfall");
        return manager.notify(input);
    });
    expect(take(world.add_credits(1, f(6), f(2))) == f(10), "positive windfall multiplier then cap");
    expect(cash(manager) == f(100) && manager.ledger(1)->categories.at(1).available_cash == available,
        "windfall changes only total category cash");
    expect(take(world.add_credits(1, f(-6), f(2))) == f(-6) && cash(manager) == f(94),
        "negative addition bypasses multiplier and notifies signed delta");
    expect(take(world.add_credits(1, f(-1000), f(2))) == f(-94) && cash(manager) == f(0),
        "windfall lower bound sends actual deducted credits");
    expect(notified == 3 && world.snapshot()->player_economy()->players[0].credits == f(0),
        "windfalls publish account immediately");
    const auto before = world.state_sha256();
    expect(!world.add_credits(2, f(1), f(1)) && !world.add_credits(1, f(1), f(-1)),
        "missing account and invalid multiplier reject");
    expect(notified == 3 && world.state_sha256() == before, "invalid additions do not notify or mutate");
    auto broken = manager.state();
    broken.at(1).categories.at(1).cash = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    take(manager.restore(broken));
    expect(!world.add_credits(1, f(10), f(1)), "ledger overflow rejects windfall transaction");
    expect(world.state_sha256() == before && manager.state() == broken,
        "failed windfall leaves account and ledger intact");
    const auto uninitialized = manager.state();
    take(manager.notify({galactic::GalacticSession::CreditNotification::Kind::windfall, 2, f(10), f(10), {}}));
    expect(manager.state() == uninitialized, "uninitialized AI does not acquire a ledger via notification");
}
void zero_income_and_refusal() {
    auto config = setup();
    for (auto& planet : config.fiscal->planets) planet.credits = f(0);
    auto world = take(galactic::GalacticSession::create(config));
    ai::GalacticFunding manager;
    const std::array templates{policy()};
    take(manager.initialize(1, templates, f(90)));
    take(manager.reserve(1, {7, 1, f(20), f(1), {}, {}}));
    unsigned notified{};
    world.set_credit_notification_handler([&](const auto& input) {
        ++notified;
        return manager.notify(input);
    });
    const eawr::sim::InlineExecutor workers;
    for (unsigned i = 0; i < 4; ++i) (void)take(world.step(workers));
    expect(notified == 0 && manager.ledger(1)->goals.at(7).remaining == f(1),
        "WGEP-112/WGA-14: zero income skips AI callback and retains goal timer");
    const auto before = manager.state();
    const galactic::GalacticSession::CreditNotification negative{
        galactic::GalacticSession::CreditNotification::Kind::fiscal, 1, f(89), f(-1), f(1)};
    const auto refused = manager.notify(negative);
    expect(!refused && refused.error().code == ai::funding::diagnostic_codes::negative_input
        && manager.state() == before, "NegativeFiscalIncome seam refuses without inventing clamped savings");
    galactic::GalacticSession::CreditNotification zero_duration = negative;
    zero_duration.seconds = f(0);
    take(manager.notify(zero_duration));
    expect(manager.state() == before, "nonpositive fiscal duration does not process ledger");
    auto paying = take(galactic::GalacticSession::create(setup()));
    unsigned refusals{};
    paying.set_credit_notification_handler([&](const auto&) {
        ++refusals;
        eawr::core::Diagnostic error;
        error.code = ai::funding::diagnostic_codes::invalid_state;
        error.message = "test consumer refusal";
        return eawr::core::Result<void>::failure(std::move(error));
    });
    bool failed{};
    // WGEP-59/GFD-04: configured fiscal checks frame 3 before its timer advances.
    for (unsigned i = 0; i < 4; ++i) {
        if (!paying.step(workers)) { failed = true; break; }
    }
    expect(refusals == 1 && failed && !paying.step(workers), "failed fiscal consumer poisons further world steps");
}
void retained_transactions() {
    ai::GalacticFunding manager;
    const std::array templates{policy()};
    take(manager.initialize(1, templates, f(100)));
    eawr::script::foc::galactic_ai::Funding& capability = manager;
    const auto held = capability.ledger(1);
    take(manager.reserve(1, {7, 1, f(20), f(1), {}, {}}));
    eawr::script::foc::galactic_ai::FundingNotice accepted;
    accepted.player = 1; accepted.goal = 7; accepted.amount = f(10);
    accepted.kind = eawr::script::foc::galactic_ai::NoticeKind::marked;
    take(capability.notify(accepted));
    expect(held && held->goals.empty() && held->categories.at(1).cash == f(50),
        "held ledger snapshot does not alias mutable reservation state");
    take(manager.remove_goal(1, 7));
    expect(manager.ledger(1)->dead_goals.contains(7), "removed plan retains producer billing obligation");
    auto billed = accepted;
    billed.kind = eawr::script::foc::galactic_ai::NoticeKind::billed;
    take(capability.notify(billed));
    expect(held->categories.at(1).cash == f(50) && held->goals.empty(),
        "actual adapter snapshot stays immutable across reserve and billing notices");
    expect(!manager.ledger(1)->dead_goals.contains(7) && cash(manager) == f(90),
        "actual billing clears obligation and debits ledger once");
    const auto before = manager.state();
    expect(!manager.spend(1, 7, f(10)) && manager.state() == before, "keyed compatibility duplicate billing fails atomically");
    eawr::script::foc::galactic_ai::FundingNotice completion;
    completion.player = 1;
    completion.kind = eawr::script::foc::galactic_ai::NoticeKind::completed;
    expect(!capability.notify(completion) && manager.state() == before,
        "unqualified completion notice does not infer funding or credit grants");
    const eawr::script::foc::galactic_ai::FundingTransaction transaction{1, {8, 2, f(5), f(1), {}, {}}, true};
    expect(take(capability.feasible(transaction)), "shared funding capability queries retained available cash");
    take(capability.reserve(transaction));
    take(manager.remove_goal(1, 8));
    auto broken = before;
    broken.at(1).goals[9].category = 999;
    const auto unmodified = manager.state();
    expect(!manager.restore(broken) && manager.state() == unmodified, "invalid DTO references reject restore atomically");
    expect(!manager.initialize(1, templates, f(90)) && manager.state() == unmodified, "initialization is once per retained player");
    take(manager.apply_template(1, {2, {{1, f(0)}, {2, f(1)}}, {}}));
    expect(cash(manager) == f(90), "template application does not grant money");
    const auto changed = manager.state();
    const std::array<ai::EvaluatedFundingWeight, 2> duplicate{{{1, f(0)}, {1, f(1)}}};
    expect(!manager.begin_maintenance(1, duplicate) && manager.state() == changed,
        "bad reevaluation does not partially mutate retained state");
    take(manager.begin_maintenance(1, {}));
    expect(manager.ledger(1)->categories.at(1).cash == before.at(1).categories.at(1).cash,
        "zero share keeps saved category cash until fiscal reconciliation");
    const auto tactical = take(manager.enter_tactical(1, {false, f(90)}));
    expect(tactical > f(0) && tactical < f(90), "battle allowance uses retained available category cash");
    const auto switched = manager.state();
    take(manager.notify({galactic::GalacticSession::CreditNotification::Kind::windfall, 1, tactical, f(3), {}}));
    const auto windfall = manager.state();
    expect(windfall.at(1).tactical == switched.at(1).tactical
        && windfall.at(1).goals == switched.at(1).goals && windfall.at(1).dead_goals == switched.at(1).dead_goals,
        "WGA-200 child windfall preserves fractional/live/dead obligations and raw release tracking");
    for (const auto& [id, row] : switched.at(1).categories)
        expect(windfall.at(1).categories.at(id).available_cash == row.available_cash,
            "child windfall updates retained totals without making cash available");
    expect(!manager.notify({galactic::GalacticSession::CreditNotification::Kind::fiscal, 1, tactical, f(3), f(1)})
        && manager.state() == windfall, "frozen parent refuses fiscal service transactionally");
    const auto released = take(manager.release_tactical(1, {f(5), tactical}));
    ai::GalacticFunding restored;
    take(restored.restore(manager.state()));
    const auto result = take(restored.restore_strategic(1, released));
    expect(result == f(90), "copied switch DTO conserves released and saved credits");
    expect(take(restored.restore_strategic(1, result)) == result, "restoration of switched balance is once only");
    expect(!restored.ledger(1)->tactical.switched, "restored switch state clears");
}
void over_cost_mark() {
    ai::GalacticFunding manager; take(manager.initialize(1, std::array{policy()}, f(100)));
    take(manager.reserve(1, {7, 1, f(20), f(1), {}, {}}));
    const auto before = manager.ledger(1); take(manager.mark_spending(1, 7, f(30)));
    const auto after = manager.ledger(1);
    expect(after->goals.at(7).marked == f(30) && after->goals.at(7).cost == f(20) && after->categories == before->categories,
        "base type mark exceeds computed cost without allocation or cash change");
}
void orphan_aggregate() {
    ai::GalacticFunding manager; take(manager.initialize(1, std::array{policy()}, f(100)));
    take(manager.reserve(1, {7, 1, f(20), f(1), {}, {}})); take(manager.mark_spending(1, 7, f(10)));
    take(manager.remove_goal(1, 7));
    const auto before = manager.ledger(1);
    expect(before->dead_budget == f(10), "removed marked goal contributes to aggregate dead budget");
    take(manager.spend(1, 0, f(15)));
    const auto after = manager.ledger(1);
    expect(after->dead_budget == Fixed{} && after->categories == before->categories && after->dead_goals == before->dead_goals,
        "null-goal orphan spending floors aggregate without category or keyed-row mutation");
    const auto unchanged = manager.state(); take(manager.spend(1, 0, f(15)));
    expect(manager.state() == unchanged, "empty orphan budget never rejects a producer bill");
}
void template_untrigger() {
    ai::GalacticFunding manager;
    const std::array templates{policy(), ai::EvaluatedFundingTemplate{
        5, {{1, f(3)}}, {{1, f(1)}}}};
    take(manager.initialize(1, templates, f(100)));
    take(manager.reserve(1, {7, 1, f(20), f(1), {}, {}}));
    take(manager.mark_spending(1, 7, f(10)));
    const auto before = manager.ledger(1);
    // An ordinary lower-priority null setter does not implement untrigger.
    take(manager.apply_template(1, {1, {{1, std::nullopt}}, {}}));
    expect(manager.ledger(1)->categories.at(1).evaluated_weight == f(3),
        "ordinary equation setters retain independent priority protection");
    take(manager.clear_equations(1));
    auto expected = *before;
    for (auto& [id, category] : expected.categories) {
        (void)id; category.evaluated_weight.reset(); category.equation_priority = 1;
    }
    expected.equations_changed = true;
    expect(*manager.ledger(1) == expected, "untrigger clears all equations and preserves every other retained field");
    take(manager.apply_template(1, policy()));
    expect(manager.ledger(1)->categories.at(1).evaluated_weight == f(1)
        && manager.ledger(1)->categories.at(1).equation_priority == 1,
        "surviving lower-priority template can replay after reset");
    expect(manager.ledger(1)->categories.at(1).allowance == f(1)
        && manager.ledger(1)->categories.at(1).allowance_priority == 5,
        "allowance priority survives untrigger and rejects lower-priority replay");
    expect(manager.ledger(1)->goals == before->goals && cash(manager) == f(100),
        "template reset/replay keeps reserved and marked money without granting credits");
    const auto unchanged = manager.state();
    expect(!manager.clear_equations(2) && manager.state() == unchanged,
        "missing manager reset is an atomic diagnostic");
}
void failed_funding_equations() {
    ai::AiData data;
    data.goal_categories.emplace("One", 1); data.goal_categories.emplace("Two", 2);
    auto host = std::make_shared<eawr::script::foc::detail::Host>();
    ai::Engine engine(host, std::move(data), {});
    auto manager = std::make_shared<ai::GalacticFunding>();
    take(manager->initialize(1, std::array{ai::EvaluatedFundingTemplate{
        1, {{1, f(3)}, {2, f(1)}}, {{3, f(1)}}}}, f(100)));
    const auto before = manager->ledger(1);
    eawr::script::foc::galactic_ai::TemplatePolicy selected;
    selected.equations.emplace("One", eawr::script::foc::galactic_ai::Setting<std::string>{"Unavailable_One", 1});
    selected.equations.emplace("Two", eawr::script::foc::galactic_ai::Setting<std::string>{"Unavailable_Two", 1});
    expect(!engine.evaluate("Unavailable_One", 1, nullptr), "missing funding equation remains an unavailable query");
    ai::EngineContractAccess::maintain(engine, 1, manager, selected);
    auto after = manager->ledger(1);
    expect(after->categories.at(1).evaluated_weight == before->categories.at(1).share
        && after->categories.at(2).evaluated_weight == before->categories.at(2).share
        && after->categories.at(1).share == before->categories.at(1).share
        && after->categories.at(2).share == before->categories.at(2).share,
        "WGA-421 failed present equations preserve current shares, rather than old weights or zero");
    expect(!after->categories.at(3).evaluated_weight && after->categories.at(3).share == Fixed{},
        "WGA-421 allowance-only maintenance share is zero without creating an equation");
    selected.equations.at("One").value.clear();
    ai::EngineContractAccess::maintain(engine, 1, manager, selected);
    after = manager->ledger(1);
    expect(!after->categories.at(1).evaluated_weight && after->categories.at(1).share == Fixed{}
        && after->categories.at(2).share == f(1),
        "WGA-421 absent equation resets share while failed present equation remains in normalization");
    selected.equations.at("Two").value.clear();
    ai::EngineContractAccess::maintain(engine, 1, manager, selected);
    after = manager->ledger(1);
    const auto equal = take(Fixed::from_ratio(1, 3));
    const auto residual = take(eawr::sim::math::subtract(f(1), take(eawr::sim::math::add(equal, take(eawr::sim::math::add(equal, equal))))));
    expect(after->categories.at(1).share == take(eawr::sim::math::add(equal, residual))
        && after->categories.at(2).share == equal && after->categories.at(3).share == equal
        && take(eawr::sim::math::add(take(eawr::sim::math::add(after->categories.at(1).share,
            after->categories.at(2).share)), after->categories.at(3).share)) == f(1),
        "WGA-69/421 equal shares use GFP-01 project fixed-point residual policy");
    for (const auto& [id, category] : after->categories)
        expect(category.cash == before->categories.at(id).cash && category.previous_share == before->categories.at(id).previous_share,
            "equation failure and absence preserve money and previous income share");
    ai::GalacticFunding restored; take(restored.restore(manager->state()));
    expect(restored.state() == manager->state(), "funding failure shares round trip through the retained owner");
}
void fresh_category_failed_initial_equation() {
    ai::AiData data;
    data.equations = take(ai::EquationSet::parse({{"funding.xml", "<Equations><Positive>3</Positive><Failed>Variable_Self.Unavailable_Field</Failed></Equations>"}}, {}));
    auto host = std::make_shared<eawr::script::foc::detail::Host>();
    ai::GalacticPerceptionSetup perception;
    perception.players = {{1, 1, "Empire", "NORMAL", false, true, false, 0}};
    auto world = std::make_shared<ai::GalacticWorld>(perception); host->ai_world = world;
    take(world->bind_economy_runtime(ai::fresh_economy_runtime(perception.players)));
    auto galaxy = take(galactic::GalacticSession::create(setup()));
    const eawr::sim::InlineExecutor executor;
    take(world->bind(galaxy.snapshot(), {}, executor, 0, galaxy.next_entity_id()));
    ai::Engine engine(host, std::move(data), {});
    const auto failed = engine.evaluate("Failed", 1, nullptr);
    expect(!failed, "fresh authored funding equation really fails evaluation");
    const auto initial_weight = take(ai::funding_equation_weight(nullptr, 1, failed));
    expect(initial_weight == Fixed{}, "WGA-421 missing startup ledger retains constructor-zero output on failure");
    const auto positive = take(ai::funding_equation_weight(nullptr, 2, engine.evaluate("Positive", 1, nullptr)));
    expect(positive == f(3), "actual defined positive equation evaluates against the bound world");
    auto manager = std::make_shared<ai::GalacticFunding>();
    take(manager->initialize(1, std::array{ai::EvaluatedFundingTemplate{1, {{1, initial_weight}, {2, positive}}, {}}}, f(100)));
    const auto ledger = manager->ledger(1);
    expect(ledger->categories.at(1).evaluated_weight == Fixed{} && ledger->categories.at(1).share == Fixed{}
        && ledger->categories.at(1).cash == Fixed{} && ledger->categories.at(2).cash == f(100),
        "WGA-421 fresh failed equation stays present with zero weight through real initialization/distribution");
    expect(take(ai::funding_equation_weight(ledger.get(), 3, failed)) == Fixed{},
        "a fresh category added to an existing ledger also has constructor-zero failure output");
    auto uninitialized = std::make_shared<ai::GalacticFunding>();
    bool rejected{};
    try { ai::EngineContractAccess::maintain(engine, 1, uninitialized, {}); }
    catch (const std::runtime_error&) { rejected = true; }
    expect(rejected && uninitialized->state().empty(), "maintenance refuses a missing initialized ledger atomically");
}
}
int main() {
    try { live_fiscal(); windfalls_and_failure(); zero_income_and_refusal(); retained_transactions(); template_untrigger(); orphan_aggregate(); over_cost_mark(); failed_funding_equations(); fresh_category_failed_initial_equation(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures ? 1 : 0;
}

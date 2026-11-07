#include "eawr/script/foc/provider_transaction.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../support/check.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sstream>

namespace {
namespace ga = eawr::script::foc::galactic_ai;
namespace foc = eawr::script::foc;
using Result = eawr::core::Result<void>;
using Commit = ga::ProviderTransaction::Commit;
eawr::testing::Checker checks;
void expect(bool condition, std::string_view message) { checks.check(condition, message, std::cerr); }
Result fail() {
    eawr::core::Diagnostic error;
    error.code = "provider-fixture";
    error.message = "deterministic fixture failure";
    return Result::failure(std::move(error));
}

std::string journal_rows(const foc::AiJournal& journal) {
    std::ostringstream out;
    for (const auto& row : journal.plans)
        out << row.tick << '|' << row.player << '|' << row.plan << '|' << row.goal << '|'
            << row.target << '|' << row.event << '|' << row.detail << '\n';
    for (const auto& [key, row] : journal.proposals) {
        out << key << '|' << row.player << '|' << row.goal << '|' << row.event << '|'
            << row.reason << '|' << row.count << '|' << row.first_tick << '|' << row.last_tick << '|' << row.example_detail;
        for (const auto& target : row.targets) out << '|' << target;
        out << '\n';
    }
    for (const auto& row : journal.costs)
        out << row.tick << '|' << row.freestore_instructions << '|' << row.plan_instructions << '|'
            << row.plan_instances << '|' << row.goals_evaluated << '|' << row.maintenances << '|'
            << row.plans_attached << '|' << row.plans_deferred << '|' << row.plans_pumped << '|'
            << row.freestore_runs << '|' << row.reinforcement_candidates << '|' << row.reinforcement_search_ns << '|'
            << row.object_instances << '|' << row.object_turn_due_ticks << '|' << row.object_turn_rows << '|' << row.object_pumps << '\n';
    for (const auto& row : journal.services)
        out << row.frame << '|' << row.player << '|' << row.goals << '|' << row.planning << '|' << row.execution << '\n';
    const auto& stats = journal.evaluators;
    out << stats.calls << '|' << stats.memo_hits << '|' << stats.failures << '|' << stats.modules_loaded << '|' << stats.instructions
        << '|' << journal.plans_dropped << '|' << journal.costs_dropped << '|' << journal.services_dropped;
    return out.str();
}

void journal_phase_rollback() {
    foc::AiJournal initial;
    // Force every bounded row kind to evict old history on a direct append.
    initial.plans.resize(foc::ai_diagnostic_capacity);
    initial.costs.resize(foc::ai_diagnostic_capacity);
    initial.services.resize(foc::ai_diagnostic_capacity);
    initial.plans.front().detail = "retained oldest plan";
    initial.costs.front().tick = 17;
    initial.services.front().frame = 19;
    initial.append_plan({1, 1, "proposal", "GOAL", "old", "rejected", "reason\noriginal trace"});
    initial.set_evaluators({1, 2, 3, 4, 5});
    const auto before = journal_rows(initial);
    const auto append = [](foc::AiJournal& journal) {
        journal.append_plan({2, 1, "proposal", "GOAL", "new", "rejected", "reason\nnew trace"});
        journal.append_plan({2, 1, "plan", "GOAL", "target", "finished", "detail"});
        journal.append_raw_plan({2, 1, "proposal", "RAW", "target", "event", "raw"});
        journal.fold_proposal({3, 1, "direct", "DIRECT", "target", "event", "direct fold"});
        foc::AiTickCost cost; cost.tick = 2; cost.plan_instances = 3;
        journal.append_cost(cost);
        journal.append_service({2, 1, true, true, false});
        journal.set_evaluators({6, 7, 8, 9, 10});
    };
    foc::AiJournal oracle = initial;
    append(oracle);
    const auto after = journal_rows(oracle);
    expect(after != before && oracle.plans_dropped > 0 && oracle.costs_dropped > 0 && oracle.services_dropped > 0,
        "journal fixture folds and evicts real retained observer history");
    {
        for (const bool throws : {false, true}) for (const bool after_mutation : {false, true}) {
            foc::AiJournal journal = initial;
            bool caught{};
            try {
                auto phase = journal.begin_phase();
                expect(phase->pending_intents() == 0, "journal begin retains no rows or history copies");
                bool nested_rejected{};
                try { static_cast<void>(journal.begin_phase()); } catch (const std::logic_error&) { nested_rejected = true; }
                expect(nested_rejected, "journal rejects nested phases");
                if (after_mutation) append(journal);
                expect(journal_rows(journal) == before, "late callback cannot expose provisional reports");
                expect(phase->pending_intents() == (after_mutation ? 7U : 0U), "only new report intents retained");
                const auto failed = [&]() -> Result {
                    if (throws) throw std::runtime_error("late after_service fixture");
                    return fail();
                }();
                expect(!failed, "late Result failure remains a failure");
            } catch (const std::runtime_error& error) { caught = std::string_view(error.what()) == "late after_service fixture"; }
            expect(caught == throws && journal_rows(journal) == before,
                "Result and exception discard restore exact rows, folds, evaluator stats and drop counts");
            auto retry = journal.begin_phase();
            append(journal);
            retry->commit();
            expect(journal_rows(journal) == after, "retry publishes every observer report exactly once in original order");
        }
    }
}

struct Slice {
    int value{};
    std::vector<int> journal;
};

void rollback_and_siblings() {
    // Synthetic owner slices stand for retained lifecycle, service owner,
    // scheduler attachments/events, world journal and outbound report.
    for (const bool throws : {false, true}) for (const std::size_t fault : {std::size_t{0}, std::size_t{1}, std::size_t{5}}) {
        std::array<std::unique_ptr<ga::ProviderState<Slice>>, 5> owners;
        for (auto& owner : owners) owner = std::make_unique<ga::ProviderState<Slice>>(Slice{});
        ga::ProviderTransaction failed;
        bool caught{};
        try {
            auto result = failed.run([&]() -> Result {
                for (std::size_t index = 0; index <= owners.size(); ++index) {
                    if (index == fault) {
                        if (throws) throw std::runtime_error("deterministic provider exception");
                        return fail();
                    }
                    if (index < owners.size()) {
                        auto staged = failed.stage(*owners[index], [](Slice& row) {
                            row.value = 42;
                            row.journal.push_back(42);
                            return Result::success();
                        });
                        if (!staged) return staged;
                    }
                }
                return Result::success();
            });
            expect(!result && result.error().code == "provider-fixture", "Result failure stays specific");
        } catch (const std::runtime_error& error) {
            caught = std::string_view(error.what()) == "deterministic provider exception";
        }
        expect(caught == throws, "deterministic exception propagates unchanged");
        expect(failed.commit() == Commit::rejected, "failed chain cannot publish any prior mutation");
        for (const auto& owner : owners)
            expect(owner->snapshot()->value == 0 && owner->snapshot()->journal.empty(), "all five publications survive failed chain");

        ga::ProviderTransaction sibling;
        for (auto& owner : owners) expect(bool(sibling.stage(*owner, [](Slice& row) {
            ++row.value;
            row.journal.push_back(1);
            return Result::success();
        })), "sibling stages after failed provider");
        expect(sibling.commit() == Commit::committed, "sibling chain still commits");
        for (const auto& owner : owners)
            expect(owner->snapshot()->value == 1 && owner->snapshot()->journal == std::vector<int>{1}, "only successful sibling output is published");
    }
}

struct Counted {
    int value{};
    std::shared_ptr<std::size_t> copies;
    std::shared_ptr<bool> throw_copy;
    std::shared_ptr<std::size_t> destructions{std::make_shared<std::size_t>(0)};
    Counted(int initial, std::shared_ptr<std::size_t> count, std::shared_ptr<bool> throws)
        : value(initial), copies(std::move(count)), throw_copy(std::move(throws)) {}
    Counted(const Counted& other) : value(other.value), copies(other.copies), throw_copy(other.throw_copy),
        destructions(other.destructions) {
        if (*throw_copy) throw std::runtime_error("copy failure");
        ++*copies;
    }
    Counted(Counted&&) noexcept = default;
    ~Counted() { if (destructions) ++*destructions; }
};

void work_and_publication() {
    auto copies = std::make_shared<std::size_t>(0);
    auto throw_copy = std::make_shared<bool>(false);
    ga::ProviderState<Counted> a(Counted(3, copies, throw_copy)), b(Counted(7, copies, throw_copy));
    const auto old = a.snapshot();
    {
        ga::ProviderTransaction read_only;
        expect(read_only.read(a).value == 3 && read_only.read(b).value == 7, "reads enlist immutable publications");
        expect(*copies == 0 && read_only.work().detached == 0, "read-only chain copies no owners");
        expect(read_only.commit() == Commit::committed && read_only.work().published == 0, "read-only commit publishes nothing");
    }
    ga::ProviderTransaction transaction;
    for (int index = 0; index < 20; ++index) expect(bool(transaction.stage(a, [](Counted& row) {
        ++row.value;
        return Result::success();
    })), "repeated writes share the staged owner");
    expect(transaction.read(a).value == 23 && a.snapshot() == old, "chain sees its own writes while publication stays unchanged");
    expect(transaction.read(b).value == 7, "untouched owner is a read dependency");
    expect(*copies == 1 && transaction.work().enlisted == 2 && transaction.work().detached == 1,
        "work is one copy per written slice, independent of callback count");
    expect(transaction.commit() == Commit::committed && transaction.work().published == 1, "successful chain publishes only its dirty owner");
    expect(a.snapshot()->value == 23 && old->value == 3 && b.snapshot()->value == 7, "retained snapshots stay immutable across commit");
    expect(transaction.commit() == Commit::closed, "second commit cannot republish");
    bool closed_read{};
    try { static_cast<void>(transaction.read(a)); } catch (const std::logic_error&) { closed_read = true; }
    expect(closed_read, "closed transaction cannot return staged references");

    const auto destructions = a.snapshot()->destructions;
    const auto before_discard = *destructions;
    {
        ga::ProviderTransaction abandoned;
        expect(bool(abandoned.stage(a, [](Counted& row) { row.value = 100; return Result::success(); })), "abandoned staging");
    }
    expect(a.snapshot()->value == 23 && *destructions == before_discard + 1,
        "destruction frees the staged copy and preserves the publication");
    ga::ProviderTransaction copy_failure;
    *throw_copy = true;
    bool caught{};
    try { static_cast<void>(copy_failure.stage(a, [](Counted&) { return Result::success(); })); }
    catch (const std::runtime_error&) { caught = true; }
    expect(caught && copy_failure.commit() == Commit::rejected && a.snapshot()->value == 23,
        "copy exceptions poison the chain and preserve the published owner");
}

void validate_before_publication() {
    ga::ProviderState<int> a(1), b(2);
    ga::ProviderTransaction stale, winner;
    expect(bool(stale.stage(a, [](int& value) { value = 10; return Result::success(); })), "first owner staged");
    expect(stale.read(b) == 2, "read-only dependencies are validated too");
    expect(bool(winner.stage(b, [](int& value) { value = 20; return Result::success(); })), "conflicting publisher stages");
    expect(winner.commit() == Commit::committed, "ordered winner publishes");
    expect(stale.commit() == Commit::conflict && stale.work().published == 0, "later conflict rejects before publishing earlier dirty owner");
    expect(*a.snapshot() == 1 && *b.snapshot() == 20, "no half-published conflicted chain");
    ga::ProviderTransaction nested;
    expect(bool(nested.stage(a, [&](int& value) {
        value = 30;
        expect(nested.commit() == Commit::busy, "callback cannot commit while it still owns a mutable reference");
        return Result::success();
    })), "busy commit preserves open chain");
    expect(nested.commit() == Commit::committed && *a.snapshot() == 30, "joined callback permits ordered publication");

    for (const bool throws : {false, true}) {
        ga::ProviderTransaction poisoned;
        bool caught{};
        try {
            auto result = poisoned.stage(a, [throws](int& value) -> Result {
                value = 100;
                if (throws) throw std::runtime_error("writer exception");
                return fail();
            });
            expect(!result, "writer Result failure propagates");
        } catch (const std::runtime_error&) { caught = true; }
        expect(caught == throws && poisoned.commit() == Commit::rejected && *a.snapshot() == 30,
            "writer failure after mutation rejects even when caller catches it");
    }
}

std::vector<int> partitioned_chain(std::size_t workers) {
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::array<std::unique_ptr<ga::ProviderState<int>>, 8> owners;
    std::array<std::unique_ptr<ga::ProviderTransaction>, 8> staged;
    std::array<bool, 8> results{};
    for (std::size_t index = 0; index < owners.size(); ++index) {
        owners[index] = std::make_unique<ga::ProviderState<int>>(0);
        staged[index] = std::make_unique<ga::ProviderTransaction>();
    }
    auto prepared = executor.execute(owners.size(), [&](std::size_t index) {
        auto result = staged[index]->stage(*owners[index], [index](int& value) {
            value = static_cast<int>(index + 1);
            if (index == 3) return fail();
            return Result::success();
        });
        results[index] = bool(result);
    });
    expect(bool(prepared), "disjoint provider staging joins successfully");
    std::vector<int> published;
    for (std::size_t index = 0; index < owners.size(); ++index) {
        expect(results[index] == (index != 3), "one provider fails after mutation in a partition");
        expect(staged[index]->commit() == (index == 3 ? Commit::rejected : Commit::committed), "ordered barrier commits successful siblings only");
        published.push_back(*owners[index]->snapshot());
    }
    return published;
}
} // namespace

int main() {
    journal_phase_rollback();
    rollback_and_siblings();
    work_and_publication();
    validate_before_publication();
    const std::vector<int> expected{1, 2, 3, 0, 5, 6, 7, 8};
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}})
        expect(partitioned_chain(workers) == expected, "provider staging and ordered publication equal on 1/2/4/8 workers");
    return checks.failures == 0 ? 0 : 1;
}

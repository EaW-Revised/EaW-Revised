#include "eawr/script/foc/galactic_ai.hpp"

#include <iostream>
#include <type_traits>

namespace a = eawr::script::foc::galactic_ai;
static_assert(std::is_copy_constructible_v<a::WorldInput>);
static_assert(std::is_copy_constructible_v<a::NativeState>);
static_assert(std::is_abstract_v<a::Queries> && std::is_abstract_v<a::Actions>);
static_assert(std::is_abstract_v<a::Funding> && std::is_abstract_v<a::StateContributor>);
static_assert(static_cast<std::size_t>(a::Service::templates) + 1 == a::service_count);

class FundingFixture final : public a::Funding {
public:
    eawr::core::Result<void> notify(const a::FundingNotice&) override {
        live.last_interval = eawr::sim::math::Fixed::from_raw(12);
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<bool> feasible(const a::FundingTransaction&) const override {
        return eawr::core::Result<bool>::success(true);
    }
    eawr::core::Result<void> reserve(const a::FundingTransaction& transaction) override {
        live.goals[transaction.request.id] = {};
        return eawr::core::Result<void>::success();
    }
    std::shared_ptr<const a::gc::funding::Ledger> ledger(a::gc::PlayerId) const override {
        return std::make_shared<const a::gc::funding::Ledger>(live);
    }
private:
    a::gc::funding::Ledger live;
};

int main() {
    a::WorldInput original;
    original.clocks = {900, 30};
    original.targets.push_back({7, 2, {}, {}});
    a::ProducerInput producer; producer.type = 42;
    original.producers.push_back(producer);
    const auto retained = std::make_shared<const a::WorldInput>(original);
    original.targets.front().owner = 3;
    original.clocks = {901, 31};
    original.producers.front().type = 43;
    if (retained->targets.front().owner != 2 || retained->clocks != a::Clocks{900, 30}
        || retained->producers.front().type != 42) {
        std::cerr << "copied parent inputs changed across child service\n";
        return 1;
    }
    a::FundingNotice notice;
    notice.key = {900, 16, 2, 1};
    notice.kind = a::NoticeKind::billed;
    notice.goal = 42;
    a::NativeState saved;
    saved.components["funding"] = {1, 2, 3};
    const auto copy = saved;
    saved.components["funding"].clear();
    if (copy.components.at("funding").size() != 3 || notice.goal != 42) return 1;
    a::Capabilities missing;
    if (missing.queries || missing.actions || missing.funding || missing.services || missing.state) return 1;
    FundingFixture funding;
    const auto snapshot = funding.ledger(2);
    if (!funding.notify(notice)) return 1;
    a::FundingTransaction transaction;
    transaction.request.id = 42;
    if (!funding.reserve(transaction)) return 1;
    const auto current = funding.ledger(2);
    if (!snapshot->goals.empty() || snapshot->last_interval.raw() != 0 ||
        current->goals.size() != 1 || current->last_interval.raw() != 12) return 1;
    std::cout << "galactic AI copied-input/capability/state contracts passed\n";
    return 0;
}

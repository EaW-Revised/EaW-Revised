#include "eawr/presentation/shader_activation.hpp"
#include "eawr/core/sha256.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace eawr::presentation::shader_activation;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("activation line " + std::to_string(__LINE__) + ": " #x); } while(false)
struct Payload final : Artifact {};
auto payload() { return std::make_shared<Payload>(); }
Input input(std::string name = "invented") {
    Input result; result.effect = std::move(name); result.layer = "parent";
    result.logical = "data/art/invented.fxo"; result.mount = "archive";
    result.bytes = {std::byte{1}, std::byte{2}};
    result.sha256 = eawr::core::sha256_hex({reinterpret_cast<const std::uint8_t*>(result.bytes.data()), result.bytes.size()});
    return result;
}
Operations operations(std::size_t& preparations, std::size_t& admissions) {
    return {[&](const Input&, const Cancellation&) {
        ++preparations; return Prepared{payload(), {{2, 1}, {0, 0}, {2, 1}}, {}};
    }, [&](const Input&, const Prepared&, Variant variant, const Cancellation&) {
        ++admissions;
        if (variant.technique == 2U) return Admitted{};
        return Admitted{payload(), {}};
    }};
}
void deterministic_work_and_routes() {
    std::size_t preparations = 0, admissions = 0;
    auto a = input("a"); a.adapter = "a";
    auto b = input("b");
    Job job(1, "chain", {b, a, a}, operations(preparations, admissions));
    job.step(1);
    REQUIRE(job.progress().state == State::running && !job.result());
    REQUIRE(job.progress().effects == 2 && job.progress().variants == 2);
    job.step(2);
    REQUIRE(job.progress().work == 3 && job.progress().prepared == 1);
    job.step(100);
    REQUIRE(job.progress().state == State::complete);
    REQUIRE(preparations == 2 && admissions == 4 && job.progress().work == 6);
    REQUIRE(job.progress().admitted == 2 && job.progress().failed_variants == 2);
    const auto generation = job.result();
    REQUIRE(generation->effects[0].input.effect == "a");
    REQUIRE(route(generation->effects[0], {0, 0}) == report::Route::translated);
    REQUIRE(route(generation->effects[0], {2, 1}) == report::Route::hand_adapter_approximation);
    REQUIRE(route(generation->effects[1], {2, 1}) == report::Route::surface_omitted);
    REQUIRE(route(generation->effects[0], {999, 99}) == report::Route::hand_adapter_approximation);
    REQUIRE(preparations == 2 && admissions == 4); // lookup schedules nothing
    std::size_t other_preparations = 0, other_admissions = 0;
    Job other(2, "chain", {a, b}, operations(other_preparations, other_admissions));
    while (other.progress().state == State::running) other.step(1);
    REQUIRE(other.progress().work == job.progress().work);
    REQUIRE(other_preparations == preparations && other_admissions == admissions);
}
void policy_and_missing() {
    auto blocked = input("blocked"); blocked.policy = Policy::opted_out;
    blocked.bytes.clear(); blocked.sha256.clear(); blocked.adapter = "blocked";
    auto malformed = blocked; malformed.effect = "malformed"; malformed.policy = Policy::malformed;
    auto missing = blocked; missing.effect = "missing"; missing.form = report::SelectedForm::absent;
    auto source = input("source"); source.form = report::SelectedForm::source;
    std::size_t preparations = 0, admissions = 0;
    Job job(1, "chain", {blocked, malformed, missing, source}, operations(preparations, admissions));
    job.step(100);
    REQUIRE(job.progress().state == State::complete && preparations == 1 && admissions == 2);
    const auto generation = job.result();
    REQUIRE(generation->effects[0].diagnostics[0].reason == report::Reason::opted_out);
    REQUIRE(generation->effects[1].diagnostics[0].reason == report::Reason::malformed_policy);
    REQUIRE(route(generation->effects[2], {}) == report::Route::missing_effect_green);
    REQUIRE(route(generation->effects[3], {2, 1}) == report::Route::surface_omitted);
}
void publication_and_cancellation() {
    std::size_t preparations = 0, admissions = 0;
    Publisher publisher;
    Job first(publisher.begin(), "first", {input()}, operations(preparations, admissions));
    REQUIRE(!publisher.publish(first));
    first.step(100); REQUIRE(publisher.publish(first));
    const auto pinned = publisher.active();
    REQUIRE(!publisher.publish(first));
    Job stale(publisher.begin(), "stale", {input()}, operations(preparations, admissions));
    Job newest(publisher.begin(), "newest", {input()}, operations(preparations, admissions));
    stale.step(100); REQUIRE(!publisher.publish(stale));
    newest.step(100); REQUIRE(publisher.publish(newest));
    REQUIRE(pinned->chain_identity == "first");
    Job cancelled(publisher.begin(), "cancelled", {input()}, operations(preparations, admissions));
    cancelled.step(100); publisher.cancel(cancelled.result()->id);
    REQUIRE(!publisher.publish(cancelled));
    REQUIRE(publisher.active()->chain_identity == "newest");
    Job cooperative(publisher.begin(), "cooperative", {input()}, operations(preparations, admissions));
    cooperative.step(1); cooperative.cancel(); cooperative.step(100);
    REQUIRE(cooperative.progress().state == State::cancelled && !cooperative.result());
    REQUIRE(!publisher.publish(cooperative));
    Job* current = nullptr;
    auto callbacks = operations(preparations, admissions);
    callbacks.admit = [&](const Input&, const Prepared&, Variant, const Cancellation&) {
        current->cancel(); return Admitted{payload(), {}};
    };
    Job during(publisher.begin(), "during", {input()}, std::move(callbacks)); current = &during;
    during.step(100); REQUIRE(during.progress().state == State::cancelled && !during.result());
    auto typed = operations(preparations, admissions);
    typed.prepare = [](const Input&, const Cancellation&) {
        return Prepared{{}, {}, {{report::FailureStage::stage_compile, report::Reason::cancelled, report::Severity::warning}}};
    };
    Job typed_cancel(publisher.begin(), "typed-cancel", {input()}, std::move(typed));
    typed_cancel.step(100); REQUIRE(typed_cancel.progress().state == State::cancelled && !publisher.publish(typed_cancel));
}
void limits_and_failure() {
    std::size_t preparations = 0, admissions = 0;
    auto altered = input(); altered.bytes.push_back(std::byte{});
    Job bad_hash(1, "chain", {altered}, operations(preparations, admissions));
    REQUIRE(bad_hash.progress().state == State::failed && !bad_hash.result());
    altered = input(); altered.adapter = "different";
    Job conflict(1, "chain", {input(), altered}, operations(preparations, admissions));
    REQUIRE(conflict.progress().state == State::failed);
    auto limits = Limits{}; limits.input_bytes = 1;
    Job bytes(1, "chain", {input()}, operations(preparations, admissions), limits);
    REQUIRE(bytes.progress().state == State::failed);
    limits = {}; limits.variants = 1;
    Job variants(1, "chain", {input()}, operations(preparations, admissions), limits);
    variants.step(100); REQUIRE(variants.progress().state == State::failed && admissions == 0);
    limits.variants = 2;
    Job deduped(1, "chain", {input()}, operations(preparations, admissions), limits);
    deduped.step(100); REQUIRE(deduped.progress().state == State::complete);
    limits = {}; limits.diagnostics = 0;
    auto missing = Input{}; missing.effect = "missing"; missing.form = report::SelectedForm::absent;
    Job diagnostics(1, "chain", {missing}, {}, limits);
    diagnostics.step(100); REQUIRE(diagnostics.progress().state == State::failed);
    auto throwing = operations(preparations, admissions);
    throwing.prepare = [](const Input&, const Cancellation&) -> Prepared { throw std::runtime_error("owner failure"); };
    Job failed(1, "chain", {input()}, throwing); failed.step(100);
    REQUIRE(failed.progress().state == State::failed && !failed.result());
    Job empty(1, "empty", {}, {}); REQUIRE(empty.progress().state == State::complete);
}
}
int main() {
    try { deterministic_work_and_routes(); policy_and_missing(); publication_and_cancellation(); limits_and_failure(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

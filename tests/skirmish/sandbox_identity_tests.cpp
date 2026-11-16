// Sandbox policy v4 binds into the skirmish match identity: a peer on the previous policy gets another identity that
// differs in the sandbox domain alone (no game data). The setup refusal itself runs in net_setup_contracts.
#include "../support/check.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/match_identity.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

const eawr::sim::IdentityDomain* domain_named(const eawr::sim::MatchIdentity& identity, std::string_view name) {
    const auto found = std::find_if(identity.domains.begin(), identity.domains.end(),
        [&](const eawr::sim::IdentityDomain& domain) { return domain.name == name; });
    return found == identity.domains.end() ? nullptr : &*found;
}

} // namespace

int main() {
    namespace auth = eawr::script::authoritative;
    eawr::testing::Checker checker;
    const auto check = [&](bool condition, std::string_view message) { checker.check(condition, message, std::cerr); };

    check(auth::sandbox_policy_identity == "eawr-lua-sandbox-v4", "this build carries sandbox policy v4");

    // Synthetic inputs: the same start on two builds that differ only in the sandbox policy they were built with.
    eawr::units::UnitTables tables;
    eawr::skirmish::StartInputs inputs;
    inputs.map = "data/art/maps/test.ted";
    inputs.map_sha256 = std::string(64, 'a');
    inputs.tables = &tables;
    const eawr::sim::MatchIdentity v4 = eawr::skirmish::match_identity(inputs);

    const auto* sandbox = domain_named(v4, "sandbox");
    check(sandbox != nullptr && *sandbox == eawr::sim::identity_domain("sandbox", "eawr-lua-sandbox-v4"),
        "the match identity's sandbox domain is the v4 policy identity");

    inputs.sandbox_policy = "eawr-lua-sandbox-v3";
    const eawr::sim::MatchIdentity v3 = eawr::skirmish::match_identity(inputs);
    check(v3 != v4 && v3.sha256 != v4.sha256, "a v3 peer has another match identity");
    check(eawr::sim::validate_match_identity(v3).has_value(), "the v3 identity is well formed, only different");

    // The comparison setup and lobby make on every offer, in both directions, with the build identity equal.
    const eawr::sim::MatchAgreement host{"build", v4}, peer{"build", v3};
    const auto refusal = eawr::sim::describe_mismatch(host, peer);
    check(refusal && refusal->code == eawr::sim::match_identity_mismatch_code &&
            refusal->message == "match identity mismatch: sandbox",
        "a v3 peer is refused by the v4 host, naming the sandbox domain");
    const auto reverse = eawr::sim::describe_mismatch(peer, host);
    check(reverse && reverse->message == "match identity mismatch: sandbox", "a v4 peer is refused by the v3 host");
    check(!eawr::sim::describe_mismatch(host, host), "two v4 peers agree");

    // Only the sandbox domain moved: nothing else in the start is touched by the policy.
    for (const auto& domain : v4.domains) {
        const auto* other = domain_named(v3, domain.name);
        check(other != nullptr && (domain.name == "sandbox" ? *other != domain : *other == domain),
            "domain " + domain.name + (domain.name == "sandbox" ? " differs" : " is the same"));
    }
    return checker.finish();
}

// #3768: the galactic-campaign-v1 setup proposal over the existing setup/readiness service. Two peers and eight
// seats reach the tick-zero barrier on identities each peer derived from its own content; every refusal happens
// before a shared step; the skirmish proposal bytes keep their documented layout.
#include "setup_test_support.hpp"
#include "fake_transport.hpp"

#include "eawr/campaign/mp_agreement.hpp"
#include "eawr/net/galactic_setup.hpp"
#include "eawr/sim/world.hpp"

#include <array>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace setup_test;
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace dg = eawr::data::galactic;
using Fixed = eawr::sim::math::Fixed;
using Digest = std::array<std::uint8_t, 32>;

Fixed whole(std::int64_t value) { return Fixed::from_integer(value).value(); }

struct Names {
    std::string a{"REBEL"}, b{"EMPIRE"}, ai{"UNDERWORLD"}, extra_ai, neutral{"NEUTRAL"};
};

dg::GalacticData make_data(const Names& names) {
    dg::GalacticData d;
    std::vector<std::string> factions{names.a, names.b, names.ai};
    if (!names.extra_ai.empty()) factions.push_back(names.extra_ai);
    for (const auto& name : factions) {
        dg::Faction row; row.name = name; row.is_neutral = false; row.is_playable = name == names.a || name == names.b;
        row.displayed_tech_level_adjustment = 0;
        row.multiplayer_campaign_heroes = {"Hero", "Hero"};
        d.factions.push_back(row);
    }
    dg::Faction neutral; neutral.name = names.neutral; neutral.is_neutral = true; neutral.displayed_tech_level_adjustment = 0;
    d.factions.push_back(neutral);
    dg::Planet planet; planet.name = "P"; planet.galactic_position = dg::Position{}; planet.behaviors = {"PLANET"};
    d.planets.push_back(planet);
    d.constants = {{"MaximumFleetMovementDistance", "10", whole(10)}, {"Fiscal_Cycle_Time_In_Secs", "45", whole(45)}};
    dg::Campaign campaign; campaign.name = "Multiplayer"; campaign.locations = {"P"}; campaign.planets = {"P"};
    campaign.supports_custom_settings = true; campaign.is_multiplayer = true;
    for (const auto& name : factions) {
        campaign.starting_credits.push_back({name, 9000}); campaign.starting_tech_levels.push_back({name, 2});
        campaign.max_tech_levels.push_back({name, 5}); campaign.home_locations.push_back({name, "P"});
    }
    d.campaigns.push_back(campaign);
    return d;
}
eawr::units::GalacticTypeTable make_types() {
    eawr::units::GalacticType ship; ship.name = "Ship"; ship.object_class = "SpaceUnit"; ship.can_hyperspace_without_ability = true;
    auto hero = ship; hero.name = "Hero";
    eawr::units::GalacticTypeTable table; table.types = {ship, hero};
    return table;
}
// What one peer owns: its installed content, as the loader would hand it over. Pointers into itself: never moved.
struct Local {
    dg::GalacticData data;
    eawr::units::GalacticTypeTable types;
    cp::MpLocalContent content;
    explicit Local(const Names& names = {}) : data(make_data(names)), types(make_types()) {
        content.data = &data; content.types = &types;
        content.script_manifest_digest = std::string(64, 'a'); content.xml_digest = std::string(64, 'b');
        content.command_quota = static_cast<std::uint32_t>(net::max_batch_commands);
        content.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
        // Independent of the digests, so a single content difference moves exactly its own identity domain.
        content.ai_configuration = [](std::string_view faction, gc::PlayerId player, std::string_view) {
            std::string text = "ai:" + std::string(faction) + ":" + std::to_string(player);
            return eawr::core::Result<Bytes>::success(Bytes(text.begin(), text.end()));
        };
    }
    Local(const Local&) = delete;
    Local& operator=(const Local&) = delete;
};

gc::MpRosterPlayer mk_faction(gc::PlayerId id, const std::string& name) {
    gc::MpRosterPlayer row; row.player = id; row.faction_id = id; row.faction = name;
    return row;
}
gc::Seat mk_human(std::uint32_t seat, gc::PlayerId player, std::uint32_t side, std::uint32_t slot) {
    return {sim::SeatId{seat}, player, sim::SeatKind::human, slot, true, side};
}
gc::Seat mk_ai_seat(std::uint32_t seat, gc::PlayerId player) {
    return {sim::SeatId{seat}, player, sim::SeatKind::ai, 0, true, 0};
}
// Two human sides against each other, plus data-driven AI factions and the neutral player.
gc::MpRoster two_sides(const Names& names = {}) {
    gc::MpRoster roster; roster.campaign = "Multiplayer"; roster.difficulty = "Normal_Default";
    roster.players = {mk_faction(1, names.a), mk_faction(2, names.b), mk_faction(3, names.ai)};
    roster.seats = {mk_human(1, 1, 1, 0), mk_human(2, 2, 2, 1), mk_ai_seat(101, 3)};
    gc::PlayerId next = 4;
    if (!names.extra_ai.empty()) { roster.players.push_back(mk_faction(next, names.extra_ai)); roster.seats.push_back(mk_ai_seat(102, next)); ++next; }
    roster.players.push_back(mk_faction(next, names.neutral));
    roster.teams = {{1, sim::SeatId{1}, false}, {2, sim::SeatId{2}, false}};
    return roster;
}
// Eight humans share one faction against two AI factions.
gc::MpRoster coop_eight() {
    gc::MpRoster roster; roster.campaign = "Multiplayer"; roster.difficulty = "Normal_Default";
    roster.players = {mk_faction(1, "REBEL"), mk_faction(2, "EMPIRE"), mk_faction(3, "UNDERWORLD"), mk_faction(4, "NEUTRAL")};
    for (std::uint32_t seat = 1; seat <= 8; ++seat) roster.seats.push_back(mk_human(seat, 1, 1, seat - 1));
    roster.seats.push_back(mk_ai_seat(101, 2)); roster.seats.push_back(mk_ai_seat(102, 3));
    roster.teams = {{1, sim::SeatId{1}, false}};
    return roster;
}

constexpr std::uint64_t match_seed = 4242;
cp::MpSessionTerms default_terms() {
    cp::MpSessionTerms terms;
    terms.seed = match_seed;
    terms.command_quota = static_cast<std::uint32_t>(net::max_batch_commands);
    terms.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
    return terms;
}

// The setup proposal a host would publish: the sealed roster, the host's own derived identity, one peer per human seat.
net::SetupOffer make_offer(Local& host, gc::MpRoster draft, std::optional<Digest> forged_tick_zero = {}) {
    const auto terms = default_terms();
    auto sealed = require(cp::seal_mp_roster(host.content, std::move(draft), terms));
    if (forged_tick_zero) sealed.tick_zero = *forged_tick_zero;
    auto agreement = require(cp::derive_mp_agreement(host.content, sealed, terms));
    net::SetupOffer proposal;
    proposal.domain = std::string(net::galactic_setup_domain);
    proposal.seed = match_seed;
    net::GalacticSetupPayload payload; payload.roster = sealed;
    proposal.setup = require(net::encode_galactic_setup(payload));
    net::PeerIndex peer = 0;
    for (const auto& seat : sealed.seats) if (seat.kind == sim::SeatKind::human) proposal.seats.push_back({peer++, seat.seat_id, seat.player});
    proposal.identity = {std::string(eawr::core::build_identity()), agreement.content};
    if (forged_tick_zero) {
        for (auto& entry : proposal.identity.content.domains)
            if (entry.name == net::galactic_tick_zero_domain) entry.digest = *forged_tick_zero;
        proposal.identity.content = sim::compute_match_identity(std::move(proposal.identity.content));
    }
    return proposal;
}

// The tick-zero state of a derived campaign. Stepping belongs to the galactic adapter (GCMP #3769); setup only needs
// the barrier hash, which is the session's own state digest.
class TickZeroAdapter final : public net::SessionAdapter {
public:
    explicit TickZeroAdapter(std::shared_ptr<cp::GameSession> session) : session_(std::move(session)) {}
    std::uint64_t completed_tick() const override { return 0; }
    std::string state_hash() const override { return session_->state_sha256(); }
    Bytes canonical_state() const override { const auto hash = state_hash(); return Bytes(hash.begin(), hash.end()); }
    eawr::core::Result<void> validate(const net::SeatInput&) const override { return unavailable<void>(); }
    eawr::core::Result<void> install(std::span<const net::SeatInput>) override { return unavailable<void>(); }
    eawr::core::Result<Bytes> canonical_input() const override { return eawr::core::Result<Bytes>::success(Bytes{}); }
    eawr::core::Result<net::CompletedTick> step() override { return unavailable<net::CompletedTick>(); }
    bool is_departure(const net::SeatInput&) const override { return false; }
private:
    template<class T> static eawr::core::Result<T> unavailable() {
        eawr::core::Diagnostic diagnostic; diagnostic.code = "EAWR-TEST-0001";
        diagnostic.message = "the galactic lockstep adapter is GCMP #3769";
        return eawr::core::Result<T>::failure(std::move(diagnostic));
    }
    std::shared_ptr<cp::GameSession> session_;
};

struct Holder {
    std::shared_ptr<cp::MpAgreement> agreement;
    int builds{};
};

// A lying host adjusts what its own identify provider reports so that it accepts its own proposal.
using HostAdjust = std::function<void(sim::MatchAgreement&, const net::SetupOffer&)>;

class Group {
public:
    Group(std::vector<std::shared_ptr<Local>> members, const net::SetupOffer& proposal, net::SetupOptions options = {},
        HostAdjust host_adjust = {})
        : network(static_cast<std::uint32_t>(members.size())), members_(std::move(members)) {
        for (net::PeerIndex peer = 0; peer < members_.size(); ++peer) {
            transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
            net::DesyncOptions monitor_options; monitor_options.exchange_every = 1000;
            monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), monitor_options));
            holders.push_back(std::make_shared<Holder>());
            auto provider = providers(*monitors.back(), executor);
            provider.identify = [local = members_[peer], holder = holders.back(), adjust = peer == 0 ? host_adjust : HostAdjust{}](
                const net::SetupOffer& selected) -> eawr::core::Result<sim::MatchAgreement> {
                auto payload = net::decode_galactic_setup(selected.setup);
                if (!payload) return eawr::core::Result<sim::MatchAgreement>::failure(payload.error());
                cp::MpSessionTerms session_terms;
                session_terms.seed = selected.seed;
                session_terms.command_quota = payload.value().command_quota; session_terms.byte_quota = payload.value().byte_quota;
                auto derived = cp::derive_mp_agreement(local->content, payload.value().roster, session_terms);
                if (!derived) return eawr::core::Result<sim::MatchAgreement>::failure(derived.error());
                holder->agreement = std::make_shared<cp::MpAgreement>(std::move(derived).value());
                sim::MatchAgreement result{std::string(eawr::core::build_identity()), holder->agreement->content};
                if (adjust) adjust(result, selected);
                return eawr::core::Result<sim::MatchAgreement>::success(std::move(result));
            };
            provider.build = [holder = holders.back()](const net::SetupOffer&)
                -> eawr::core::Result<std::unique_ptr<net::SessionAdapter>> {
                ++holder->builds;
                return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(
                    std::make_unique<TickZeroAdapter>(holder->agreement->session));
            };
            sessions.push_back(peer == 0 ? require(net::SetupSession::host(*transports.back(), proposal, std::move(provider), options))
                                         : require(net::SetupSession::join(*transports.back(), std::move(provider), options)));
        }
    }
    void pump(bool expect_ok) {
        ++network.round;
        for (auto& session : sessions) {
            auto pumped = session.pump(network.round);
            if (expect_ok) require(std::move(pumped));
        }
    }
    bool every(net::SetupPhase phase) const {
        return std::all_of(sessions.begin(), sessions.end(), [phase](const auto& session) { return session.phase() == phase; });
    }
    void ready() {
        for (unsigned i = 0; i < 400; ++i) { pump(true); if (every(net::SetupPhase::ready)) return; }
        throw std::runtime_error("galactic setup never reached ready");
    }
    void refused() {
        for (unsigned i = 0; i < 400; ++i) {
            pump(false);
            if (std::all_of(sessions.begin(), sessions.end(), [](const auto& session) { return session.refusal().has_value(); })) return;
        }
        throw std::runtime_error("refusal did not reach every peer");
    }
    // Identity domains that differ between the host's and a peer's local derivation.
    std::set<std::string> differing(std::size_t peer) const {
        std::set<std::string> out;
        const auto& left = holders[0]->agreement->content.domains;
        const auto& right = holders[peer]->agreement->content.domains;
        for (const auto& domain : left) {
            const auto found = std::find_if(right.begin(), right.end(), [&](const auto& row) { return row.name == domain.name; });
            if (found == right.end() || found->digest != domain.digest) out.insert(domain.name);
        }
        return out;
    }
    eawr::testing::FakeNetwork network;
    sim::InlineExecutor executor;
    std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<std::shared_ptr<Holder>> holders;
    std::vector<net::SetupSession> sessions;
private:
    std::vector<std::shared_ptr<Local>> members_;
};

std::vector<std::shared_ptr<Local>> peers_of(std::size_t count, const Names& names = {}) {
    std::vector<std::shared_ptr<Local>> out;
    for (std::size_t i = 0; i < count; ++i) out.push_back(std::make_shared<Local>(names));
    return out;
}
std::string hex(std::span<const std::uint8_t> digest) {
    std::string out;
    for (const auto byte : digest) {
        out.push_back("0123456789abcdef"[byte >> 4U]);
        out.push_back("0123456789abcdef"[byte & 15U]);
    }
    return out;
}

void check_ready_group(Group& group, const net::SetupOffer& proposal, const std::string& label) {
    group.ready();
    std::vector<net::SetupResult> results;
    for (auto& session : group.sessions) results.push_back(require(session.take_result()));
    const auto claim = require(net::decode_galactic_setup(proposal.setup)).roster.tick_zero;
    for (std::size_t peer = 0; peer < results.size(); ++peer) {
        auto& result = results[peer];
        expect(result.adapter().completed_tick() == 0, label + ": the barrier never steps tick one");
        expect(result.adapter().state_hash() == hex(claim), label + ": every peer's own tick-zero digest equals the agreed one");
        expect(result.offer().seats == proposal.seats && result.offer().seed == proposal.seed, label + ": seat mapping and seed agreed");
        expect(group.holders[peer]->builds == 1, label + ": each peer built from its own derivation once");
        expect(group.holders[peer]->agreement->content == group.holders[0]->agreement->content, label + ": identical derived identities");
    }
}

void agreement_two_peers() {
    auto cohort = peers_of(2);
    const auto proposal = make_offer(*cohort[0], two_sides());
    Group group(cohort, proposal);
    check_ready_group(group, proposal, "two peers");
    const auto& domains = group.holders[1]->agreement->content.domains;
    for (const auto name : {"gc-campaign", "gc-content", "gc-map", "gc-story", "gc-ai-data", "gc-ai-controllers", "gc-resolver", "gc-rules", "gc-tick-zero"})
        expect(std::any_of(domains.begin(), domains.end(), [&](const auto& row) { return row.name == name; }), std::string("identity domain ") + name);
    expect(net::galactic_tick_zero_domain == cp::mp_domains::tick_zero, "net and campaign agree on the tick-zero domain name");
    const auto roster = require(net::decode_galactic_setup(proposal.setup)).roster;
    expect(roster.teams.size() == 2 && roster.seats.size() == 3, "two human sides plus an AI seat");
}
void agreement_eight_seats() {
    auto cohort = peers_of(8);
    const auto proposal = make_offer(*cohort[0], coop_eight());
    expect(proposal.seats.size() == 8, "eight human peers on one faction");
    Group group(cohort, proposal);
    check_ready_group(group, proposal, "eight seats");
}
void data_driven_factions() {
    const Names renamed{"CORPORATE_SECTOR", "ZANN_CONSORTIUM", "HUTT_CARTEL", "BLACK_SUN", "UNCLAIMED"};
    auto cohort = peers_of(2, renamed);
    const auto proposal = make_offer(*cohort[0], two_sides(renamed));
    Group group(cohort, proposal);
    check_ready_group(group, proposal, "renamed factions");
    const auto roster = require(net::decode_galactic_setup(proposal.setup)).roster;
    expect(roster.players.size() == 5 && roster.players[0].faction == "CORPORATE_SECTOR" && roster.players[3].faction == "BLACK_SUN"
        && !roster.players[2].ai_configuration.empty() && !roster.players[3].ai_configuration.empty()
        && roster.players[0].ai_configuration.empty(), "renamed and additional AI factions keep their own controller configuration");
    auto stock = peers_of(2);
    const auto stock_offer = make_offer(*stock[0], two_sides());
    expect(net::decode_galactic_setup(stock_offer.setup).value().roster.tick_zero != roster.tick_zero,
        "faction names and counts are part of the agreed tick-zero state");
}

// Each variant changes one thing about the second peer's installed content or build.
void content_refusals() {
    struct Variant {
        std::string name;
        std::function<void(Local&)> change;
        std::set<std::string> expected_within; // domains that must differ
        std::set<std::string> exactly;         // when nonempty, the complete set
    };
    const std::vector<Variant> variants{
        {"campaign xml", [](Local& l) { l.data.planets[0].planet_credit_value = whole(17); }, {"gc-content"}, {}},
        {"map", [](Local& l) { l.data.planets[0].galactic_position = dg::Position{whole(5), whole(0), whole(0)}; }, {"gc-map"}, {}},
        {"story scripts", [](Local& l) { l.content.script_manifest_digest = std::string(64, 'c'); }, {}, {"gc-story"}},
        {"story names", [](Local& l) { l.data.campaigns[0].rebel_story_name = "Other_Story"; }, {"gc-story"}, {}},
        {"ai data", [](Local& l) { l.content.xml_digest = std::string(64, 'd'); }, {}, {"gc-ai-data"}},
        {"resolver", [](Local& l) { l.data.constants.push_back({"AutoResolve_Test", "2", whole(2)}); }, {"gc-resolver"}, {}},
        {"ai controllers", [](Local& l) {
             l.content.ai_configuration = [](std::string_view, gc::PlayerId, std::string_view) {
                 return eawr::core::Result<Bytes>::success(Bytes{9, 9, 9});
             }; }, {"gc-ai-controllers", "gc-tick-zero"}, {}},
    };
    for (const auto& variant : variants) {
        auto cohort = peers_of(2);
        variant.change(*cohort[1]);
        const auto proposal = make_offer(*cohort[0], two_sides());
        Group group(cohort, proposal);
        group.refused();
        const auto& refusal = *group.sessions[1].refusal();
        expect(refusal.reason == net::SetupReason::identity && refusal.domain.starts_with("gc-"),
            variant.name + ": refused as an identity mismatch naming a campaign domain, got " + refusal.domain);
        expect(*group.sessions[0].refusal() == refusal, variant.name + ": the host retains the same refusal");
        expect(group.holders[1]->builds == 0 && group.holders[0]->builds == 1,
            variant.name + ": the mismatched peer never builds a session");
        const auto differing = group.differing(1);
        for (const auto& name : variant.expected_within)
            expect(differing.contains(name), variant.name + ": " + name + " must differ");
        if (!variant.exactly.empty()) expect(differing == variant.exactly, variant.name + ": exactly the named domains differ");
        expect(!differing.empty() && refusal.domain == *differing.begin(), variant.name + ": the refusal names the first differing domain");
        expect(!group.sessions[1].take_result() && !group.sessions[0].take_result(), variant.name + ": no result without agreement");
    }
}
// The proposal decoded fine but this peer's content cannot run it at all.
void incompatible_content() {
    {
        auto cohort = peers_of(2);
        cohort[1]->data.campaigns.clear();
        const auto proposal = make_offer(*cohort[0], two_sides());
        Group group(cohort, proposal); group.refused();
        expect(group.sessions[1].refusal()->reason == net::SetupReason::local_build
            && group.sessions[1].refusal()->message.find("not installed") != std::string::npos, "a missing campaign is refused");
    }
    {
        auto cohort = peers_of(2);
        cohort[1]->data.campaigns[0].is_multiplayer = false;
        const auto proposal = make_offer(*cohort[0], two_sides());
        Group group(cohort, proposal); group.refused();
        expect(group.sessions[1].refusal()->reason == net::SetupReason::local_build, "a single-player campaign is refused");
    }
    {
        auto cohort = peers_of(2);
        cohort[1]->data.factions[1].is_playable = false;
        const auto proposal = make_offer(*cohort[0], two_sides());
        Group group(cohort, proposal); group.refused();
        expect(group.sessions[1].refusal()->reason == net::SetupReason::local_build
            && group.holders[1]->builds == 0, "a human seat on a faction this content does not let humans play is refused");
    }
    {
        auto cohort = peers_of(2);
        cohort[1]->content.command_quota -= 1;
        const auto proposal = make_offer(*cohort[0], two_sides());
        Group group(cohort, proposal); group.refused();
        expect(group.sessions[1].refusal()->reason == net::SetupReason::local_build
            && group.sessions[1].refusal()->message.find("quotas") != std::string::npos, "different batch quotas are refused");
    }
}
// The peer derives with a different local seed; the host's tick-zero claim cannot replace its result.
void seed_refusal() {
    auto cohort = peers_of(2);
    const auto proposal = make_offer(*cohort[0], two_sides());
    const auto peer = cohort[1];
    peer->content.planner = [local = peer.get()](const cp::CampaignSetupOptions& options,
        std::string_view campaign, std::string_view) {
        auto different = options;
        ++different.seed;
        return cp::plan_campaign_setup(local->data, local->types, campaign, different);
    };
    Group group(cohort, proposal);
    group.refused();
    const auto& refusal = *group.sessions[1].refusal();
    expect(refusal.reason == net::SetupReason::identity && refusal.domain == "gc-tick-zero",
        "a differing local seed is refused with gc-tick-zero as the first differing domain");
    expect(group.differing(1) == std::set<std::string>{"gc-tick-zero"}, "only tick zero differs for a seed change");
    expect(*group.sessions[0].refusal() == refusal && group.holders[1]->builds == 0,
        "seed refusal reaches the host before the peer builds an adapter");
    expect(!group.sessions[0].take_result() && !group.sessions[1].take_result(), "a seed mismatch cannot release shared steps");
}
// A host that claims a tick-zero digest its content does not produce never replaces a peer's own computation.
void forged_host_digest() {
    Digest forged{};
    forged.fill(0x5a);
    auto cohort = peers_of(2);
    const auto proposal = make_offer(*cohort[0], two_sides(), forged);
    Group group(cohort, proposal, {}, [forged](sim::MatchAgreement& reported, const net::SetupOffer&) {
        for (auto& entry : reported.content.domains)
            if (entry.name == net::galactic_tick_zero_domain) entry.digest = forged;
        reported.content = sim::compute_match_identity(std::move(reported.content));
    });
    group.refused();
    const auto& refusal = *group.sessions[1].refusal();
    expect(refusal.reason == net::SetupReason::identity && refusal.domain == "gc-tick-zero", "forged tick-zero claim is refused by name");
    expect(group.holders[1]->builds == 0, "the peer never builds from a forged claim");
    expect(group.holders[1]->agreement->tick_zero != forged, "the peer kept its own digest");
    // The forged claim cannot ride an identity that disagrees with it either.
    auto honest = peers_of(2);
    auto mismatched = make_offer(*honest[0], two_sides());
    auto roster = require(net::decode_galactic_setup(mismatched.setup));
    roster.roster.tick_zero = forged;
    mismatched.setup = require(net::encode_galactic_setup(roster));
    expect(!net::encode_setup_offer(mismatched), "payload and identity must carry the same tick-zero claim");
}
// A roster whose AI configuration the peer would not use is refused even when the rest of the proposal is honest.
void forged_controller_claim() {
    auto cohort = peers_of(2);
    auto proposal = make_offer(*cohort[0], two_sides());
    auto payload = require(net::decode_galactic_setup(proposal.setup));
    payload.roster.players[2].ai_configuration = Bytes{1, 2, 3};
    proposal.setup = require(net::encode_galactic_setup(payload));
    // The lying host keeps its honest digests and reports them for its own check, as a forged proposal would.
    Group group(cohort, proposal, {}, [](sim::MatchAgreement& reported, const net::SetupOffer& selected) {
        reported = selected.identity;
    });
    group.refused();
    expect(group.sessions[1].refusal().has_value() && group.sessions[1].refusal()->reason == net::SetupReason::identity
        && group.sessions[1].refusal()->domain == "gc-ai-controllers", "a controller claim that differs from local derivation is refused");
    expect(group.holders[1]->builds == 0, "the peer never builds from a forged controller claim");
}

void payload_codec() {
    auto cohort = peers_of(1);
    const auto proposal = make_offer(*cohort[0], two_sides());
    const auto payload = require(net::decode_galactic_setup(proposal.setup));
    expect(require(net::encode_galactic_setup(payload)) == proposal.setup, "payload is canonical");
    for (std::size_t size = 0; size < proposal.setup.size(); ++size)
        expect(!net::decode_galactic_setup(std::span(proposal.setup).first(size)), "every truncated payload is refused");
    auto trailing = proposal.setup; trailing.push_back(0);
    expect(!net::decode_galactic_setup(trailing), "trailing byte refused");
    auto wrong_magic = proposal.setup; wrong_magic[0] ^= 1;
    expect(!net::decode_galactic_setup(wrong_magic), "wrong magic refused");
    auto wrong_version = proposal.setup; wrong_version[4] = 2;
    expect(!net::decode_galactic_setup(wrong_version), "unknown payload version refused");
    for (std::size_t index = 8; index < proposal.setup.size(); ++index) {
        auto flipped = proposal.setup; flipped[index] ^= 0x80;
        const auto decoded = net::decode_galactic_setup(flipped);
        expect(!decoded || require(net::encode_galactic_setup(decoded.value())) == flipped, "any accepted mutation re-encodes to itself");
    }
    auto bad_quota = payload; bad_quota.command_quota += 1;
    expect(!net::encode_galactic_setup(bad_quota), "quotas other than the session limits are refused");
    expect(require(net::decode_setup_offer(require(net::encode_setup_offer(proposal)))).setup == proposal.setup, "offer round trip keeps the payload");
    const auto encoded = require(net::encode_setup_offer(proposal));
    for (std::size_t size = 0; size < encoded.size(); size += 17)
        expect(!net::decode_setup_offer(std::span(encoded).first(size)), "every truncated offer is refused");
}

void roster_refusals() {
    auto cohort = peers_of(1);
    const auto base = make_offer(*cohort[0], two_sides());
    const auto accepted = [&](const std::function<void(net::GalacticSetupPayload&)>& mutate) {
        auto payload = require(net::decode_galactic_setup(base.setup));
        mutate(payload);
        return static_cast<bool>(net::encode_galactic_setup(payload));
    };
    expect(accepted([](auto&) {}), "the unchanged payload is accepted");
    expect(!accepted([](auto& p) { p.roster.seats.push_back(mk_human(103, 3, 3, 2)); }), "a third human side is refused");
    expect(!accepted([](auto& p) { p.roster.seats[1].human_side_id = 1; }), "one side may not hold two factions");
    expect(!accepted([](auto& p) { p.roster.seats[2].seat_id = p.roster.seats[0].seat_id; }), "duplicate seat IDs are refused");
    expect(!accepted([](auto& p) { std::swap(p.roster.seats[0], p.roster.seats[1]); }), "seat IDs must ascend");
    expect(!accepted([](auto& p) { p.roster.seats[0].player = 99; }), "a seat on an unlisted faction is refused");
    expect(!accepted([](auto& p) { p.roster.seats[0].human_side_id = 0; }), "a human seat needs a side");
    expect(!accepted([](auto& p) { p.roster.seats[2].human_side_id = 1; }), "an AI seat has no side");
    expect(!accepted([](auto& p) { p.roster.seats[0].kind = sim::SeatKind::ai; p.roster.seats[0].human_side_id = 0; }),
        "a roster needs two human seats");
    expect(!accepted([](auto& p) { p.roster.teams[0].lead = sim::SeatId{2}; }), "a lead outside its side is refused");
    expect(!accepted([](auto& p) { p.roster.teams.pop_back(); }), "every side needs a lead entry");
    expect(!accepted([](auto& p) { p.roster.players[0].ai_configuration = Bytes{1}; }), "a human faction carries no AI configuration");
    expect(!accepted([](auto& p) { p.roster.players[2].ai_configuration.clear(); }), "an AI faction needs its controller configuration");
    expect(!accepted([](auto& p) { p.roster.players[2].ai_configuration.assign(5000, 1); }), "an AI configuration is bounded");
    expect(!accepted([](auto& p) { p.roster.players[2].dropout = gc::FactionControl::human; }), "a dropout is AI or passive");
    expect(!accepted([](auto& p) { p.roster.players[2].faction = p.roster.players[1].faction; }), "faction names are distinct");
    expect(!accepted([](auto& p) { p.roster.players[2].faction_id = p.roster.players[1].faction_id; }), "faction IDs are distinct");
    expect(!accepted([](auto& p) { p.roster.campaign.clear(); }), "a campaign is named");
    expect(!accepted([](auto& p) { p.roster.credits = 5; }), "custom values need the custom flag");
    expect(accepted([](auto& p) { p.roster.custom = true; p.roster.credits = 5000; p.roster.starting_tech = 1; p.roster.maximum_tech = 4; }),
        "custom options travel in the roster");
    // Nine humans: eight seats is the ceiling.
    auto crowd = coop_eight();
    crowd.seats.insert(crowd.seats.begin() + 8, mk_human(9, 1, 1, 8));
    for (auto& player : crowd.players) if (player.player != 1) player.ai_configuration = Bytes{1};
    expect(!gc::validate_mp_roster(crowd), "nine human seats are refused");
}

void offer_refusals() {
    auto cohort = peers_of(1);
    const auto base = make_offer(*cohort[0], two_sides());
    expect(static_cast<bool>(net::encode_setup_offer(base)), "the unchanged campaign offer is accepted");
    auto proposal = base; proposal.seats[1].seat = sim::SeatId{101};
    expect(!net::encode_setup_offer(proposal), "a peer cannot take an AI seat");
    proposal = base; proposal.seats[1].player = 1;
    expect(!net::encode_setup_offer(proposal), "a peer's faction must be the seat's faction");
    proposal = base; proposal.seats[1].seat = proposal.seats[0].seat;
    expect(!net::encode_setup_offer(proposal), "two peers cannot share a seat");
    proposal = base; proposal.seats[1].seat = sim::SeatId{7};
    expect(!net::encode_setup_offer(proposal), "an unlisted seat is unauthorized");
    proposal = base; proposal.seats[1].peer = 5;
    expect(!net::encode_setup_offer(proposal), "peer indices are canonical");
    proposal = base; proposal.seats.pop_back();
    expect(!net::encode_setup_offer(proposal), "a roster with fewer than two peers is refused");
    {
        auto three = peers_of(1);
        auto roster = coop_eight();
        auto crowded = make_offer(*three[0], roster);
        crowded.seats.pop_back();
        expect(!net::encode_setup_offer(crowded), "every human seat needs its peer");
        crowded = make_offer(*three[0], roster);
        crowded.seats.push_back({8, sim::SeatId{101}, 2});
        expect(!net::encode_setup_offer(crowded), "a ninth peer is refused");
    }
    proposal = base; proposal.identity.content.domains.erase(std::remove_if(proposal.identity.content.domains.begin(),
        proposal.identity.content.domains.end(), [](const auto& row) { return row.name == "gc-tick-zero"; }), proposal.identity.content.domains.end());
    proposal.identity.content = sim::compute_match_identity(std::move(proposal.identity.content));
    expect(!net::encode_setup_offer(proposal), "the identity must carry the tick-zero domain");
    proposal = base; proposal.domain = "campaign-v1";
    expect(!net::encode_setup_offer(proposal), "an unknown domain is refused");
    proposal = base; proposal.domain = std::string(net::tactical_setup_domain);
    expect(!net::encode_setup_offer(proposal), "campaign bytes are not a skirmish setup");
    proposal = setup_test::offer(); proposal.domain = std::string(net::galactic_setup_domain);
    expect(!net::encode_setup_offer(proposal), "skirmish bytes are not a campaign setup");
}

void readiness() {
    {
        auto cohort = peers_of(2);
        const auto proposal = make_offer(*cohort[0], two_sides());
        Group group(cohort, proposal);
        expect(!group.sessions[0].cancel(), "cancel returns the typed refusal");
        group.refused();
        expect(group.sessions[1].refusal()->reason == net::SetupReason::cancelled, "cancellation reaches the peer");
    }
    {
        auto cohort = peers_of(2);
        const auto proposal = make_offer(*cohort[0], two_sides());
        net::SetupOptions options; options.timeout_ms = 10;
        Group group(cohort, proposal, options);
        for (unsigned i = 0; i < 12; ++i) {
            ++group.network.round;
            static_cast<void>(group.sessions[0].pump(group.network.round));
        }
        expect(group.sessions[0].refusal() && group.sessions[0].refusal()->reason == net::SetupReason::timeout,
            "a peer that never answers times the setup out");
    }
    {
        auto cohort = peers_of(2);
        const auto proposal = make_offer(*cohort[0], two_sides());
        Group group(cohort, proposal);
        group.ready();
        expect(!group.sessions[0].cancel(), "a ready setup can still be cancelled before the hand-off");
        group.refused();
        expect(!group.sessions[1].take_result(), "no result after cancel");
    }
}

// Full revision-3 offer captured once from the encoder shared with origin/main cad8df340.
// The fixed synthetic build label keeps this literal stable across builds; it includes the entire SKSU body.
void skirmish_layout_pinned() {
    auto selected = offer();
    selected.identity.build = "setup-wire-pin-v1";
    constexpr std::string_view expected_hex =
        "2803000000e502000000000000030000000000000040420f000000000014000000746163746963616c2d736b69726d697368"
        "2d76316904000045415752504c590004003103010000000100000018000000010000001e000000e502000000000000000000"
        "00000000000300000000000000030000000000000000000000000000008e4b73d0a9254545a8a85561682283164a356574a2"
        "66cf984fb9215cbeafbf7c020000000200fe001600646174612f6172742f6d6170732f746573742e74656440006161616161"
        "6161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161"
        "616161616161616161100000000000008813000000010000000000000005000000000000007b000000000000000300000000"
        "000000010000000000000000000002000000000800636f6e7175657374080073746172626173650100000003000000010000"
        "000100000002000000010000000700666c6565742d61020000000100000004000000010000000700666c6565742d62030000"
        "000000000001000000010000000800666c6565742d61690700bf01656177722d6d617463682d6964656e7469747901000000"
        "080000000700000061692d646174610100000032e83e92d45d71f69dcf9d214688f0375542108631b45d344e5df2eb91c115"
        "660a0000006d61702d64696765737401000000ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668"
        "eb080000006d61702d7061746801000000eccb79d65f536e6d36d0ae3dd22fc790c9f40127ecbc5c2da44fe471f95ceb9b0b"
        "0000006e756d657269632d61626901000000992a93455c71fedd36ac9bbc439952c041cf61445958472af479269b8d873513"
        "0500000072756c657301000000b317bcc0c540953efb46f561c58953f0ca5c1bbb3c3a2ea2ae892d429255f07e0700000073"
        "616e64626f7801000000b7ad567477c83756aab9a542b2be04f77dbae25115d85f22070d74d8cc4779dc0f00000073637269"
        "70742d6d616e6966657374010000008c5967fd8486f34493710cc39b240aad46536cf4ee421ffd0479e6542db03e36050000"
        "00756e697473010000008e4b73d0a9254545a8a85561682283164a356574a266cf984fb9215cbeafbf7cd5f02573ead8e360"
        "216423e48136efe3fc69286d99d5ade4927d428afa50b8cb0100000001000000000000000000000001000000000000000200"
        "0000020000000100000000000000010000000000000003000000030000000200000000000000010000000000000001000000"
        "0000000001000000000000000100000000000000000000ceffffffff00000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000100000000020000000000000001000000000000000200000000000000"
        "0000003200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0001000000000300000000000000010000000000000003000000000000000000003200000000000000640000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000001000000000200000000000000070000000100"
        "0000010000000b000000020000001100000073657475702d776972652d70696e2d76319f010000656177722d6d617463682d"
        "6964656e7469747901000000080000000700000061692d646174610100000032e83e92d45d71f69dcf9d214688f037554210"
        "8631b45d344e5df2eb91c115660a0000006d61702d64696765737401000000ffe054fe7ae0cb6dc65c3af9b61d5209f43985"
        "1db43d0ba5997337df154668eb080000006d61702d7061746801000000eccb79d65f536e6d36d0ae3dd22fc790c9f40127ec"
        "bc5c2da44fe471f95ceb9b0b0000006e756d657269632d61626901000000992a93455c71fedd36ac9bbc439952c041cf6144"
        "5958472af479269b8d8735130500000072756c657301000000b317bcc0c540953efb46f561c58953f0ca5c1bbb3c3a2ea2ae"
        "892d429255f07e0700000073616e64626f7801000000b7ad567477c83756aab9a542b2be04f77dbae25115d85f22070d74d8"
        "cc4779dc0f0000007363726970742d6d616e6966657374010000008c5967fd8486f34493710cc39b240aad46536cf4ee421f"
        "fd0479e6542db03e3605000000756e697473010000008e4b73d0a9254545a8a85561682283164a356574a266cf984fb9215c"
        "beafbf7cd5f02573ead8e360216423e48136efe3fc69286d99d5ade4927d428afa50b8cb0030750000000000003075000000"
        "000000e09304000000000000000004000000003075000000000000a4865f45b2d759fefa3294b70c09d2e5095165b00125bc"
        "bc117e02e6766fb89c";
    expect(hex(require(net::encode_setup_offer(selected))) == expected_hex,
        "skirmish offer bytes, including the SKSU body, keep the revision 3 encoding");
    expect(net::tactical_setup_domain == "tactical-skirmish-v1" && net::setup_offer_kind == 40 && net::setup_accept_kind == 41
        && net::setup_refuse_kind == 42 && net::setup_start_kind == 43 && net::setup_preview_kind == 44,
        "skirmish domain id and message kinds are unchanged");
    expect(net::galactic_setup_domain != net::tactical_setup_domain, "the campaign domain is a new domain id");
}

} // namespace

int main() {
    try {
        skirmish_layout_pinned(); payload_codec(); roster_refusals(); offer_refusals();
        agreement_two_peers(); agreement_eight_seats(); data_driven_factions();
        content_refusals(); incompatible_content(); seed_refusal(); forged_host_digest(); forged_controller_claim(); readiness();
        std::cout << "galactic setup contracts passed: payload, roster rules, two peers, eight seats, refusals, forged digests\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

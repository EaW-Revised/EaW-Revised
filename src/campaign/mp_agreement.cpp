#include "eawr/campaign/mp_agreement.hpp"

#include "eawr/campaign/live_setup.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/numeric/backend.hpp"
#include "eawr/sim/math/math.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace eawr::campaign {
namespace gc = sim::galactic;
namespace {
using Digest = std::array<std::uint8_t, 32>;
// Owner question #4358 D2: run non-human faction AI on every peer; keep the default reversible here.
constexpr auto default_nonhuman_controller = gc::FactionControl::ai;

template<class T> core::Result<T> refuse(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(gc::diagnostic_codes::invalid_setup);
    diagnostic.message = "multiplayer campaign: " + std::move(message);
    return core::Result<T>::failure(std::move(diagnostic));
}
class Writer {
public:
    explicit Writer(std::string_view tag) { text(tag); }
    void integer(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void text(std::string_view value) { integer(value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); }
    void raw(std::span<const std::uint8_t> value) { integer(value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); }
    [[nodiscard]] Digest digest() const { return core::sha256(bytes); }
    std::vector<std::uint8_t> bytes;
};
std::string upper(std::string_view value) {
    std::string out(value);
    for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
sim::IdentityDomain domain(std::string_view name, const Digest& digest) {
    return sim::IdentityDomain{std::string(name), 1, digest};
}
core::Result<Digest> digest_from_hex(const std::string& hex) {
    Digest out{};
    if (hex.size() != 64) return refuse<Digest>("session state digest is not 64 hex digits");
    const auto nibble = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::uint8_t>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    return core::Result<Digest>::success(out);
}

core::Result<std::vector<std::uint8_t>> default_ai_configuration(const MpLocalContent& local,
    std::string_view faction, gc::PlayerId player, std::string_view difficulty) {
    Writer out("eawr-gc-ai-controller-v1");
    out.text(upper(faction)); out.integer(player); out.text(difficulty);
    out.text(local.xml_digest); out.text(local.script_manifest_digest);
    return core::Result<std::vector<std::uint8_t>>::success(std::move(out.bytes));
}
core::Result<std::vector<std::uint8_t>> ai_configuration_for(const MpLocalContent& local,
    std::string_view faction, gc::PlayerId player, std::string_view difficulty) {
    if (local.ai_configuration) return local.ai_configuration(faction, player, difficulty);
    return default_ai_configuration(local, faction, player, difficulty);
}

struct Derived {
    CampaignSetupOptions options;
    std::shared_ptr<const CampaignSetupPlan> plan;
    std::shared_ptr<GameSession> session;
    Digest tick_zero{};
    std::vector<std::vector<std::uint8_t>> ai_configuration;
};

// Controller kind a faction has at tick zero: the roster's human sides hold their factions, everyone else is AI.
core::Result<Derived> derive_local(const MpLocalContent& local, const gc::MpRoster& roster, const MpSessionTerms& terms) {
    using Result = core::Result<Derived>;
    if (!local.data || !local.types) return refuse<Derived>("local content is not loaded");
    if (local.command_quota == 0 || local.byte_quota == 0
        || terms.command_quota != local.command_quota || terms.byte_quota != local.byte_quota)
        return refuse<Derived>("batch quotas differ from this build's session limits");
    const auto& data = *local.data;
    const auto* campaign = data.campaign(roster.campaign);
    if (!campaign) return refuse<Derived>("campaign " + roster.campaign + " is not installed");
    if (!campaign->is_multiplayer.value_or(false)) return refuse<Derived>("campaign " + roster.campaign + " is not a multiplayer campaign");
    Derived out;
    auto& options = out.options;
    options.seed = terms.seed;
    options.mode = CampaignSetupMode::multiplayer;
    if (roster.custom) options.custom = CampaignCustomOptions{roster.credits, roster.starting_tech, roster.maximum_tech, roster.allow_heroes};
    for (const auto& player : roster.players) options.players.push_back({{player.player, player.faction_id, player.flags}, player.faction, player.active});
    options.seats = roster.seats;
    std::vector<std::string> names;
    for (const auto& type : local.types->types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    gc::TypeId next_type{};
    for (const auto& name : names) options.types.push_back({++next_type, name});
    std::set<gc::PlayerId> human_players, active_humans;
    for (const auto& seat : roster.seats) if (seat.kind == sim::SeatKind::human) {
        human_players.insert(seat.player);
        if (seat.active) active_humans.insert(seat.player);
    }
    gc::SeatControlSetup control;
    control.teams = roster.teams;
    for (const auto& player : roster.players) {
        gc::FactionController controller;
        controller.player = player.player;
        controller.dropout = player.dropout;
        const bool human = human_players.contains(player.player);
        controller.kind = human ? (active_humans.contains(player.player) ? gc::FactionControl::human : player.dropout)
                                : default_nonhuman_controller;
        if (!human) {
            auto configuration = ai_configuration_for(local, player.faction, player.player, roster.difficulty);
            if (!configuration) return Result::failure(configuration.error());
            controller.configuration = configuration.value();
        }
        out.ai_configuration.push_back(controller.configuration);
        control.controllers.push_back(std::move(controller));
    }
    options.seat_control = std::move(control);
    if (auto playable = validate_live_campaign_seats(data, options.players, options.seats); !playable)
        return Result::failure(playable.error());
    auto planned = local.planner ? local.planner(options, roster.campaign, roster.difficulty)
                                 : plan_campaign_setup(data, *local.types, roster.campaign, options);
    if (!planned) return Result::failure(planned.error());
    out.plan = std::make_shared<const CampaignSetupPlan>(std::move(planned).value());
    std::string hex;
    if (local.services) {
        auto services = local.services(*out.plan);
        if (!services) return Result::failure(services.error());
        // The root owns the session and the setup context; the shared pointer keeps the root alive with it.
        auto root = CampaignRoot::create(CampaignSetupPlan(*out.plan), services.value());
        if (!root) return Result::failure(root.error());
        auto owner = std::make_shared<CampaignRoot>(std::move(root).value());
        if (!owner->game()) return refuse<Derived>("campaign root has no session");
        hex = owner->game()->state_sha256();
        out.session = std::shared_ptr<GameSession>(owner, owner->game());
    } else {
        auto created = GameSession::create(out.plan->galaxy());
        if (!created) return Result::failure(created.error());
        out.session = std::make_shared<GameSession>(std::move(created).value());
        hex = out.session->state_sha256();
    }
    auto zero = digest_from_hex(hex);
    if (!zero) return Result::failure(zero.error());
    out.tick_zero = zero.value();
    return Result::success(std::move(out));
}

// The galaxy layout as the map identity: planet positions and the route graph, in setup order.
Digest map_digest(const gc::GalacticSetup& setup) {
    Writer out("eawr-gc-map-v1");
    out.integer(setup.planets.size());
    for (const auto& planet : setup.planets) {
        out.integer(planet.id);
        out.integer(static_cast<std::uint64_t>(planet.position.x.raw())); out.integer(static_cast<std::uint64_t>(planet.position.y.raw()));
        out.integer(static_cast<std::uint64_t>(planet.position.z.raw()));
        out.integer(planet.owner);
    }
    out.integer(setup.routes.size());
    for (const auto& route : setup.routes) {
        out.integer(route.id); out.integer(route.from); out.integer(route.to);
        out.integer(static_cast<std::uint64_t>(route.hs_speed_factor.raw()));
    }
    return out.digest();
}
Digest story_digest(const MpLocalContent& local, const data::galactic::Campaign& campaign) {
    Writer out("eawr-gc-story-v1");
    out.text(campaign.rebel_story_name); out.text(campaign.empire_story_name); out.text(campaign.underworld_story_name);
    out.text(local.script_manifest_digest);
    return out.digest();
}
// The auto-resolve resolver is code; its data inputs are the campaign's permission flags, the excluded locations and
// every authored constant whose tag names the auto-resolve rules.
Digest resolver_digest(const MpLocalContent& local, const data::galactic::Campaign& campaign) {
    Writer out("eawr-gc-resolver-v1");
    out.integer(campaign.is_autoresolve_allowed.has_value() ? (*campaign.is_autoresolve_allowed ? 2 : 1) : 0);
    std::vector<std::string> excluded;
    for (const auto& location : campaign.autoresolve_exclusion_locations) excluded.push_back(upper(location));
    std::sort(excluded.begin(), excluded.end());
    out.integer(excluded.size());
    for (const auto& location : excluded) out.text(location);
    std::map<std::string, std::string> constants;
    for (const auto& constant : local.data->constants) {
        const auto tag = upper(constant.tag);
        if (tag.find("AUTORESOLVE") != std::string::npos || tag.find("AUTO_RESOLVE") != std::string::npos)
            constants.emplace(tag, constant.text);
    }
    out.integer(constants.size());
    for (const auto& [tag, text] : constants) { out.text(tag); out.text(text); }
    return out.digest();
}
Digest rules_digest(const MpLocalContent& local) {
    const auto abi = script::numeric::numeric_abi();
    Writer out("eawr-gc-rules-v1");
    out.integer(gc::galactic_rules_version); out.integer(gc::galactic_frames_per_second);
    out.integer(sim::math::Fixed::fractional_bits);
    out.text(abi.name); out.integer(abi.version);
    out.text(script::authoritative::sandbox_policy_identity);
    out.integer(local.command_quota); out.integer(local.byte_quota);
    return out.digest();
}
Digest content_digest(std::string_view tag, std::string_view text) {
    Writer out(tag);
    out.text(text);
    return out.digest();
}
} // namespace

core::Result<gc::MpRoster> seal_mp_roster(const MpLocalContent& local, gc::MpRoster draft, const MpSessionTerms& terms) {
    using Result = core::Result<gc::MpRoster>;
    if (auto valid = gc::validate_mp_roster(draft, false); !valid) return Result::failure(valid.error());
    std::set<gc::PlayerId> human_players;
    for (const auto& seat : draft.seats) if (seat.kind == sim::SeatKind::human) human_players.insert(seat.player);
    for (auto& player : draft.players) {
        player.ai_configuration.clear();
        if (human_players.contains(player.player)) continue;
        auto configuration = ai_configuration_for(local, player.faction, player.player, draft.difficulty);
        if (!configuration) return Result::failure(configuration.error());
        player.ai_configuration = std::move(configuration).value();
    }
    auto derived = derive_local(local, draft, terms);
    if (!derived) return Result::failure(derived.error());
    draft.tick_zero = derived.value().tick_zero;
    return Result::success(std::move(draft));
}

core::Result<MpAgreement> derive_mp_agreement(const MpLocalContent& local, const gc::MpRoster& roster, const MpSessionTerms& terms) {
    using Result = core::Result<MpAgreement>;
    if (auto valid = gc::validate_mp_roster(roster); !valid) return Result::failure(valid.error());
    auto derived = derive_local(local, roster, terms);
    if (!derived) return Result::failure(derived.error());
    auto& value = derived.value();
    const auto& setup = value.plan->galaxy();
    const auto* campaign = local.data->campaign(roster.campaign);
    // Claims never replace the local value. A differing claim enters beside it, so the digest differs.
    Writer controllers("eawr-gc-ai-controllers-v1");
    controllers.integer(roster.players.size());
    bool claims_match = true;
    for (std::size_t i = 0; i < roster.players.size(); ++i) {
        const auto& player = roster.players[i];
        controllers.integer(player.player); controllers.text(upper(player.faction));
        controllers.integer(player.dropout == gc::FactionControl::passive ? 1 : 0);
        controllers.raw(value.ai_configuration[i]);
        claims_match = claims_match && player.ai_configuration == value.ai_configuration[i];
    }
    if (!claims_match) {
        controllers.text("claim-differs");
        for (const auto& player : roster.players) controllers.raw(player.ai_configuration);
    }
    Writer zero("eawr-gc-tick-zero-v1");
    zero.raw(value.tick_zero);
    if (roster.tick_zero != value.tick_zero) { zero.text("claim-differs"); zero.raw(roster.tick_zero); }
    MpAgreement out;
    out.content.domains = {
        domain(mp_domains::campaign, setup.campaign_identity),
        domain(mp_domains::content, setup.content_identity),
        domain(mp_domains::map, map_digest(setup)),
        domain(mp_domains::story, story_digest(local, *campaign)),
        domain(mp_domains::ai_data, content_digest("eawr-gc-ai-data-v1", local.xml_digest)),
        domain(mp_domains::ai_controllers, controllers.digest()),
        domain(mp_domains::resolver, resolver_digest(local, *campaign)),
        domain(mp_domains::rules, rules_digest(local)),
        domain(mp_domains::tick_zero, roster.tick_zero == value.tick_zero ? value.tick_zero : zero.digest()),
    };
    out.content = sim::compute_match_identity(std::move(out.content));
    out.tick_zero = value.tick_zero;
    out.ai_configuration = std::move(value.ai_configuration);
    out.plan = std::move(value.plan);
    out.session = std::move(value.session);
    return Result::success(std::move(out));
}

} // namespace eawr::campaign

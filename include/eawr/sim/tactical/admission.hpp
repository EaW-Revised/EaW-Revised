#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/seat.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <map>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace eawr::sim::tactical {

enum class PayloadAuthority { human_wire, engine_only };

// Deliberately no default: a new payload must receive an explicit classification.
template <typename Payload> struct PayloadPolicy;
#define EAWR_PAYLOAD_POLICY(Payload, Authority) \
    template <> struct PayloadPolicy<Payload> { static constexpr auto authority = PayloadAuthority::Authority; }
EAWR_PAYLOAD_POLICY(StopPayload, human_wire);
EAWR_PAYLOAD_POLICY(MovePayload, human_wire);
EAWR_PAYLOAD_POLICY(AttackPayload, human_wire);
EAWR_PAYLOAD_POLICY(DamagePayload, engine_only);
EAWR_PAYLOAD_POLICY(FacePayload, human_wire);
EAWR_PAYLOAD_POLICY(AttackMovePayload, human_wire);
EAWR_PAYLOAD_POLICY(GuardPayload, human_wire);
EAWR_PAYLOAD_POLICY(AbilityPayload, human_wire);
EAWR_PAYLOAD_POLICY(BuyPayload, human_wire);
EAWR_PAYLOAD_POLICY(CancelPayload, human_wire);
EAWR_PAYLOAD_POLICY(ReinforcePayload, human_wire);
EAWR_PAYLOAD_POLICY(PadBuildPayload, human_wire);
EAWR_PAYLOAD_POLICY(CreditGrantPayload, engine_only);
EAWR_PAYLOAD_POLICY(QuitPayload, human_wire);
EAWR_PAYLOAD_POLICY(PadSellPayload, human_wire);
EAWR_PAYLOAD_POLICY(AreaAbilityPayload, human_wire);
EAWR_PAYLOAD_POLICY(ManualTargetPayload, human_wire);
EAWR_PAYLOAD_POLICY(RevealAllPayload, engine_only);
EAWR_PAYLOAD_POLICY(BeaconPayload, human_wire);
EAWR_PAYLOAD_POLICY(RepairHardpointPayload, human_wire);
EAWR_PAYLOAD_POLICY(AiReservationDebitPayload, engine_only);
EAWR_PAYLOAD_POLICY(SpecialFirePayload, human_wire);
EAWR_PAYLOAD_POLICY(RetreatPayload, human_wire);
EAWR_PAYLOAD_POLICY(RetreatCinematicPayload, human_wire);
EAWR_PAYLOAD_POLICY(CampaignCreditReleasePayload, engine_only);
#undef EAWR_PAYLOAD_POLICY

[[nodiscard]] constexpr PayloadAuthority payload_authority(const CommandPayload& payload) {
    return std::visit([](const auto& value) {
        return PayloadPolicy<std::remove_cvref_t<decltype(value)>>::authority;
    }, payload);
}

// Constructed by the active campaign owner, never decoded from a human packet.
// Recorded policy qualifies type/owner/restriction; the completed pool qualifies
// availability. Tactical admission still consumes the token atomically.
struct CampaignReserveAdmission {
    bool active{};
    PlayerId player{};
    std::span<const CampaignReserve> reserves;
    std::span<const std::uint64_t> available_tokens;
};

[[nodiscard]] constexpr bool human_wire_payload(const CommandPayload& payload,
    const CampaignReserveAdmission campaign = {}) {
    if (payload_authority(payload) != PayloadAuthority::human_wire) return false;
    // CP-1 includes engine entry routes embedded in otherwise ordinary payloads.
    if (const auto* buy = std::get_if<BuyPayload>(&payload)) return !buy->prepaid;
    if (const auto* cinematic = std::get_if<RetreatCinematicPayload>(&payload)) return cinematic->frame == 0;
    if (const auto* reinforce = std::get_if<ReinforcePayload>(&payload)) {
        if (reinforce->pool_token == 0) return true;
        if (!campaign.active || campaign.player == 0) return false;
        bool available{};
        for (const auto token : campaign.available_tokens) if (token == reinforce->pool_token) { available = true; break; }
        if (!available) return false;
        for (const auto& reserve : campaign.reserves)
            if (reserve.id == reinforce->pool_token && reserve.type == reinforce->type
                && reserve.owner == campaign.player && !reserve.restricted) return true;
        return false;
    }
    if (const auto* special = std::get_if<SpecialFirePayload>(&payload))
        return special->admission == SpecialFireAdmission::player;
    return true;
}

struct SeatBinding {
    SeatId seat{};
    PlayerId player{}; // Effective battle faction; persistent GC ownership is elsewhere.
    bool active{true};
    bool observer{};
};

struct SeatCommand {
    SeatCommandKey key{};
    PlayerId player{}; // Claimed faction is checked against the trusted binding.
    std::vector<EntityId> units;
    CommandPayload payload;
};

struct AdmittedSeatCommand {
    SeatCommandKey issuer{}; // Retained even when the child derives its faction sequence.
    PlayerCommand command;
};

[[nodiscard]] inline core::Result<AdmittedSeatCommand> admit_human_command(
    const SeatBinding& binding, const SeatCommand& intent, const CampaignReserveAdmission campaign = {}) {
    const auto refuse = [](const char* reason) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::invalid_command);
        diagnostic.message = std::string("human seat admission: ") + reason;
        return core::Result<AdmittedSeatCommand>::failure(std::move(diagnostic));
    };
    if (binding.seat.value == 0 || intent.key.seat != binding.seat) return refuse("unbound issuer");
    if (!binding.active) return refuse("inactive seat");
    if (binding.observer) return refuse("observer cannot issue gameplay");
    if (binding.player == 0 || intent.player != binding.player) return refuse("wrong faction");
    if (campaign.active && campaign.player != binding.player) return refuse("campaign binding is for another faction");
    if (!human_wire_payload(intent.payload, campaign)) return refuse("engine-only payload or entry route");
    return core::Result<AdmittedSeatCommand>::success({intent.key,
        {{intent.key.tick, binding.player, intent.key.sequence}, intent.units, intent.payload}});
}

// Intake cursors are per seat, never per faction. The sealed router subsequently assigns
// child faction sequences in (tick, seat, sequence) order; packet arrival cannot do that.
class SeatAdmission {
public:
    [[nodiscard]] core::Result<AdmittedSeatCommand> admit(const SeatBinding& binding, const SeatCommand& intent,
        const CampaignReserveAdmission campaign = {}) {
        auto admitted = admit_human_command(binding, intent, campaign);
        if (!admitted) return admitted;
        const auto cursor = std::pair{intent.key.tick, intent.key.sequence};
        const auto previous = cursors_.find(binding.seat);
        if (previous != cursors_.end() && !(previous->second < cursor)) {
            core::Diagnostic diagnostic;
            diagnostic.code = std::string(diagnostic_codes::order);
            diagnostic.message = "human seat admission: command does not follow this seat's cursor";
            return core::Result<AdmittedSeatCommand>::failure(std::move(diagnostic));
        }
        cursors_.insert_or_assign(binding.seat, cursor);
        return admitted;
    }
private:
    std::map<SeatId, std::pair<std::uint64_t, std::uint64_t>> cursors_;
};

} // namespace eawr::sim::tactical

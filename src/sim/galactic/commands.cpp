#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/replay.hpp"

#include "../replay_internal.hpp"
#include "encoding.hpp"

#include <span>
#include <set>
#include <type_traits>

namespace eawr::sim::galactic {

namespace {

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

CommandLayer layer_of(const CommandPayload& payload) noexcept {
    return std::visit(Overloaded{
                          [](const PauseGalaxy&) { return CommandLayer::session; },
                          [](const ResumeGalaxy&) { return CommandLayer::session; },
                          [](const BattleSummaryAcknowledge&) { return CommandLayer::session; },
                          [](const SeatLeave&) { return CommandLayer::session; },
                          [](const SeatRejoin&) { return CommandLayer::session; },
                          [](const AssignBattleTeamRole&) { return CommandLayer::session; },
                          [](const SaveRequest&) { return CommandLayer::session; },
                          [](const PostLoadFixup&) { return CommandLayer::session; },
                          [](const SetSpeed&) { return CommandLayer::session; },
                          [](const DebugSetPlanetOwner&) { return CommandLayer::session; },
                          [](const DebugSetFleetOrbit&) { return CommandLayer::session; },
                          [](const TacticalLayerCommand&) { return CommandLayer::tactical; },
                          [](const CreateFleet&) { return CommandLayer::galactic; },
                          [](const AddFleetMembers&) { return CommandLayer::galactic; },
                          [](const RemoveFleetMembers&) { return CommandLayer::galactic; },
                          [](const MergeFleets&) { return CommandLayer::galactic; },
                          [](const SetFleetOrbitSlot&) { return CommandLayer::galactic; },
                          [](const MoveFleet&) { return CommandLayer::galactic; },
                          [](const RecheckOrbit&) { return CommandLayer::galactic; },
                          [](const LandFleet&) { return CommandLayer::galactic; },
                          [](const BattleChoice&) { return CommandLayer::session; },
                          [](const AutoResolveWithdraw&) { return CommandLayer::session; },
                          [](const InvasionAccept&) { return CommandLayer::session; },
                          [](const ProductionCommand&) { return CommandLayer::galactic; },
                          [](const SellObject&) { return CommandLayer::galactic; },
                          [](const ObserverCommand&) { return CommandLayer::galactic; },
                          [](const PrepareBlackMarket&) { return CommandLayer::galactic; },
                          [](const BuyBlackMarket&) { return CommandLayer::galactic; },
                          [](const PrepareGalacticAbility&) { return CommandLayer::galactic; },
                          [](const ExecutePreparedAbility&) { return CommandLayer::galactic; },
                          [](const CancelGalacticAbility&) { return CommandLayer::galactic; },
                          [](const LaunchFromSurface&) { return CommandLayer::galactic; },
                          [](const LaunchFleet&) { return CommandLayer::galactic; },
                          [](const TransferFleetMembers&) { return CommandLayer::galactic; },

                      },
        payload);
}

std::uint16_t opcode_of(const CommandPayload& payload) noexcept {
    return std::visit(Overloaded{
                          [](const PauseGalaxy&) { return session_opcodes::pause; },
                          [](const ResumeGalaxy&) { return session_opcodes::resume; },
                          [](const BattleSummaryAcknowledge&) { return session_opcodes::dialog_acknowledge; },
                          [](const SeatLeave&) { return session_opcodes::seat_leave; },
                          [](const SeatRejoin&) { return session_opcodes::seat_rejoin; },
                          [](const AssignBattleTeamRole&) { return session_opcodes::battle_team_role; },
                          [](const SaveRequest&) { return session_opcodes::save_request; },
                          [](const PostLoadFixup&) { return session_opcodes::post_load_fixup; },
                          [](const SetSpeed& command) {
                              return command.tactical ? session_opcodes::set_tactical_speed : session_opcodes::set_strategic_speed;
                          },
                          [](const DebugSetPlanetOwner&) { return session_opcodes::debug_set_planet_owner; },
                          [](const DebugSetFleetOrbit&) { return session_opcodes::debug_set_fleet_orbit; },
                          [](const TacticalLayerCommand& command) { return std::uint16_t{command.opcode}; },
                          [](const CreateFleet&) { return galactic_opcodes::create_fleet; },
                          [](const AddFleetMembers&) { return galactic_opcodes::add_members; },
                          [](const RemoveFleetMembers&) { return galactic_opcodes::remove_members; },
                          [](const MergeFleets&) { return galactic_opcodes::merge_fleets; },
                          [](const SetFleetOrbitSlot&) { return galactic_opcodes::set_orbit_slot; },
                          [](const MoveFleet&) { return galactic_opcodes::move_fleet; },
                          [](const RecheckOrbit&) { return galactic_opcodes::recheck_orbit; },
                          [](const LandFleet&) { return galactic_opcodes::land_transports; },
                          [](const BattleChoice&) { return session_opcodes::battle_choice; },
                          [](const AutoResolveWithdraw&) { return session_opcodes::auto_resolve_withdraw; },
                          [](const InvasionAccept&) { return session_opcodes::invasion_accept; },
                          [](const ProductionCommand&) { return galactic_opcodes::production; },
                          [](const SellObject&) { return galactic_opcodes::sell_object; },
                          [](const ObserverCommand&) { return galactic_opcodes::observer_intelligence; },
                          [](const PrepareBlackMarket&) { return galactic_opcodes::black_market_prepare; },
                          [](const BuyBlackMarket&) { return galactic_opcodes::black_market_buy; },
                          [](const PrepareGalacticAbility&) { return galactic_opcodes::special_prepare; },
                          [](const ExecutePreparedAbility&) { return galactic_opcodes::special_execute; },
                          [](const CancelGalacticAbility&) { return galactic_opcodes::special_cancel; },
                          [](const LaunchFromSurface&) { return galactic_opcodes::launch_from_surface; },
                          [](const LaunchFleet&) { return galactic_opcodes::launch_fleet; },
                          [](const TransferFleetMembers&) { return galactic_opcodes::transfer_fleet_members; },

                      },
        payload);
}

std::vector<std::uint8_t> encode_payload(const CommandPayload& payload) {
    std::vector<std::uint8_t> bytes;
    const auto members = [&](const auto& command, const EntityId subject) {
        detail::put_u64(bytes, subject);
        detail::put_u32(bytes, static_cast<std::uint32_t>(command.members.size()));
        for (const auto id : command.members) detail::put_u64(bytes, id);
    };
    std::visit(Overloaded{
                   [](const PauseGalaxy&) {},
                   [&](const TransferFleetMembers& c) {
                       detail::put_u32(bytes, static_cast<std::uint32_t>(c.members.size()));
                       for (const auto& row : c.members) { detail::put_u64(bytes, row.fleet); detail::put_u64(bytes, row.member); }
                       detail::put_u64(bytes, c.target); detail::put_u8(bytes, c.slot);
                   },
                   [&](const ObserverCommand& command) { detail::encode_observer_command(bytes, command); },
                   [&](const PrepareBlackMarket& c) { detail::put_u64(bytes, c.buyer); detail::put_u64(bytes, c.planet); },
                   [&](const BuyBlackMarket& c) { detail::put_u64(bytes, c.buyer); detail::put_u32(bytes, c.index); },
                   [&](const PrepareGalacticAbility& c) {
                       detail::put_u64(bytes, c.actor); detail::put_u64(bytes, c.planet); detail::put_u8(bytes, static_cast<std::uint8_t>(c.kind));
                   },
                   [&](const ExecutePreparedAbility& c) { detail::encode_prepared_ability(bytes, c.prepared); },
                   [&](const CancelGalacticAbility& c) { detail::put_u64(bytes, c.actor); detail::put_u64(bytes, c.preparation); },
                   [](const ResumeGalaxy&) {},
                   [&](const BattleSummaryAcknowledge& command) { detail::put_u64(bytes, command.conflict); },
                   [&](const BattleChoice& command) { detail::put_u64(bytes, command.conflict); detail::put_u8(bytes, command.tactical ? 1 : 0); },
                   [&](const AutoResolveWithdraw& command) { detail::put_u64(bytes, command.conflict); },
                   [&](const InvasionAccept& command) { detail::put_u64(bytes, command.conflict); },
                   [](const SeatLeave&) {},
                   [](const SeatRejoin&) {},
                   [&](const AssignBattleTeamRole& command) {
                       detail::put_u64(bytes, command.conflict);
                       detail::put_u8(bytes, static_cast<std::uint8_t>(command.role));
                   },
                   [&](const CreateFleet& command) { members(command, command.planet); detail::put_u8(bytes, command.slot); },
                   [&](const AddFleetMembers& command) { members(command, command.fleet); },
                   [&](const RemoveFleetMembers& command) { members(command, command.fleet); },
                   [&](const MergeFleets& command) { detail::put_u64(bytes, command.source); detail::put_u64(bytes, command.target); },
                   [&](const SetFleetOrbitSlot& command) { detail::put_u64(bytes, command.fleet); detail::put_u8(bytes, command.slot); },
                   [&](const LandFleet& command) { detail::put_u64(bytes, command.fleet); detail::put_u64(bytes, command.planet); },
                   [&](const LaunchFromSurface& command) {
                       detail::put_u32(bytes, static_cast<std::uint32_t>(command.units.size()));
                       for (const auto id : command.units) detail::put_u64(bytes, id);
                       detail::put_u64(bytes, command.target); detail::put_u8(bytes, command.slot);
                   },
                   [&](const SellObject& command) {
                       detail::put_u64(bytes, command.planet); detail::put_u64(bytes, command.object);
                   },
                   [&](const LaunchFleet& command) {
                       detail::put_u32(bytes, static_cast<std::uint32_t>(command.units.size()));
                       for (const auto id : command.units) detail::put_u64(bytes, id);
                       detail::put_u32(bytes, static_cast<std::uint32_t>(command.path.size()));
                       for (const auto id : command.path) detail::put_u64(bytes, id);
                       detail::put_u8(bytes, command.destination_slot ? 1 : 0);
                       if (command.destination_slot) detail::put_u8(bytes, *command.destination_slot);
                       detail::put_u64(bytes, command.destination_actor);
                   },
                   [&](const ProductionCommand& command) {
                       detail::put_u64(bytes, command.planet); detail::put_u64(bytes, command.type);
                       detail::put_u8(bytes, static_cast<std::uint8_t>(command.action));
                       detail::put_u8(bytes, static_cast<std::uint8_t>(command.domain));
                       detail::put_u32(bytes, command.index);
                       if (command.target_build_id) {
                           detail::put_u8(bytes, 1); // optional cancel-identity version
                           detail::put_u64(bytes, *command.target_build_id);
                       }
                   },
                   [&](const RecheckOrbit& command) { detail::put_u64(bytes, command.target); },
                   [&](const MoveFleet& command) {
                       detail::put_u64(bytes, command.fleet); detail::put_u32(bytes, static_cast<std::uint32_t>(command.path.size()));
                       for (const auto id : command.path) detail::put_u64(bytes, id);
                       detail::put_u8(bytes, command.destination_slot ? 1 : 0);
                       if (command.destination_slot) detail::put_u8(bytes, *command.destination_slot);
                       detail::put_u8(bytes, command.raid ? 1 : 0);
                       detail::put_u64(bytes, command.destination_actor);
                   },
                   [&](const SaveRequest& command) {
                       detail::put_i64(bytes, command.slot);
                       detail::put_u8(bytes, command.quick ? 1 : 0);
                       detail::put_u8(bytes, command.autosave ? 1 : 0);
                       detail::put_u8(bytes, command.prebattle ? 1 : 0);
                   },
                   [&](const PostLoadFixup& command) {
                       detail::put_u32(bytes, command.pause_depth);
                       detail::put_u8(bytes, command.resume ? 1 : 0);
                       detail::put_u8(bytes, command.pending_battle ? 1 : 0);
                       detail::put_u8(bytes, command.autosave ? 1 : 0);
                   },
                   [&](const SetSpeed& command) { detail::put_i64(bytes, command.factor.raw()); },
                   [&](const DebugSetPlanetOwner& command) {
                       detail::put_u64(bytes, command.planet);
                       detail::put_u32(bytes, command.owner);
                   },
                   [&](const DebugSetFleetOrbit& command) {
                       detail::put_u64(bytes, command.fleet);
                       detail::put_u64(bytes, command.planet);
                   },
                   [&](const TacticalLayerCommand& command) {
                       detail::put_u64(bytes, command.conflict);
                       detail::put_bytes(bytes, command.body);
                   },
               },
        payload);
    return bytes;
}

std::optional<CommandPayload> decode_payload(
    const CommandLayer layer, const std::uint16_t opcode, const std::vector<std::uint8_t>& bytes) {
    sim::detail::Reader reader(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    switch (layer) {
    case CommandLayer::session:
        if (opcode == session_opcodes::dialog_acknowledge) {
            BattleSummaryAcknowledge command;
            if (!reader.read_u64(command.conflict) || !command.conflict || reader.remaining()) return std::nullopt;
            return CommandPayload{command};
        }
        if (opcode == session_opcodes::auto_resolve_withdraw) {
            AutoResolveWithdraw command;
            if (!reader.read_u64(command.conflict) || !command.conflict || reader.remaining()) return std::nullopt;
            return CommandPayload{command};
        }
        if (opcode == session_opcodes::invasion_accept) {
            InvasionAccept command;
            if (!reader.read_u64(command.conflict) || !command.conflict || reader.remaining()) return std::nullopt;
            return CommandPayload{command};
        }
        if (opcode == session_opcodes::battle_choice) {
            BattleChoice command; std::uint8_t choice{};
            if (!reader.read_u64(command.conflict) || !command.conflict || !reader.read_u8(choice) || choice > 1 || reader.remaining()) return std::nullopt;
            command.tactical = choice != 0; return CommandPayload{command};
        }
        if (opcode == session_opcodes::seat_leave || opcode == session_opcodes::seat_rejoin) {
            if (!bytes.empty()) return std::nullopt;
            return opcode == session_opcodes::seat_leave ? CommandPayload{SeatLeave{}} : CommandPayload{SeatRejoin{}};
        }
        if (opcode == session_opcodes::battle_team_role) {
            AssignBattleTeamRole command; std::uint8_t role{};
            if (!reader.read_u64(command.conflict) || command.conflict == 0 || !reader.read_u8(role)
                || role < 1 || role > 3 || reader.remaining()) return std::nullopt;
            command.role = static_cast<BattleTeamRole>(role); return CommandPayload{command};
        }
        if (opcode == session_opcodes::save_request) {
            SaveRequest command; std::uint8_t quick{}, autosave{}, prebattle{};
            if (!reader.read_i64(command.slot) || !reader.read_u8(quick) || !reader.read_u8(autosave)
                || !reader.read_u8(prebattle) || reader.remaining() != 0 || quick > 1 || autosave > 1
                || prebattle > 1 || (quick && autosave) || (prebattle && !autosave)) return std::nullopt;
            command.quick = quick != 0; command.autosave = autosave != 0; command.prebattle = prebattle != 0;
            return CommandPayload{command};
        }
        if (opcode == session_opcodes::post_load_fixup) {
            PostLoadFixup command; std::uint8_t resume{}, pending{}, autosave{};
            if (!reader.read_u32(command.pause_depth) || !reader.read_u8(resume) || !reader.read_u8(pending)
                || !reader.read_u8(autosave) || reader.remaining() != 0 || resume > 1 || pending > 1 || autosave > 1
                || command.pause_depth > 1 || (resume && command.pause_depth != 0)) return std::nullopt;
            command.resume = resume != 0; command.pending_battle = pending != 0; command.autosave = autosave != 0;
            return CommandPayload{command};
        }
        if (opcode == session_opcodes::pause || opcode == session_opcodes::resume) {
            if (!bytes.empty()) return std::nullopt;
            return opcode == session_opcodes::pause ? CommandPayload{PauseGalaxy{}} : CommandPayload{ResumeGalaxy{}};
        }
        if (opcode == session_opcodes::set_strategic_speed || opcode == session_opcodes::set_tactical_speed) {
            std::int64_t raw{};
            if (!reader.read_i64(raw) || reader.remaining() != 0 || raw <= 0 || raw > max_speed_factor_raw) return std::nullopt;
            return CommandPayload{SetSpeed{opcode == session_opcodes::set_tactical_speed, math::Fixed::from_raw(raw)}};
        }
        if (opcode == session_opcodes::debug_set_planet_owner) {
            DebugSetPlanetOwner command;
            if (!reader.read_u64(command.planet) || !reader.read_u32(command.owner) || reader.remaining() != 0) return std::nullopt;
            return CommandPayload{command};
        }
        if (opcode == session_opcodes::debug_set_fleet_orbit) {
            DebugSetFleetOrbit command;
            if (!reader.read_u64(command.fleet) || !reader.read_u64(command.planet) || reader.remaining() != 0) return std::nullopt;
            return CommandPayload{command};
        }
        return std::nullopt; // reserved or unknown: no session command is implemented there yet
    case CommandLayer::galactic:
        if (opcode == galactic_opcodes::transfer_fleet_members) {
            std::uint32_t count{};
            constexpr std::size_t tail = 9;
            if (!reader.read_u32(count) || !count || count > (galactic_replay_max_payload - 4 - tail) / 16
                || std::size_t{count} * 16 + tail != reader.remaining()) return std::nullopt;
            std::vector<FleetMemberRef> refs;
            std::set<EntityId> seen;
            for (std::uint32_t i = 0; i < count; ++i) {
                FleetMemberRef row;
                if (!reader.read_u64(row.fleet) || !row.fleet || !reader.read_u64(row.member) || !row.member
                    || !seen.insert(row.member).second) return std::nullopt;
                refs.push_back(row);
            }
            TransferFleetMembers c; c.members = std::move(refs);
            if (!reader.read_u64(c.target) || !reader.read_u8(c.slot) || c.slot > 2 || (c.target && c.slot)
                || reader.remaining()) return std::nullopt;
            return c;
        }
        if (opcode == galactic_opcodes::special_prepare) {
            PrepareGalacticAbility c; std::uint8_t kind{};
            if (!reader.read_u64(c.actor) || !c.actor || !reader.read_u64(c.planet) || !c.planet
                || !reader.read_u8(kind) || kind < 1 || kind > 7 || reader.remaining()) return {};
            c.kind = static_cast<AbilityKind>(kind); return c;
        }
        if (opcode == galactic_opcodes::special_execute) {
            auto p = detail::decode_prepared_ability(bytes);
            if (!p) { return {}; }
            return ExecutePreparedAbility{*p};
        }
        if (opcode == galactic_opcodes::special_cancel) {
            CancelGalacticAbility c;
            if (!reader.read_u64(c.actor) || !c.actor || !reader.read_u64(c.preparation) || !c.preparation || reader.remaining()) return {};
            return c;
        }
        if (opcode == galactic_opcodes::black_market_prepare) {
            PrepareBlackMarket c;
            if (!reader.read_u64(c.buyer) || !c.buyer || !reader.read_u64(c.planet) || !c.planet || reader.remaining()) return std::nullopt;
            return c;
        }
        if (opcode == galactic_opcodes::black_market_buy) {
            BuyBlackMarket c;
            if (!reader.read_u64(c.buyer) || !c.buyer || !reader.read_u32(c.index) || reader.remaining()) return std::nullopt;
            return c;
        }
        if (opcode == galactic_opcodes::observer_intelligence) {
            auto command = detail::decode_observer_command(bytes);
            if (!command) return std::nullopt;
            return *command;
        }
        {
        if (opcode == galactic_opcodes::sell_object) {
            SellObject c;
            if (!reader.read_u64(c.planet) || !c.planet || !reader.read_u64(c.object) || !c.object || reader.remaining()) return std::nullopt;
            return CommandPayload{c};
        }
        if (opcode == galactic_opcodes::production) {
            ProductionCommand c; std::uint8_t action{}, domain{};
            if (!reader.read_u64(c.planet) || c.planet == 0 || !reader.read_u64(c.type)
                || !reader.read_u8(action) || action > 3 || !reader.read_u8(domain) || domain > 1
                || !reader.read_u32(c.index)
                || (action != 3 && c.type == 0) || (action == 3 && c.type != 0)
                || (action != 1 && c.index != 0)) return std::nullopt;
            if (reader.remaining() != 0) {
                std::uint8_t version{}; EntityId build{};
                if (action != 1 || !reader.read_u8(version) || version != 1
                    || !reader.read_u64(build) || build == 0 || reader.remaining() != 0) return std::nullopt;
                c.target_build_id = build;
            }
            c.action = static_cast<ProductionAction>(action); c.domain = static_cast<ProductionDomain>(domain);
            return CommandPayload{c};
        }
        const auto members = [&](auto& command, EntityId& subject) {
            std::uint32_t count{};
            if (!reader.read_u64(subject) || subject == 0 || !reader.read_u32(count) || count == 0
                || count > (galactic_replay_max_payload - 13) / 8 || std::size_t{count} * 8 > reader.remaining()) return false;
            std::set<EntityId> seen;
            for (std::uint32_t i = 0; i < count; ++i) {
                EntityId id{};
                if (!reader.read_u64(id) || id == 0 || !seen.insert(id).second) return false;
                command.members.push_back(id);
            }
            return true;
        };
        if (opcode == galactic_opcodes::land_transports) {
            LandFleet c;
            if (!reader.read_u64(c.fleet) || !reader.read_u64(c.planet) || c.fleet == 0 || c.planet == 0 || reader.remaining()) return std::nullopt;
            return CommandPayload{c};
        }
        if (opcode == galactic_opcodes::launch_from_surface) {
            // u32 count, count * u64 unit, u64 target, u8 slot (13 fixed bytes besides the units).
            LaunchFromSurface c; std::uint32_t count{};
            if (!reader.read_u32(count) || count == 0 || count > (galactic_replay_max_payload - 13) / 8
                || std::size_t{count} * 8 + 9 != reader.remaining()) return std::nullopt;
            std::set<EntityId> seen;
            for (std::uint32_t i = 0; i < count; ++i) {
                EntityId id{};
                if (!reader.read_u64(id) || id == 0 || !seen.insert(id).second) return std::nullopt;
                c.units.push_back(id);
            }
            if (!reader.read_u64(c.target) || !reader.read_u8(c.slot) || c.slot > 2 || (c.target && c.slot)
                || reader.remaining()) return std::nullopt;
            return CommandPayload{std::move(c)};
        }
        if (opcode == galactic_opcodes::launch_fleet) {
            if (bytes.size() > galactic_replay_max_payload) return std::nullopt;
            LaunchFleet c; std::uint32_t count{}; std::uint8_t present{};
            if (!reader.read_u32(count) || count == 0 || std::size_t{count} > reader.remaining() / 8) return std::nullopt;
            std::set<EntityId> units;
            for (std::uint32_t i = 0; i < count; ++i) {
                EntityId id{};
                if (!reader.read_u64(id) || !id || !units.insert(id).second) return std::nullopt;
                c.units.push_back(id);
            }
            if (!reader.read_u32(count) || count < 2 || count > 4096 || std::size_t{count} > reader.remaining() / 8) return std::nullopt;
            std::set<PlanetId> planets;
            for (std::uint32_t i = 0; i < count; ++i) {
                PlanetId id{};
                if (!reader.read_u64(id) || !id || !planets.insert(id).second) return std::nullopt;
                c.path.push_back(id);
            }
            if (!reader.read_u8(present) || present > 1) return std::nullopt;
            if (present) {
                std::uint8_t slot{};
                if (!reader.read_u8(slot) || slot > 2) return std::nullopt;
                c.destination_slot = slot;
            }
            if (!reader.read_u64(c.destination_actor) || reader.remaining()) return std::nullopt;
            return CommandPayload{std::move(c)};
        }
        if (opcode == galactic_opcodes::recheck_orbit) {
            RecheckOrbit c;
            if (!reader.read_u64(c.target) || !c.target || reader.remaining()) return std::nullopt;
            return CommandPayload{c};
        }
        if (opcode == galactic_opcodes::move_fleet) {
            MoveFleet c; std::uint32_t count{}; std::uint8_t present{};
            if (!reader.read_u64(c.fleet) || c.fleet == 0 || !reader.read_u32(count) || count < 1 || count > 4096
                || std::size_t{count} * 8 > reader.remaining()) return std::nullopt;
            std::set<PlanetId> seen;
            for (std::uint32_t i = 0; i < count; ++i) {
                PlanetId id{}; if (!reader.read_u64(id) || id == 0) return std::nullopt;
                // WGM-99 permits the orbital retreat fallback [source, source].
                if (!seen.insert(id).second && !(count == 2 && i == 1 && id == c.path.front())) return std::nullopt;
                c.path.push_back(id);
            }
            if (!reader.read_u8(present) || present > 1) return std::nullopt;
            if (present) { std::uint8_t slot{}; if (!reader.read_u8(slot) || slot > 2) return std::nullopt; c.destination_slot = slot; }
            std::uint8_t raid{};
            if (!reader.read_u8(raid) || raid > 1 || !reader.read_u64(c.destination_actor) || reader.remaining()) return std::nullopt;
            c.raid = raid != 0;
            return CommandPayload{std::move(c)};
        }
        if (opcode == galactic_opcodes::create_fleet) {
            CreateFleet c;
            if (!members(c, c.planet) || !reader.read_u8(c.slot) || c.slot > 2 || reader.remaining()) return std::nullopt;
            return CommandPayload{std::move(c)};
        }
        if (opcode == galactic_opcodes::add_members) {
            AddFleetMembers c;
            if (!members(c, c.fleet) || reader.remaining()) return std::nullopt;
            return CommandPayload{std::move(c)};
        }
        if (opcode == galactic_opcodes::remove_members) {
            RemoveFleetMembers c;
            if (!members(c, c.fleet) || reader.remaining()) return std::nullopt;
            return CommandPayload{std::move(c)};
        }
        if (opcode == galactic_opcodes::merge_fleets) {
            MergeFleets c;
            if (!reader.read_u64(c.source) || !reader.read_u64(c.target) || c.source == 0 || c.target == 0 || reader.remaining()) return std::nullopt;
            return CommandPayload{c};
        }
        if (opcode == galactic_opcodes::set_orbit_slot) {
            SetFleetOrbitSlot c;
            if (!reader.read_u64(c.fleet) || c.fleet == 0 || !reader.read_u8(c.slot) || c.slot > 2 || reader.remaining()) return std::nullopt;
            return CommandPayload{c};
        }
        return std::nullopt;
    }
    case CommandLayer::tactical: {
        if (opcode == 0 || opcode > 0xffU) return std::nullopt;
        TacticalLayerCommand command;
        command.opcode = static_cast<std::uint8_t>(opcode);
        if (!reader.read_u64(command.conflict)) return std::nullopt;
        command.body.assign(bytes.begin() + 8, bytes.end());
        return CommandPayload{command};
    }
    }
    return std::nullopt;
}

} // namespace eawr::sim::galactic

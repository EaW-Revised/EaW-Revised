#include "galactic_support.hpp"
#include "eawr/campaign/fleets.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/fleets.hpp"

#include <filesystem>
#include <fstream>

namespace {
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
gc::GalacticSetup setup() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {}, 2}};
    s.fleets = {{10, 1, 1, {{20, 1}, {21, 1}}}, {11, 1, 1, {{22, 1}}},
        {12, 2, 1, {{30, 1}}}, {13, 1, 2, {{31, 1}}}};
    s.rules.maximum_fleet_movement_distance = eawr::sim::math::Fixed{};
    s.fleet_control.emplace();
    s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}, {3, false, false, false}, {4, false, true, true}};
    s.fleet_control->detached_units = {{{40, 1}, 1, 1}, {{41, 2}, 1, 1}};
    return s;
}
gc::GalacticReplay replay() {
    gc::GalacticReplay r; r.setup = setup(); r.final_tick_count = 12;
    r.commands = {
        {{0, gc::SeatId{1}, 1}, gc::CreateFleet{1, {40, 41}, 2}},
        {{1, gc::SeatId{1}, 2}, gc::RemoveFleetMembers{10, {20}}}, {{1, gc::SeatId{1}, 3}, gc::AddFleetMembers{11, {20}}},
        {{2, gc::SeatId{1}, 4}, gc::MergeFleets{11, 42}},
        {{3, gc::SeatId{1}, 5}, gc::RemoveFleetMembers{10, {21}}}, {{3, gc::SeatId{1}, 6}, gc::CreateFleet{1, {21}, 0}},
        {{4, gc::SeatId{1}, 7}, gc::MergeFleets{43, 42}}, {{5, gc::SeatId{1}, 8}, gc::SetFleetOrbitSlot{42, 1}},
        {{6, gc::SeatId{1}, 9}, gc::MergeFleets{12, 42}}, {{7, gc::SeatId{1}, 10}, gc::RemoveFleetMembers{42, {40, 999}}},
        {{8, gc::SeatId{1}, 11}, gc::PauseGalaxy{}}, {{8, gc::SeatId{1}, 12}, gc::CreateFleet{1, {999}, 0}},
        {{9, gc::SeatId{1}, 13}, gc::ResumeGalaxy{}}, {{9, gc::SeatId{1}, 14}, gc::SetFleetOrbitSlot{42, 0}}};
    return r;
}
const gc::Fleet* fleet(const gc::GalacticSnapshot& s, const gc::FleetId id) {
    for (const auto& row : s.fleets()) if (row.id == id) return &row;
    return nullptr;
}
void contracts() {
    const auto r = replay();
    const auto bytes = gc::write_replay(r).value();
    const auto parsed = gc::parse_replay(bytes);
    expect(parsed && parsed.value() == r, "fleet extension and all five commands roundtrip");
    for (const auto& c : r.commands)
        expect(gc::decode_payload(gc::layer_of(c.payload), gc::opcode_of(c.payload), gc::encode_payload(c.payload)) == c.payload,
            "typed command canonical payload");
    expect(!gc::decode_payload(gc::CommandLayer::galactic, 1, {}), "truncated create payload refuses");
    auto duplicate = r; duplicate.commands[0].payload = gc::CreateFleet{1, {40, 40}, 0};
    expect(!gc::write_replay(duplicate), "duplicate member list refused before execution");
    auto bad = setup(); bad.fleet_control->detached_units[0].member.id = 20;
    expect(!gc::validate_setup(bad), "attached/detached identity collision refuses");
    bad = setup(); bad.fleet_control->types.clear(); expect(!gc::validate_setup(bad), "missing original traits refuses");
    bad = setup(); bad.rules.maximum_fleet_movement_distance.reset(); expect(!gc::validate_setup(bad), "fleet control requires graph");
    auto truncated = bytes; truncated.resize(101); expect(!gc::parse_replay(truncated), "truncated extension refuses");
    auto unknown = bytes; unknown[118] = 9; expect(!gc::parse_replay(unknown), "unknown second extension refuses");
    const auto types = setup().fleet_control->types;
    expect(gc::fleet_move_without_ability({99, 1, 1, {}}, types).value(), "empty flattened fleet qualifies");
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 2}}}, types).value(), "transport cannot qualify even with true flag");
    expect(gc::fleet_move_without_ability({99, 1, 1, {{40, 3}, {41, 1}}}, types).value(), "any qualifying member suffices");
    // WGM-83 breakdown: a transport's carried units are listed too (#3188), one level.
    gc::MovementSetup carried; carried.contents = {{40, {{50, 1}}}, {41, {{51, 3}}}, {42, {}}};
    expect(gc::fleet_move_without_ability({99, 1, 1, {{40, 2}}}, types, &carried).value(),
        "transport carrying a qualifying unit qualifies the fleet");
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{41, 2}}}, types, &carried).value(),
        "transport carrying only non-qualifying units still fails");
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{42, 2}}}, types, &carried).value()
        && !gc::fleet_move_without_ability({99, 1, 1, {{43, 2}}}, types, &carried).value(),
        "empty or unlisted transport contents do not qualify");
    carried.contents = {{40, {{50, 77}}}};
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 2}}}, types, &carried),
        "carried unit without original traits refuses explicitly");
    const auto nested = gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types);
    expect(!nested && nested.error().message.find("unverified boundary") != std::string::npos, "live container flattening explicit diagnostic");
    carried.contents = {{40, {{50, 4}}}, {50, {{60, 1}}}};
    expect(gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried).value(),
        "nested live containers qualify through surviving craft");
    carried.contents[1].children = {{60, 3}};
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried).value(),
        "container template true cannot qualify ability-dependent surviving craft");
    carried.contents[1].children.clear();
    expect(gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried).value(),
        "empty live flattened membership qualifies");
    carried.contents.pop_back();
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried),
        "missing nested live contents refuses explicitly");
    carried.contents = {{40, {{40, 4}}}};
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried),
        "cyclic unvalidated contents refuse within the traversal bound");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = eawr::campaign::GameSession::create(r.setup).value();
        for (const auto& c : r.commands) expect(static_cast<bool>(game.submit(c)), "submit fleet command");
        game.scramble_storage_for_testing();
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < r.final_tick_count; ++tick) {
            const auto step = game.step(executor).value(); hashes.push_back(step.state_hash.get() + ":" + step.galaxy->sha256());
            expect(game.galaxy().state_sha256() == game.galaxy().cached_state_hash().get(), "fleet cached digest equals full state");
            expect(game.state_sha256() == step.state_hash.get(), "campaign fleet digest includes the mode timeline");
            if (tick == 0) expect(fleet(*step.galaxy, 42)->members == std::vector<gc::FleetMember>{{40, 1}, {41, 2}}
                && fleet(*step.galaxy, 42)->orbit_slot == 2, "create ordered identities and slot override");
            if (tick == 2) expect(!fleet(*step.galaxy, 11) && fleet(*step.galaxy, 42)->members
                == std::vector<gc::FleetMember>{{40, 1}, {41, 2}, {20, 1}, {22, 1}}, "merge source last-first and empty cleanup");
            if (tick == 4) expect(!fleet(*step.galaxy, 10) && !fleet(*step.galaxy, 43)
                && fleet(*step.galaxy, 42)->members.back().id == 21, "split and merge preserve identities, destroy empty containers");
            if (tick == 6 || tick == 7 || tick == 8) {
                const auto& events = step.galaxy->events();
                expect(events.back().kind == gc::GalacticEventKind::command_rejected, "owner, membership and pause rejections");
                expect(game.galaxy().next_entity_id() == 44 && fleet(*step.galaxy, 42)->members.size() == 5,
                    "rejected batch changes no members or allocator frontier");
            }
            if (tick == 5) {
                const auto ids = eawr::checkpoint::game_identities(r.setup);
                const auto saved = eawr::checkpoint::save_game_checkpoint(game, ids).value();
                auto restored = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor);
                expect(restored && restored.value().session.state_sha256() == game.state_sha256()
                    && restored.value().session.galaxy().snapshot()->canonical_bytes() == game.galaxy().snapshot()->canonical_bytes(),
                    "fleet rules/identities/slots/events reconstruct through save");
            }
        }
        expect(game.galaxy().snapshot()->fleet_control()->detached_units.empty(), "all detached identities transferred exactly once");
        if (workers == 1) reference = hashes; else expect(reference == hashes, "fleet commands 1/2/4/8 hashes equal");
    }
    // Location admission and static typed WGM-07 gate input.
    auto session = gc::GalacticSession::create(setup()).value(); const eawr::sim::InlineExecutor inline_executor;
    expect(static_cast<bool>(session.submit({{0, gc::SeatId{1}, 1}, gc::MergeFleets{13, 10}})), "location order submits");
    expect(session.step(inline_executor).value().snapshot->events().back().reason == gc::GalacticRejectReason::incompatible_location,
        "same owner on different planets cannot merge");
    bad = setup(); bad.fleet_control->gates.active_speech = true;
    session = gc::GalacticSession::create(bad).value();
    expect(static_cast<bool>(session.submit({{0, gc::SeatId{1}, 1}, gc::CreateFleet{1, {40}, 0}})), "gated order submits");
    expect(session.step(inline_executor, true).value().snapshot->events().back().reason == gc::GalacticRejectReason::paused,
        "speech plus queued local conflict suppresses single-player management");
}
void adapter() {
    eawr::units::GalacticTypeTable table;
    eawr::units::GalacticType ship; ship.name = "Ship"; ship.object_class = "SpaceUnit"; ship.can_hyperspace_without_ability = true;
    eawr::units::GalacticType transport; transport.name = "Transport"; transport.object_class = "TransportUnit";
    transport.behavior = {"TRANSPORT"};
    eawr::units::GalacticType squadron; squadron.name = "Squadron"; squadron.object_class = "Squadron";
    table.types = {ship, transport, squadron};
    const std::vector<eawr::campaign::FleetTypeBinding> bindings{{7, "ship"}, {9, "Transport"}, {20, "Squadron"}};
    const auto built = eawr::campaign::build_fleet_type_traits(table, bindings);
    expect(built && built.value() == std::vector<gc::FleetTypeTraits>{{7, false, true, false}, {9, true, true, false}, {20, false, true, true}},
        "WGM-100 shared true omission default retains transport and live-child classification");
    table.types[0].can_hyperspace_without_ability.reset();
    const auto omitted = eawr::campaign::build_fleet_type_traits(table, bindings);
    expect(omitted && omitted.value().front().can_hyperspace_without_ability, "WGM-100 ordinary omitted capability uses the verified true default");
    table.types[0].can_hyperspace_without_ability = false;
    expect(!eawr::campaign::build_fleet_type_traits(table, bindings).value().front().can_hyperspace_without_ability,
        "authored false capability precedes the constructor default");
    table.types[1].object_class = "UniqueUnit";
    table.types[1].can_hyperspace_without_ability = true;
    const auto hero = eawr::campaign::build_fleet_type_traits(table, bindings).value();
    expect(hero[1].transport && !gc::fleet_move_without_ability({99, 1, 1, {{40, 9}}}, hero).value(),
        "WGM-83 hero transport behaviour defeats ordinary capability despite its UniqueUnit class");
}
void member_transfer_contracts() {
    const std::vector<gc::CommandPayload> commands{
        gc::TransferFleetMembers{{{10, 20}}, 11, 0},
        gc::TransferFleetMembers{{{10, 21}, {11, 22}}, 0, 2},
        gc::TransferFleetMembers{{{42, 21}, {42, 22}}, 11, 0},
        gc::TransferFleetMembers{{{11, 20}, {11, 999}}, 0, 2},
        gc::TransferFleetMembers{{{11, 20}}, 12, 0},
        gc::TransferFleetMembers{{{11, 20}}, 13, 0},
        gc::TransferFleetMembers{{{12, 30}}, 11, 0},
        gc::TransferFleetMembers{{{11, 20}}, 11, 0}};
    for (const auto& command : commands)
        expect(gc::decode_payload(gc::layer_of(command), gc::opcode_of(command), gc::encode_payload(command)) == command,
            "member transfer payload roundtrip");
    const gc::CommandPayload duplicate = gc::TransferFleetMembers{{{10, 20}, {10, 20}}, 0, 2};
    expect(!gc::decode_payload(gc::layer_of(duplicate), gc::opcode_of(duplicate), gc::encode_payload(duplicate)),
        "duplicate transfer identities refuse before admission");
    const gc::CommandPayload empty = gc::TransferFleetMembers{{}, 0, 1};
    expect(!gc::decode_payload(gc::layer_of(empty), gc::opcode_of(empty), gc::encode_payload(empty)), "empty transfer list refuses");
    const auto configured = setup();
    gc::GalacticReplay recording{configured, commands.size(), {}, {}};
    for (std::size_t tick = 0; tick < commands.size(); ++tick)
        recording.commands.push_back({{tick, gc::SeatId{1}, tick + 1}, commands[tick]});
    expect(gc::parse_replay(gc::write_replay(recording).value()).value() == recording, "member transfer replay roundtrip");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = eawr::campaign::GameSession::create(configured).value();
        for (const auto& command : recording.commands) expect(bool(game.submit(command)), "member command submits");
        game.scramble_storage_for_testing();
        std::vector<std::string> hashes;
        for (std::size_t tick = 0; tick < commands.size(); ++tick) {
            const auto step = game.step(executor).value();
            const auto& snapshot = *step.galaxy;
            hashes.push_back(step.state_hash.get());
            expect(game.galaxy().state_sha256() == game.galaxy().cached_state_hash().get(), "transfer dirties canonical state digest");
            if (tick == 0) {
                expect(fleet(snapshot, 10)->members == std::vector<gc::FleetMember>{{21, 1}}
                    && fleet(snapshot, 11)->members == std::vector<gc::FleetMember>{{22, 1}, {20, 1}},
                    "plain transfer moves one identity and leaves the rest of its type stack");
                expect(std::count_if(snapshot.events().begin(), snapshot.events().end(), [](const auto& event) {
                    return event.kind == gc::GalacticEventKind::member_detached || event.kind == gc::GalacticEventKind::member_attached;
                }) == 2, "transfer publishes source detach and destination attach events");
            }
            if (tick == 1) expect(!fleet(snapshot, 10) && fleet(snapshot, 42)->orbit_slot == 2
                && fleet(snapshot, 42)->members == std::vector<gc::FleetMember>{{21, 1}, {22, 1}},
                "type stack across two source fleets creates one fleet in empty slot and cleans empty source");
            if (tick == 2) expect(!fleet(snapshot, 42) && fleet(snapshot, 11)->members.size() == 3,
                "complete member transfer destroys empty fleet shell");
            if (tick >= 3) expect(snapshot.events().back().kind == gc::GalacticEventKind::command_rejected
                && fleet(snapshot, 11)->members.size() == 3 && game.galaxy().next_entity_id() == 43,
                "invalid member, foreign source/target, remote target and self transfer refuse atomically");
            if (tick == 1 || tick == 2) {
                const auto ids = eawr::checkpoint::game_identities(configured);
                auto saved = eawr::checkpoint::save_game_checkpoint(game, ids).value();
                auto restored = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor);
                expect(restored && restored.value().session.state_sha256() == step.state_hash.get()
                    && restored.value().session.galaxy().snapshot()->canonical_bytes() == snapshot.canonical_bytes(),
                    "member transfer state reconstructs through save/load");
            }
        }
        if (workers == 1) reference = hashes;
        else expect(reference == hashes, "member transfer hashes equal on 1/2/4/8 workers");
    }
}

} // namespace
int main(const int argc, const char* const argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "--generate") {
        const auto r = replay(); auto game = eawr::campaign::GameSession::create(r.setup).value();
        for (const auto& c : r.commands) if (!game.submit(c)) return 1;
        const std::filesystem::path directory(argv[2]); std::filesystem::create_directories(directory);
        std::ofstream hashes(directory / "fleet-commands.hashes.csv", std::ios::binary); hashes << "tick,sha256\n";
        const eawr::sim::InlineExecutor executor;
        for (std::uint64_t i = 0; i < r.final_tick_count; ++i) { const auto step = game.step(executor).value(); hashes << step.completed_tick << ',' << step.state_hash.get() << '\n'; }
        const auto bytes = gc::write_replay(game.record()).value();
        std::ofstream out(directory / "fleet-commands.eawr-gcreplay", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return out && hashes ? 0 : 1;
    }
    contracts(); adapter(); member_transfer_contracts(); return galactic_test::failures == 0 ? 0 : 1;
}

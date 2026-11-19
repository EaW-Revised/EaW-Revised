// #7869: humans joining a loaded multiplayer save (SeatAdmit, the load prologue), and leaving / rejoining afterwards.
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace galactic_test;
namespace campaign = eawr::campaign;
namespace tactical = eawr::sim::tactical;
namespace cp = eawr::checkpoint;
using eawr::testing::result_code_colon::take;
void check(eawr::core::Result<void> result) { eawr::testing::must(std::move(result)); }

constexpr std::uint32_t ai_seat_of(std::uint32_t player) { return 8 + player; }

// Four factions. Seats 1 and 2 are humans sharing faction 1 (side 1); every faction has an AI issuer seat 8 + player,
// active only for the AI factions. With `second_side`, faction 2 is a human side too (seat 3).
galactic::GalacticSetup saved_setup(const bool second_side = false) {
    galactic::SyntheticGalaxyOptions options; options.planets = 8; options.players = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1; options.debug_commands = true;
    auto result = galactic::synthetic_galaxy(options); result.seats.clear();
    result.seats.push_back({{1}, 1, galactic::SeatKind::human, 0, true, 1});
    result.seats.push_back({{2}, 1, galactic::SeatKind::human, 0, true, 1});
    if (second_side) result.seats.push_back({{3}, 2, galactic::SeatKind::human, 1, true, 2});
    for (std::uint32_t player = 1; player <= 4; ++player)
        result.seats.push_back({{ai_seat_of(player)}, player, galactic::SeatKind::ai, 0,
            player > (second_side ? 2U : 1U), 0});
    result.seat_control.emplace();
    result.seat_control->teams.push_back({1, {1}, false});
    if (second_side) result.seat_control->teams.push_back({2, {3}, false});
    for (std::uint32_t player = 1; player <= 4; ++player)
        result.seat_control->controllers.push_back({player,
            player <= (second_side ? 2U : 1U) ? galactic::FactionControl::human : galactic::FactionControl::ai,
            galactic::FactionControl::ai, {1, 2}, {3, 4, 5}});
    result.networked_transport = true;
    return result;
}

cp::GameLoadOptions load_options() {
    cp::GameLoadOptions options; options.script_requirement = campaign::ScriptRequirement::none;
    options.content = [](const campaign::ConflictRecord&) { return eawr::core::Result<tactical::TacticalContent>::success({}); };
    return options;
}

// What a peer does at a resume: restore the saved bytes, then admit the extras.
struct Peer {
    campaign::GameSession session;
};
Peer resume(const std::vector<std::uint8_t>& bytes, const galactic::GalacticSetup& config,
    const std::vector<galactic::PlayerId>& extras, const eawr::sim::PartitionExecutor& executor) {
    auto restored = take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(), executor));
    check(restored.session.admit_human_seats(extras));
    return {std::move(restored.session)};
}
std::vector<std::uint8_t> saved_bytes(const galactic::GalacticSetup& config, const eawr::sim::PartitionExecutor& executor) {
    auto game = take(campaign::GameSession::create(config));
    for (int tick = 0; tick < 3; ++tick) (void)take(game.step(executor));
    return take(cp::save_game_checkpoint(game, cp::game_identities(config)));
}
const galactic::Seat* seat_of(const campaign::GameSession& game, const std::uint32_t id) {
    const auto seats = game.galaxy().seats();
    const auto found = std::find_if(seats.begin(), seats.end(), [&](const auto& row) { return row.seat_id.value == id; });
    return found == seats.end() ? nullptr : &*found;
}
std::size_t humans(const campaign::GameSession& game) {
    const auto seats = game.galaxy().seats();
    return static_cast<std::size_t>(std::count_if(seats.begin(), seats.end(), [](const auto& row) { return row.kind == galactic::SeatKind::human; }));
}
const galactic::FactionController& controller(const campaign::GameSession& game, const std::uint32_t player) {
    const auto& rows = game.galaxy().seat_control()->control.controllers;
    return *std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.player == player; });
}

void shared_seat_and_takeover() {
    const eawr::sim::InlineExecutor executor;
    const auto config = saved_setup();
    const auto bytes = saved_bytes(config, executor);

    auto shared = resume(bytes, config, {1}, executor).session;
    const auto* extra = seat_of(shared, 13);
    expect(extra && extra->kind == galactic::SeatKind::human && extra->active && extra->player == 1 && extra->human_side_id == 1,
        "an extra human on a faction that had humans shares its side, with the next free seat id");
    expect(humans(shared) == 3 && controller(shared, 1).kind == galactic::FactionControl::human, "the shared faction stays human-controlled");
    expect(shared.galaxy().seat_control()->control.teams.size() == 1 && shared.galaxy().seat_control()->control.teams.front().lead.value == 1,
        "the lead stays the lowest active seat");

    auto taken = resume(bytes, config, {2, 2}, executor).session;
    expect(humans(taken) == 4 && seat_of(taken, 13) && seat_of(taken, 14), "two humans take over an AI faction");
    expect(seat_of(taken, 13)->human_side_id == 2 && seat_of(taken, 14)->human_side_id == 2, "they share the one new side");
    expect(taken.galaxy().seat_control()->control.teams.size() == 2 && taken.galaxy().seat_control()->control.teams.back().side == 2
        && taken.galaxy().seat_control()->control.teams.back().lead.value == 13, "the new side is led by its lowest seat");
    expect(controller(taken, 2).kind == galactic::FactionControl::human && !seat_of(taken, ai_seat_of(2))->active,
        "the AI stops controlling the taken-over faction");
    expect(seat_of(taken, ai_seat_of(3))->active && controller(taken, 3).kind == galactic::FactionControl::ai, "other AI factions are untouched");
    expect(controller(taken, 2).configuration == std::vector<std::uint8_t>({1, 2}) && controller(taken, 2).state == std::vector<std::uint8_t>({3, 4, 5}),
        "the frozen AI keeps its configuration and state");

    auto fresh = take(campaign::GameSession::create(config));
    expect(!fresh.submit({{3, {13}, 1}, galactic::SeatAdmit{1, 0, 1}}) && !shared.submit({{shared.completed_tick(), {1}, 1}, galactic::SeatAdmit{1, 0, 1}}),
        "a peer can never submit a seat admission");
    const auto planned = galactic::plan_seat_admissions(shared.galaxy().seats(), std::vector<galactic::PlayerId>{3, 3}, 5);
    expect(planned == galactic::plan_seat_admissions(shared.galaxy().seats(), std::vector<galactic::PlayerId>{3, 3}, 5)
        && planned.size() == 2 && planned[0].key.seat.value == 14 && planned[1].key.seat.value == 15, "the plan is a pure function of the table and the list");
}

void refusals() {
    const eawr::sim::InlineExecutor executor;
    {
        const auto config = saved_setup(true);
        const auto bytes = saved_bytes(config, executor);
        auto restored = take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(), executor));
        expect(!restored.session.admit_human_seats(std::vector<galactic::PlayerId>{3}), "a third human side is refused");
        check(restored.session.admit_human_seats(std::vector<galactic::PlayerId>{1, 2}));
        expect(humans(restored.session) == 5, "extras may still join either existing side");
    }
    {
        const auto config = saved_setup();
        const auto bytes = saved_bytes(config, executor);
        auto restored = take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(), executor));
        expect(!restored.session.admit_human_seats(std::vector<galactic::PlayerId>{9}), "an unlisted faction is refused");
        auto many = take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(), executor));
        expect(!many.session.admit_human_seats(std::vector<galactic::PlayerId>(7, 1)), "more than eight humans are refused");
        auto eight = take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(), executor));
        check(eight.session.admit_human_seats(std::vector<galactic::PlayerId>(6, 1)));
        expect(humans(eight.session) == 8, "eight humans in all are accepted");
    }
}

// The same bytes and the same extras give the same game on every peer and every worker count, through leave,
// the AI taking over from the last human, a rejoin by seat, and a save and load of the amended game.
void leave_rejoin_and_determinism() {
    const auto config = saved_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto bytes = saved_bytes(config, executor);
        auto game = resume(bytes, config, {1, 2}, executor).session; // seat 13 shares faction 1, seat 14 takes faction 2
        std::vector<std::string> hashes;
        const auto step = [&] { (void)take(game.step(executor)); hashes.push_back(game.state_sha256()); };
        const auto command = [&](const std::uint32_t seat, const std::uint64_t sequence, galactic::CommandPayload payload) {
            check(game.submit({{game.completed_tick(), {seat}, sequence}, std::move(payload)}));
        };
        step();
        // One of three humans on faction 1 leaves: the faction continues under the others.
        command(1, 1, galactic::SeatLeave{}); step();
        expect(controller(game, 1).kind == galactic::FactionControl::human && !seat_of(game, ai_seat_of(1))->active
            && game.galaxy().seat_control()->control.teams.front().lead.value == 2, "a shared faction continues when one human leaves");
        command(2, 1, galactic::SeatLeave{}); command(13, 1, galactic::SeatLeave{}); step();
        expect(controller(game, 1).kind == galactic::FactionControl::ai && seat_of(game, ai_seat_of(1))->active
            && game.galaxy().seat_control()->control.teams.front().lead.value == 0, "the AI takes the faction over when its last human leaves");
        // The admitted humans of faction 2 are one human side: the lone seat 14 leaves and the same AI wakes up.
        command(14, 1, galactic::SeatLeave{}); step();
        expect(controller(game, 2).kind == galactic::FactionControl::ai && seat_of(game, ai_seat_of(2))->active
            && controller(game, 2).configuration == std::vector<std::uint8_t>({1, 2}), "the frozen AI of a taken-over faction wakes up unchanged");
        command(14, 2, galactic::SeatRejoin{}); command(13, 2, galactic::SeatRejoin{}); step();
        expect(controller(game, 2).kind == galactic::FactionControl::human && !seat_of(game, ai_seat_of(2))->active
            && controller(game, 1).kind == galactic::FactionControl::human && !seat_of(game, ai_seat_of(1))->active,
            "a rejoin by seat ends the AI control again");
        step();
        const auto amended = take(cp::save_game_checkpoint(game, cp::game_identities(config)));
        auto loaded = take(cp::load_game_checkpoint(amended, cp::game_identities(config), load_options(), executor));
        expect(loaded.session.state_sha256() == game.state_sha256() && humans(loaded.session) == 4, "a save made after the admission loads with all its seats");
        expect(take(cp::save_game_checkpoint(loaded.session, cp::game_identities(config))) == amended, "its checkpoint bytes are canonical");
        auto reconstructed = take(cp::replay_game_checkpoint_for_testing(amended, cp::game_identities(config), load_options(), executor, bytes));
        expect(reconstructed.session.state_sha256() == game.state_sha256(),
            "replaying the amended game from its recording reproduces the admission");
        for (int tick = 0; tick < 3; ++tick) {
            step();
            (void)take(loaded.session.step(executor));
            expect(loaded.session.state_sha256() == hashes.back(), "the loaded amended game continues identically");
        }
        if (reference.empty()) reference = hashes;
        expect(hashes == reference, "every worker count gives the same hashes");
        auto twin = resume(bytes, config, {1, 2}, executor).session;
        (void)take(twin.step(executor));
        expect(twin.state_sha256() == reference.front(), "a second peer with the same bytes and extras reaches the same hash");
    }
}
// #7876: a player drops and the game is saved right after; the save keeps that human's seat (inactive), a returning or new player
// takes it back as the game loads, the seat is active again on every peer and worker count, a save made after that loads
// with it, and nothing but a departed human seat can be taken back.
void departed_seat_taken_back() {
    const auto config = saved_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(campaign::GameSession::create(config));
        for (int tick = 0; tick < 2; ++tick) (void)take(game.step(executor));
        check(game.submit({{game.completed_tick(), {2}, 1}, galactic::SeatLeave{}}));
        (void)take(game.step(executor));
        const auto bytes = take(cp::save_game_checkpoint(game, cp::game_identities(config)));
        auto restored = take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(), executor));
        expect(seat_of(restored.session, 2) && !seat_of(restored.session, 2)->active && seat_of(restored.session, 2)->kind == galactic::SeatKind::human,
            "the save keeps the departed human seat as a human seat");
        expect(!restored.session.reseat_human_seats(std::vector<galactic::SeatId>{galactic::SeatId{1}}), "an active seat cannot be taken back");
        expect(!restored.session.reseat_human_seats(std::vector<galactic::SeatId>{galactic::SeatId{ai_seat_of(1)}}), "an AI seat cannot be taken back");
        check(restored.session.reseat_human_seats(std::vector<galactic::SeatId>{galactic::SeatId{2}}));
        check(restored.session.admit_human_seats(std::vector<galactic::PlayerId>{1}));
        expect(seat_of(restored.session, 2)->active && seat_of(restored.session, 13) && humans(restored.session) == 3,
            "a joining player takes the departed seat back and another joins");
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 3; ++tick) { (void)take(restored.session.step(executor)); hashes.push_back(restored.session.state_sha256()); }
        const auto amended = take(cp::save_game_checkpoint(restored.session, cp::game_identities(config)));
        auto again = take(cp::load_game_checkpoint(amended, cp::game_identities(config), load_options(), executor));
        expect(again.session.state_sha256() == restored.session.state_sha256() && seat_of(again.session, 2)->active,
            "a save made after the seat was taken back loads with it");
        expect(take(cp::save_game_checkpoint(again.session, cp::game_identities(config))) == amended, "and is canonical");
        auto replayed = take(cp::replay_game_checkpoint_for_testing(amended, cp::game_identities(config), load_options(), executor, bytes));
        expect(replayed.session.state_sha256() == restored.session.state_sha256(), "replaying the recording reproduces the taken-back seat");
        {   // The smallest tail: only the take-back row, no joiner (a save row of 30 bytes must pass the decode bound).
            auto alone = take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(), executor));
            check(alone.session.reseat_human_seats(std::vector<galactic::SeatId>{galactic::SeatId{2}}));
            (void)take(alone.session.step(executor));
            const auto lone = take(cp::save_game_checkpoint(alone.session, cp::game_identities(config)));
            auto lone_loaded = take(cp::load_game_checkpoint(lone, cp::game_identities(config), load_options(), executor));
            expect(lone_loaded.session.state_sha256() == alone.session.state_sha256() && seat_of(lone_loaded.session, 2)->active
                && take(cp::save_game_checkpoint(lone_loaded.session, cp::game_identities(config))) == lone,
                "a save holding only a taken-back seat loads with it and is canonical");
        }
        if (reference.empty()) reference = hashes;
        expect(hashes == reference, "every worker count gives the same hashes");
    }
}

// A plain loaded save with no joiner: the faction continues when one of its two humans leaves, the AI takes it when the last
// leaves, and a rejoin by seat gives it back (the shared-faction and load-then-leave cases of the departure policy).
void load_then_leave() {
    const eawr::sim::InlineExecutor executor;
    const auto config = saved_setup();
    const auto bytes = saved_bytes(config, executor);
    auto game = resume(bytes, config, {}, executor).session;
    const auto submit = [&](const std::uint32_t seat, const std::uint64_t sequence, galactic::CommandPayload payload) {
        check(game.submit({{game.completed_tick(), {seat}, sequence}, std::move(payload)})); (void)take(game.step(executor));
    };
    submit(2, 1, galactic::SeatLeave{});
    expect(controller(game, 1).kind == galactic::FactionControl::human && game.galaxy().seat_control()->control.teams.front().lead.value == 1,
        "a loaded shared faction continues when one human leaves");
    submit(1, 1, galactic::SeatLeave{});
    expect(controller(game, 1).kind == galactic::FactionControl::ai && seat_of(game, ai_seat_of(1))->active,
        "a loaded faction goes to its AI when the last human leaves");
    submit(2, 2, galactic::SeatRejoin{});
    expect(controller(game, 1).kind == galactic::FactionControl::human && !seat_of(game, ai_seat_of(1))->active, "a rejoin by seat ends the AI control");
}

// The recorded window of an amended game: a command of an admitted seat is valid only with that seat's SeatAdmit at or before its
// tick. The replay validator sees commands and setup only; the session, which has the seat table, refuses the rest.
void command_before_admission() {
    const eawr::sim::InlineExecutor executor;
    const auto config = saved_setup();
    const auto bytes = saved_bytes(config, executor);
    auto game = resume(bytes, config, {1}, executor).session; // seat 13 shares faction 1
    check(game.submit({{game.completed_tick(), {13}, 1}, galactic::SeatLeave{}}));
    (void)take(game.step(executor)); (void)take(game.step(executor));
    const auto recording = take(game.checkpoint_recording());
    const auto fresh = [&] { return take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(), executor)).session; };
    const auto is_admission = [](const galactic::GameCommand& command) { return std::holds_alternative<galactic::SeatAdmit>(command.payload); };
    expect(std::any_of(recording.load_fixups.begin(), recording.load_fixups.end(), is_admission)
        && !recording.commands.commands.empty() && recording.commands.commands.front().key.seat.value == 13, "the window holds the admission and the command");
    auto honest = fresh();
    expect(static_cast<bool>(honest.append_recording_window(recording)), "the honest window appends onto the same save");
    auto stripped = recording;
    std::erase_if(stripped.load_fixups, is_admission);
    auto without = fresh();
    const auto refused = without.append_recording_window(stripped);
    expect(!refused && refused.error().code == galactic::diagnostic_codes::replay_command, "a command of a seat that no SeatAdmit creates is refused");
    auto late = recording;
    for (auto& fixup : late.load_fixups) if (is_admission(fixup)) fixup.key.tick = recording.commands.commands.front().key.tick + 1;
    auto after = fresh();
    expect(!after.append_recording_window(late), "a command before its seat's admission is refused");
}

// Three peers play the amended game in lockstep: two saved humans share faction 1, a joiner shares it too (seat 13) and one
// takes over faction 2 (seat 14). Same-tick commands from the shared seats, a leave that leaves the faction to the others,
// the last human leaving so the AI takes over, and a rejoin by seat: every peer hashes the same on 1/2/4/8 workers.
void amended_lockstep() {
    const auto config = saved_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto bytes = saved_bytes(config, executor);
        const auto tick = 3ULL;
        const auto fixed = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
        const std::vector<galactic::GameCommand> commands{
            {{tick + 1, {1}, 1}, galactic::SetSpeed{false, fixed(1)}}, {{tick + 1, {13}, 1}, galactic::SetSpeed{false, fixed(2)}},
            {{tick + 2, {2}, 1}, galactic::SeatLeave{}},
            {{tick + 3, {1}, 2}, galactic::SeatLeave{}}, {{tick + 3, {13}, 2}, galactic::SeatLeave{}},
            {{tick + 4, {14}, 1}, galactic::SeatLeave{}},
            {{tick + 6, {13}, 3}, galactic::SeatRejoin{}}, {{tick + 6, {14}, 2}, galactic::SeatRejoin{}}};
        std::vector<campaign::GameSession> peers;
        for (std::uint32_t index = 0; index < 3; ++index) peers.push_back(resume(bytes, config, {1, 2}, executor).session);
        std::vector<std::string> hashes;
        // Every peer runs the same agreed command list in (tick, seat, sequence) order and must hash the same after every tick.
        for (std::uint64_t completed = tick; completed < 14; ++completed) {
            for (auto& peer : peers)
                for (const auto& command : commands)
                    if (command.key.tick == completed) check(peer.submit(command));
            for (auto& peer : peers) (void)take(peer.step(executor));
            for (const auto& peer : peers)
                expect(peer.state_sha256() == peers.front().state_sha256(), "every peer hashes the same after every tick");
            hashes.push_back(peers.front().state_sha256());
        }
        for (const auto& peer : peers) {
            expect(controller(peer, 1).kind == galactic::FactionControl::human && controller(peer, 2).kind == galactic::FactionControl::human,
                "after the rejoins both factions are human again on every peer");
            expect(humans(peer) == 4, "four human seats on every peer");
        }
        if (reference.empty()) reference = hashes;
        expect(hashes == reference, "every worker count gives the same hashes");
    }
}
} // namespace
int case_galactic_seat_admission_tests() {
    try { shared_seat_and_takeover(); refusals(); leave_rejoin_and_determinism(); departed_seat_taken_back(); load_then_leave(); command_before_admission(); amended_lockstep(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}

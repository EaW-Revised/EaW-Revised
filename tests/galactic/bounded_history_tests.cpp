// #4058: a GC save keeps the history since the previous save or load, and its input frontier chains through it.
#include "galactic_support.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/checkpoint/game_session_internal.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace galactic_test;
namespace cp = eawr::checkpoint;
namespace campaign = eawr::campaign;
using Bytes = std::vector<std::uint8_t>;

template <class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
void check(eawr::core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}
galactic::GalacticSetup setup() {
    galactic::SyntheticGalaxyOptions spec; spec.planets = 8; spec.players = 2;
    spec.fleets_per_player = 1; spec.members_per_fleet = 2; spec.debug_commands = true;
    auto result = galactic::synthetic_galaxy(spec);
    result.seats[1].kind = galactic::SeatKind::ai; result.seats[1].human_side_id = 0;
    return result;
}

constexpr std::uint64_t first_save = 10, second_save = 30, third_save = 50;
// Eight human commands, three of them saves. Everything is submitted up front, as a lobby or a queued input would.
void submit_play(campaign::GameSession& game, const galactic::GalacticSetup& config) {
    const auto a = config.planets[0].id, b = config.planets[1].id;
    std::uint64_t sequence = 0;
    const auto put = [&](std::uint64_t tick, galactic::CommandPayload payload) {
        check(game.submit({{tick, {1}, ++sequence}, std::move(payload)}));
    };
    put(3, galactic::DebugSetPlanetOwner{a, 2});
    put(first_save, galactic::SaveRequest{1, false, false, false});
    put(14, galactic::DebugSetPlanetOwner{a, 1});
    put(20, galactic::DebugSetPlanetOwner{b, 2});
    put(second_save, galactic::SaveRequest{2, false, false, false});
    put(40, galactic::DebugSetPlanetOwner{b, 1});
    put(third_save, galactic::SaveRequest{3, false, false, false});
    put(60, galactic::DebugSetPlanetOwner{a, 2});
}

struct Lineage {
    std::vector<std::string> hashes;
    Bytes first, second, third;
};
void run_to(campaign::GameSession& game, std::uint64_t tick, const eawr::sim::PartitionExecutor& pool, Lineage& out) {
    while (game.completed_tick() < tick) out.hashes.push_back(take(game.step(pool)).state_hash.get());
}

Lineage uninterrupted(const galactic::GalacticSetup& config, const eawr::sim::PartitionExecutor& pool) {
    const auto identities = cp::game_identities(config);
    Lineage out;
    auto game = take(campaign::GameSession::create(config));
    submit_play(game, config);
    run_to(game, first_save + 1, pool, out); out.first = take(cp::save_game_checkpoint(game, identities));
    run_to(game, second_save + 1, pool, out); out.second = take(cp::save_game_checkpoint(game, identities));
    run_to(game, third_save + 1, pool, out); out.third = take(cp::save_game_checkpoint(game, identities));
    run_to(game, 70, pool, out);
    return out;
}
// The same play, but the game is reloaded from each save it just wrote.
Lineage reloaded(const galactic::GalacticSetup& config, const eawr::sim::PartitionExecutor& pool) {
    const auto identities = cp::game_identities(config);
    Lineage out;
    auto game = take(campaign::GameSession::create(config));
    submit_play(game, config);
    run_to(game, first_save + 1, pool, out); out.first = take(cp::save_game_checkpoint(game, identities));
    auto loaded = take(cp::load_game_checkpoint(out.first, identities, {}, pool));
    expect(loaded.work.resimulated_ticks == 0, "the first load restores directly");
    expect(take(cp::save_game_checkpoint(loaded.session, identities)) == out.first, "a load recaptures its save before it steps");
    run_to(loaded.session, second_save + 1, pool, out); out.second = take(cp::save_game_checkpoint(loaded.session, identities));
    loaded = take(cp::load_game_checkpoint(out.second, identities, {}, pool));
    expect(take(cp::save_game_checkpoint(loaded.session, identities)) == out.second, "the second load recaptures its save before it steps");
    run_to(loaded.session, third_save + 1, pool, out); out.third = take(cp::save_game_checkpoint(loaded.session, identities));
    loaded = take(cp::load_game_checkpoint(out.third, identities, {}, pool));
    run_to(loaded.session, 70, pool, out);
    return out;
}

cp::Checkpoint parsed(const Bytes& save) { return take(cp::parse_checkpoint(save)); }
Bytes window_replay(const Bytes& save) {
    const auto checkpoint = parsed(save);
    eawr::checkpoint::detail::Reader in(checkpoint.find(cp::SectionId::session)->bytes);
    const auto prefix = cp::detail::read_session_prefix(in);
    return Bytes(prefix.replay.begin(), prefix.replay.end());
}
eawr::campaign::HistoryBase history_base(const Bytes& save) {
    const auto checkpoint = parsed(save);
    eawr::checkpoint::detail::Reader in(checkpoint.find(cp::SectionId::session)->bytes);
    return cp::detail::read_session_prefix(in).base;
}
std::size_t window_commands(const Bytes& save) { return take(galactic::parse_replay(window_replay(save))).commands.size(); }
cp::Digest chain(const cp::Digest& previous, const Bytes& window) {
    Bytes input(previous.begin(), previous.end());
    input.insert(input.end(), window.begin(), window.end());
    return eawr::core::sha256(input);
}

void frontier_chain(const galactic::GalacticSetup& config) {
    const eawr::sim::InlineExecutor serial;
    const auto reference = uninterrupted(config, serial);
    const auto first = parsed(reference.first), second = parsed(reference.second), third = parsed(reference.third);
    expect((first.header.flags & cp::flag_frontier) && (second.header.flags & cp::flag_frontier), "saves with sealed commands carry a frontier");
    // The link is the digest of the previous link and the window bytes: never the window alone.
    expect(first.header.frontier.input_digest == chain(cp::Digest{}, window_replay(reference.first)), "the first window chains from the zero digest");
    expect(second.header.frontier.input_digest == chain(first.header.frontier.input_digest, window_replay(reference.second)),
        "the second frontier digest is sha256(previous frontier digest || window bytes)");
    expect(third.header.frontier.input_digest == chain(second.header.frontier.input_digest, window_replay(reference.third)),
        "the third frontier digest chains from the second");
    expect(second.header.frontier.input_digest != eawr::core::sha256(window_replay(reference.second)), "a window digest alone is not the frontier");
    expect(history_base(reference.second).at.tick == first_save + 1 && history_base(reference.third).at.tick == second_save + 1,
        "each save's history base is the previous save's barrier");
    expect(history_base(reference.second).at.digest == first.header.frontier.input_digest, "the base carries the previous frontier digest");
    // History is bounded by play since the last save.
    expect(window_commands(reference.first) == 8 && window_commands(reference.second) == 6 && window_commands(reference.third) == 3,
        "each save carries the commands since the previous save (8, then 6 sealed since tick 11, then 3 since tick 31)");
    expect(second.header.frontier.sequence == 8 && third.header.frontier.sequence == 8, "the frontier key survives the dropped history");

    // Chain independence from loads: reloading at every save ends on the same frontiers, bytes and per-tick hashes.
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        const auto other = reloaded(config, pool);
        const auto again = uninterrupted(config, pool);
        expect(again.hashes == reference.hashes, "per-tick hashes are equal across worker counts");
        expect(other.hashes == reference.hashes, "per-tick hashes are equal with and without save/load cycles");
        expect(other.first == reference.first && other.second == reference.second && other.third == reference.third,
            "a save -> load -> save chain writes the same saves as the uninterrupted run");
        expect(parsed(other.third).header.frontier == third.header.frontier, "the frontier does not depend on whether a load happened");
    }
}

void bounded_growth(const galactic::GalacticSetup& config) {
    // Many commands per cycle: the saved window follows the play since the last save, not the whole game.
    const auto identities = cp::game_identities(config); const eawr::sim::InlineExecutor serial;
    auto game = take(campaign::GameSession::create(config));
    std::uint64_t sequence = 0;
    std::vector<std::size_t> windows, owners;
    for (int cycle = 0; cycle < 4; ++cycle) {
        for (int i = 0; i < 20; ++i) {
            const auto tick = game.completed_tick();
            check(game.submit({{tick, {1}, ++sequence}, galactic::DebugSetPlanetOwner{config.planets[0].id, static_cast<galactic::PlayerId>(1 + (sequence % 2))}}));
            (void)take(game.step(serial));
        }
        check(game.submit({{game.completed_tick(), {1}, ++sequence}, galactic::SaveRequest{cycle + 1, false, false, false}}));
        (void)take(game.step(serial));
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        windows.push_back(window_commands(saved));
        owners.push_back(parsed(saved).find(cp::SectionId::galactic_owner)->bytes.size());
        auto loaded = take(cp::load_game_checkpoint(saved, identities, {}, serial));
        game = std::move(loaded.session);
    }
    expect(windows[0] == 21 && windows[1] == 21 && windows[2] == 21 && windows[3] == 21,
        "every cycle's save holds only its own 21 commands");
    expect(owners[3] <= owners[1] + 64, "the owner section does not grow with the number of earlier cycles");
    expect(game.history_base().executed_dropped >= 63, "the dropped count keeps the absolute command total");
}

void replay_oracle(const galactic::GalacticSetup& config) {
    const eawr::sim::InlineExecutor serial; const auto identities = cp::game_identities(config);
    const auto run = uninterrupted(config, serial);
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto genesis = take(cp::replay_game_checkpoint_for_testing(run.first, identities, {}, pool));
        auto direct = take(cp::load_game_checkpoint(run.second, identities, {}, pool));
        auto windowed = take(cp::replay_game_checkpoint_for_testing(run.second, identities, {}, pool, run.first));
        expect(windowed.work.resimulated_ticks == second_save - first_save, "the oracle replays only the window after the base");
        expect(windowed.session.state_sha256() == direct.session.state_sha256(), "window replay equals the direct load");
        expect(windowed.session.galaxy().canonical_state_bytes() == direct.session.galaxy().canonical_state_bytes(),
            "window replay equals the direct load component by component");
        auto third = take(cp::replay_game_checkpoint_for_testing(run.third, identities, {}, pool, run.second));
        expect(third.session.completed_tick() == third_save + 1, "the oracle chains from the previous save");
        (void)genesis;
    }
    expect(!cp::replay_game_checkpoint_for_testing(run.second, identities, {}, serial), "a bounded save needs its base");
    expect(!cp::replay_game_checkpoint_for_testing(run.third, identities, {}, serial, run.first),
        "the oracle refuses a base that is not the save it names");
}

void old_saves(const galactic::GalacticSetup& config) {
    const eawr::sim::InlineExecutor serial; const auto identities = cp::game_identities(config);
    const auto run = uninterrupted(config, serial);
    auto container = parsed(run.first);
    for (auto& section : container.sections)
        if (section.version == cp::galaxy_section_version) section.version = 33;
    const auto old = take(cp::write_checkpoint(container));
    for (const auto direct : {true, false}) {
        const auto refused = direct ? cp::load_game_checkpoint(old, identities, {}, serial).has_value()
            : cp::replay_game_checkpoint_for_testing(old, identities, {}, serial).has_value();
        expect(!refused, "a section version 33 save does not load");
    }
    const auto result = cp::load_game_checkpoint(old, identities, {}, serial);
    expect(!result && result.error().code == cp::diagnostic_codes::version, "an old save fails with the named version code");
    expect(!result && result.error().message.find("33") != std::string::npos && result.error().message.find("35") != std::string::npos,
        "the diagnostic names the found and expected versions");
}

// A native journal window: the dropped entries keep the absolute sequence and the limits.
void native_window() {
    galactic::SyntheticGalaxyOptions spec; spec.planets = 8; spec.players = 2;
    spec.fleets_per_player = 2; spec.members_per_fleet = 2; spec.debug_commands = true;
    auto config = galactic::synthetic_galaxy(spec);
    config.rules.maximum_fleet_movement_distance = take(eawr::sim::math::Fixed::from_integer(10000));
    for (auto& fleet : config.fleets) for (const auto& planet : config.planets)
        if (planet.owner == fleet.owner) { fleet.orbit = planet.id; break; }
    config.fleet_control.emplace();
    std::set<galactic::TypeId> types;
    for (const auto& fleet : config.fleets) for (const auto& member : fleet.members) types.insert(member.type);
    for (const auto type : types) config.fleet_control->types.push_back({type, false, true, false});
    const auto identities = cp::game_identities(config); const eawr::sim::InlineExecutor serial;
    auto game = take(campaign::GameSession::create(config)); galactic::GalacticSession* world{};
    check(game.configure_native_replay_owners([&](auto& owner) { world = &owner; return eawr::core::Result<void>::success(); }));
    const auto& fleet = config.fleets.front();
    for (int i = 0; i < 4; ++i) (void)take(game.step(serial));
    expect(take(world->admit_native_fleet(fleet.owner, galactic::RemoveFleetMembers{fleet.id, {fleet.members.front().id}})).admitted,
        "the fixture journals a native fleet mutation");
    check(game.submit({{game.completed_tick(), {1}, 1}, galactic::SaveRequest{1, false, false, false}}));
    (void)take(game.step(serial));
    const auto first = take(cp::save_game_checkpoint(game, identities));
    expect(take(galactic::parse_replay(window_replay(first))).native_effects.size() == 1, "the first save holds the journal");
    (void)take(game.step(serial));
    expect(game.galaxy().native_effects().empty() && game.galaxy().native_dropped() == 1, "the next step drops the journal and counts it");
    expect(game.history_base().native_dropped == 1, "the base counts the dropped journal");
    expect(take(world->admit_native_fleet(fleet.owner, galactic::RemoveFleetMembers{fleet.id, {fleet.members.back().id}})).admitted,
        "a second native mutation after the rebase is admitted");
    expect(game.galaxy().native_effects().size() == 1 && game.galaxy().native_effects().front().sequence == 2,
        "the new entry keeps its absolute sequence");
    check(game.submit({{game.completed_tick(), {1}, 2}, galactic::SaveRequest{2, false, false, false}}));
    (void)take(game.step(serial));
    const auto second = take(cp::save_game_checkpoint(game, identities));
    const auto window = take(galactic::parse_replay(window_replay(second)));
    expect(window.native_effects.size() == 1 && window.native_effects.front().sequence == 1,
        "the saved replay numbers its window from 1, as the replay format requires");
    auto loaded = take(cp::load_game_checkpoint(second, identities, {}, serial));
    expect(loaded.session.galaxy().native_dropped() == 1 && loaded.session.galaxy().native_effects().size() == 1
        && loaded.session.galaxy().native_effects().front().sequence == 2, "a load adopts the window at its absolute sequence");
    expect(take(cp::save_game_checkpoint(loaded.session, identities)) == second, "the loaded window recaptures byte-identically");
    auto replayed = take(cp::replay_game_checkpoint_for_testing(second, identities, {}, serial, first));
    expect(replayed.session.state_sha256() == loaded.session.state_sha256(), "the oracle replays a native window on its base");
    for (int i = 0; i < 20; ++i)
        expect(take(game.step(serial)).state_hash.get() == take(loaded.session.step(serial)).state_hash.get(),
            "the loaded session continues with the original's hashes");
}
} // namespace

int case_galactic_bounded_history_tests(int, char**) {
    try {
        const auto config = setup();
        frontier_chain(config);
        bounded_growth(config);
        replay_oracle(config);
        old_saves(config);
        native_window();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}

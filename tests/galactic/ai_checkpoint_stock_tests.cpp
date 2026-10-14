// WGA-56/139 supporting native boundary contracts. The composed installed
// campaign opponent/victory proof is a separate acceptance, not this fixture.
#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {
using namespace galactic_test;
namespace ai = eawr::script::foc::galactic_ai;
namespace cp = eawr::checkpoint;
namespace campaign = eawr::campaign;
namespace platform = eawr::platform;
namespace tactical = eawr::sim::tactical;
namespace funding = galactic::funding;
using Fixed = eawr::sim::math::Fixed;
using Bytes = std::vector<std::uint8_t>;
using namespace std::chrono_literals;
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
void take(eawr::core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
Fixed f(std::int64_t value) { return take(Fixed::from_integer(value)); }
ai::FundingState funds() {
    funding::Ledger ledger;
    ledger = take(funding::set_equation(std::move(ledger), 4, f(1), -3));
    ledger = take(funding::set_allowance(std::move(ledger), 4, take(Fixed::from_decimal("0.25")), 17));
    ledger = take(funding::initialize(std::move(ledger), f(500)));
    ledger = take(funding::reserve(std::move(ledger), {9, 4, f(30), f(10), {}, {}}));
    ledger = take(funding::mark_spending(std::move(ledger), 9, f(12)));
    ledger = take(funding::remove_goal(std::move(ledger), 9));
    ledger = take(funding::reserve(std::move(ledger), {10, 4, f(5), f(2), {}, {}}));
    ledger.last_interval = Fixed::from_raw(12345); ledger.equations_changed = true;
    return {{1, std::move(ledger)}};
}
class FixtureState final : public ai::StateContributor {
public:
    ai::FundingState state = funds();
    unsigned restored{}, entered{}, returned{};
    std::optional<campaign::BattleResult> observed_return;
    eawr::core::Result<ai::NativeState> capture() const override {
        auto bytes = ai::encode_funding_state(state);
        if (!bytes) return eawr::core::Result<ai::NativeState>::failure(bytes.error());
        ai::NativeState value; value.components["funding"] = std::move(bytes).value();
        return eawr::core::Result<ai::NativeState>::success(std::move(value));
    }
    eawr::core::Result<void> restore(const ai::NativeState& value) override {
        if (value.version != 1 || value.components.size() != 1 || !value.components.contains("funding"))
            return refusal("unsupported fixture state");
        auto decoded = ai::decode_funding_state(value.components.at("funding"));
        if (!decoded) return eawr::core::Result<void>::failure(decoded.error());
        state = std::move(decoded).value(); ++restored;
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<std::string> state_hash() const override {
        auto value = capture(); if (!value) return eawr::core::Result<std::string>::failure(value.error());
        return ai::native_state_hash(value.value());
    }
    static eawr::core::Result<void> refusal(std::string message) {
        eawr::core::Diagnostic error; error.code = "EAWR-TEST-NATIVE"; error.message = std::move(message);
        return eawr::core::Result<void>::failure(std::move(error));
    }
    campaign::GalacticAiServices services() {
        return {{}, [this](const campaign::GalacticAiBattleNotice& notice) {
            auto& ledger = state.at(1);
            if (notice.kind == campaign::GalacticAiBattleKind::entered) {
                ++entered; expect(!notice.result, "entry has no fabricated result");
                ledger.tactical = {true, Fixed::from_raw(117), Fixed::from_raw(13)};
            } else {
                ++returned; expect(notice.result && notice.result->conflict == notice.conflict.id,
                    "committed battle result reaches retained plan hook");
                if (notice.result) observed_return = *notice.result;
                ledger.tactical.switched = false; ledger.tactical.extra_released = Fixed::from_raw(19);
            }
            return eawr::core::Result<void>::success();
        }};
    }
};
void install(campaign::GameSession& game, const std::shared_ptr<FixtureState>& fixture) {
    auto services = fixture->services(); services.state = fixture; take(game.set_galactic_ai_services(std::move(services)));
}
void codecs() {
    auto values = funds();
    auto& category = values.at(1).categories.at(4);
    category.previous_share = Fixed::from_raw(1); category.income = Fixed::from_raw(2);
    category.available_income = Fixed::from_raw(3);
    values.at(1).goals.at(10).reserved_income = Fixed::from_raw(4);
    values.at(1).goals.at(10).reassessed = true;
    values.at(1).tactical = {true, Fixed::from_raw(5), Fixed::from_raw(6)};
    const auto bytes = take(ai::encode_funding_state(values));
    expect(take(ai::decode_funding_state(bytes)) == values, "complete fractional ledger including dead obligations and switch round-trips");
    ai::NativeState state; state.components["funding"] = bytes; state.components["GALACTIC_PERCEPTION"] = {1, 0, 0, 0};
    const auto encoded = take(ai::encode_native_state(state));
    auto decoded = take(ai::decode_native_state(encoded));
    expect(decoded.version == 1 && decoded.components == state.components, "canonical component envelope preserves bytes and ASCII keys");
    expect(take(ai::encode_native_state(decoded)) == encoded, "native round-trip byte identity");
    const auto hash = take(ai::native_state_hash(state));
    expect(hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos, "native digest lowercase SHA256");
    for (auto* field : {&category.allowance, &category.share, &category.previous_share, &category.cash,
        &category.available_cash, &category.income, &category.available_income}) {
        const auto before = *field; *field = Fixed::from_raw(before.raw() + 1);
        expect(take(ai::encode_funding_state(values)) != bytes, "each retained category balance changes encoded state"); *field = before;
    }
    auto damaged = encoded; damaged.push_back(0); expect(!ai::decode_native_state(damaged), "native trailing bytes refused");
    damaged = encoded; damaged[4] ^= 1; expect(!ai::decode_native_state(damaged), "native domain mismatch refused");
    for (std::size_t size = 0; size < encoded.size(); ++size)
        expect(!ai::decode_native_state(std::span(encoded).first(size)), "native truncation refused");
    auto invalid = state; invalid.version = 2; expect(!ai::encode_native_state(invalid), "unknown version refused");
    invalid = state; invalid.components["bad key"] = {1}; expect(!ai::encode_native_state(invalid), "invalid component key refused");
    damaged = bytes; damaged.push_back(0); expect(!ai::decode_funding_state(damaged), "funding trailing bytes refused");
    damaged = bytes; damaged.back() = 255; // Raw Fixed words are preserved; all bit patterns are valid.
    expect(bool(ai::decode_funding_state(damaged)), "codec preserves signed raw Fixed words without float conversion");
    auto boundary = take(campaign::encode_galactic_ai_save({11, 7, state}));
    const auto saved = take(campaign::decode_galactic_ai_save(boundary));
    expect(saved.completed_tick == 11 && saved.galactic_frame == 7 && saved.state->components == state.components,
        "distinct synchronization and mode clocks persisted");
    boundary[16] = 2; expect(!campaign::decode_galactic_ai_save(boundary), "nonboolean native presence refused");
}
galactic::GalacticSetup setup() {
    return galactic::synthetic_galaxy({.seed = 71, .planets = 8, .players = 2,
        .fleets_per_player = 1, .members_per_fleet = 1, .debug_commands = true});
}
campaign::ConflictRecord conflict(const galactic::GalacticSetup& config) {
    campaign::ConflictRecord record; record.id = take(galactic::GalacticSession::create(config)).next_entity_id();
    record.planet = config.planets.front().id; record.invader = 1; record.defender = 2;
    record.tactical.content_identity = config.content_identity; record.tactical.first_free_entity_id = record.id + 20;
    record.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    for (const auto& fleet : config.fleets) {
        const auto& member = fleet.members.front(); record.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        tactical::UnitState unit; unit.entity_id = member.id; unit.type_id = member.type; unit.owner = fleet.owner;
        record.tactical.units.push_back(unit);
    }
    for (const auto& seat : config.seats) record.seats.push_back({seat.seat_id, seat.player});
    return record;
}
tactical::TacticalContent content(const campaign::ConflictRecord& record) {
    tactical::TacticalContent value;
    for (const auto& force : record.forces) {
        if (std::any_of(value.durability.profiles.begin(), value.durability.profiles.end(), [&](const auto& row) { return row.type_id == force.type; })) continue;
        tactical::DurabilityProfile row; row.type_id = force.type; row.max_hull = f(100); value.durability.profiles.push_back(row);
    }
    value.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    value.victory.contenders = {1, 2}; value.victory.controlled_players = {1, 2}; value.victory.humans = {1}; value.victory.countdown_frames = 2;
    for (const auto& row : value.durability.profiles) value.victory.relevant_types.push_back(row.type_id);
    return value;
}
galactic::TacticalLayerCommand damage(const campaign::ConflictRecord& record) {
    tactical::TacticalReplay replay; replay.setup.players = record.tactical.players; replay.final_tick_count = 1;
    replay.commands.push_back({{0, 1, 1}, {record.forces.back().id}, tactical::DamagePayload{f(1000)}});
    const auto bytes = take(tactical::write_replay(replay));
    const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
    const auto start = header + replay.setup.players.size() * 24 + 4;
    return {record.id, bytes[start + 20], Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end())};
}
cp::GameLoadOptions load_options(std::shared_ptr<FixtureState>& restored) {
    cp::GameLoadOptions options;
    options.content = [](const auto& record) { return eawr::core::Result<tactical::TacticalContent>::success(content(record)); };
    options.configure_native_ai = [&restored](auto& game) {
        restored = std::make_shared<FixtureState>(); install(game, restored);
        return eawr::core::Result<void>::success();
    };
    return options;
}
void checkpoint_boundaries(std::size_t workers, std::vector<std::string>& reference) {
    const platform::ThreadWorkerAdapter pool(workers); const auto config = setup(); const auto record = conflict(config);
    auto game = take(campaign::GameSession::create(config)); auto native = std::make_shared<FixtureState>(); install(game, native);
    if (workers > 1) game.set_state_hasher(std::make_shared<platform::ThreadStateHasher>());
    const auto plain = take(campaign::GameSession::create(config)).state_sha256();
    expect(game.state_sha256() != plain, "native state contributes before the first service");
    auto boundary = [&]() {
        const auto identities = cp::game_identities(config); const auto bytes = take(cp::save_game_checkpoint(game, identities));
        std::shared_ptr<FixtureState> restored;
        auto options = load_options(restored);
        auto loaded = take(cp::load_game_checkpoint(bytes, identities, options, pool));
        expect(restored && restored.get() != native.get() && restored->restored == 1,
            "fresh contributor restored once after reconstruction");
        expect(restored->entered == 0 && restored->returned == 0, "reconstruction does not replay native battle listeners");
        expect(restored->state == native->state && loaded.session.state_sha256() == game.state_sha256(), "native boundary exact state and hash");
        expect(take(cp::save_game_checkpoint(loaded.session, identities)) == bytes, "native boundary recapture byte identity");
        expect(loaded.work.resimulated_ticks == 0, "native direct restore performs zero historical steps");
        std::shared_ptr<FixtureState> direct_native, replay_native;
        auto direct = take(cp::load_game_checkpoint(bytes, identities, load_options(direct_native), pool));
        auto replayed = take(cp::replay_game_checkpoint_for_testing(bytes, identities, load_options(replay_native), pool));
        expect(direct.session.galaxy().canonical_state_bytes() == replayed.session.galaxy().canonical_state_bytes(),
            "native saved components equal replay restoration");
        for (int i = 0; i < 90; ++i) {
            const auto a = take(direct.session.step(pool)), b = take(replayed.session.step(pool));
            expect(a.state_hash.get() == b.state_hash.get() && a.mode == b.mode
                && a.conflict_events == b.conflict_events && a.save_events == b.save_events,
                "native/child/return direct and replay paths continue identically for 90 ticks");
            expect(direct.session.galaxy().canonical_state_bytes() == replayed.session.galaxy().canonical_state_bytes()
                && direct_native->state == replay_native->state, "native continued components are equal");
        }
        for (int i = 0; i < 5; ++i) {
            const auto a = take(loaded.session.step(pool)), b = take(game.step(pool));
            expect(a.state_hash.get() == b.state_hash.get() && a.conflict_events == b.conflict_events,
                "loaded continuation equals uninterrupted control at the same saved boundary");
        }
        expect(restored->state == native->state, "uninterrupted and loaded native ledgers continue identically");
        auto missing = options; missing.configure_native_ai = {};
        const auto retained = loaded.session.state_sha256();
        expect(!cp::restore_game_checkpoint(loaded, bytes, identities, missing, pool) && loaded.session.state_sha256() == retained,
            "missing native resolver leaves running owner untouched");
        auto container = take(cp::parse_checkpoint(bytes));
        auto& section = *std::find_if(container.sections.begin(), container.sections.end(), [](const auto& row) { return row.id == 14; });
        section.bytes[0] ^= 1;
        expect(!cp::load_game_checkpoint(take(cp::write_checkpoint(container)), identities, options, pool), "mismatched native clock refused before reconstruction");
    };
    boundary(); // reservation and dead-goal marked obligation
    take(game.stage_battle(record, content(record)));
    std::vector<std::string> hashes;
    hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(native->entered == 1 && native->state.at(1).tactical.switched, "real child activation notifies retained native state once");
    boundary(); // active zero-tick child
    take(game.submit({{game.completed_tick(), {1}, 1}, damage(record)}));
    hashes.push_back(take(game.step(pool)).state_hash.get());
    boundary(); // active progressed child
    for (int i = 0; game.mode() == campaign::SessionMode::battle && i < 20; ++i)
        hashes.push_back(take(game.step(pool)).state_hash.get());
    expect(game.mode() == campaign::SessionMode::galaxy && native->returned == 1 && !native->state.at(1).tactical.switched,
        "actual casualty/victory returns once into retained native listener");
    expect(game.latest_tactical_result() && native->observed_return == *game.latest_tactical_result(),
        "retained listener observes the complete final tactical result");
    boundary(); // committed return
    hashes.push_back(take(game.step(pool)).state_hash.get());
    if (reference.empty()) reference = hashes; else expect(hashes == reference, "native battle sequence equal on 1/2/4/8 workers");
}
void transit_boundary(std::size_t workers) {
    galactic::GalacticSetup config;
    config.players = {{1, 1, 0}, {2, 2, 0}};
    config.seats = {{{1}, 1, galactic::SeatKind::human, 0, true, 1}, {{2}, 2, galactic::SeatKind::ai, 1, true, 0}};
    config.planets = {{1, {}, 1}, {2, {f(120), {}, {}}, 1}};
    config.fleets = {{10, 1, 1, {{20, 1}}}};
    config.rules.maximum_fleet_movement_distance = f(120);
    config.fleet_control.emplace(); config.fleet_control->types = {{1, false, true, false}};
    config.movement.emplace(); auto& movement = *config.movement; movement.mode_modifier = f(1);
    movement.types = {{1, f(2), f(100), 0, true, false, false, false}};
    movement.players = {{1, f(1), {}, false}, {2, f(1), {}, false}};
    movement.planets = {{1, f(1), false}, {2, f(1), false}};
    for (const auto& player : config.players) for (const auto& planet : config.planets)
        movement.access.push_back({player.player_id, planet.id, true, false});
    const platform::ThreadWorkerAdapter pool(workers);
    auto game = take(campaign::GameSession::create(config)); auto native = std::make_shared<FixtureState>(); install(game, native);
    take(game.submit({{0, {1}, 1}, galactic::MoveFleet{10, {1, 2}, {}}}));
    (void)take(game.step(pool));
    expect(game.galaxy().snapshot()->fleets()[0].motion.transit.has_value(), "native reservation save uses an actual in-transit fleet");
    const auto bytes = take(cp::save_game_checkpoint(game, cp::game_identities(config)));
    std::shared_ptr<FixtureState> loaded_native;
    auto restored = take(cp::load_game_checkpoint(bytes, cp::game_identities(config), load_options(loaded_native), pool));
    expect(restored.work.resimulated_ticks == 0, "in-transit native owner restores without historical steps");
    for (int i = 0; i < 90; ++i)
        expect(take(game.step(pool)).state_hash.get() == take(restored.session.step(pool)).state_hash.get(),
            "in-transit fleet and retained funding continue identically");
}
void auto_return(std::size_t workers) {
    const platform::ThreadWorkerAdapter pool(workers);
    auto config = setup(); for (auto& fleet : config.fleets) fleet.orbit = config.planets.front().id;
    const auto record = conflict(config);
    auto game = take(campaign::GameSession::create(config)); auto native = std::make_shared<FixtureState>(); install(game, native);
    game.set_conflict_service([](const auto&, const auto&, auto) {
        return eawr::core::Result<std::optional<campaign::RecordedAdmission>>::success({});
    }, [](const auto& row) { return eawr::core::Result<tactical::TacticalContent>::success(content(row)); });
    // Explicit fixture outcome: tests the existing journal's confirmed return,
    // not installed auto-resolve combat/AI or terminal campaign acceptance.
    game.set_auto_resolver([](const auto& row, const auto&, const auto&) {
        campaign::AutoResolveOutcomeApplication result; result.planet = row.planet; result.planet_type = "FixturePlanet";
        result.invader = row.invader; result.defender = row.defender; result.result.conflict = row.id; result.result.winner = row.invader;
        for (const auto& force : row.forces) {
            result.result.forces.push_back({force.id, force.owner == row.defender ? campaign::ForceDisposition::destroyed : campaign::ForceDisposition::survived});
            if (force.owner == row.defender) result.destroyed.push_back(force.id);
        }
        return eawr::core::Result<std::optional<campaign::AutoResolveOutcomeApplication>>::success(std::move(result));
    });
    take(game.queue_conflict(record, {campaign::ConflictRoute::instant_auto}));
    (void)take(game.step(pool)); const auto resolved = take(game.step(pool));
    expect(resolved.result.has_value() && native->returned == 1 && native->entered == 0,
        "one committed auto-result notifies retained plans without claiming a tactical child");
    (void)take(game.step(pool)); expect(native->returned == 1, "auto-result notification never repeats on a later parent service");
}
void live_save(std::size_t workers, const std::filesystem::path& directory) {
    platform::LiveGameSession::Options options; options.workers = workers;
    options.pacing = platform::LiveGameSession::Pacing::driven; options.saves.directory = directory.string();
    auto configuration = std::make_shared<platform::CampaignServiceConfiguration>();
    unsigned created{};
    configuration->configure_native_ai = [&created](auto& game) {
        ++created; auto state = std::make_shared<FixtureState>();
        state->state.at(1).tactical.saved = Fixed::from_raw(77);
        install(game, state); return eawr::core::Result<void>::success();
    };
    options.services = configuration;
    auto game = take(platform::LiveGameSession::start(setup(), options));
    game->advance_to(4); expect(game->wait_for(4, 5s), "live native owner reaches save boundary");
    platform::LiveSaveRequest request; request.seat = {1}; request.name = "native"; request.overwrite = true;
    const auto token = game->request_save(request); game->advance_to(5);
    expect(game->wait_for(5, 5s) && game->wait_save(token, 5s), "native state uses the live disk worker");
    auto restored = take(platform::LiveGameSession::start(take(game->prepare_load("native", false)), options));
    expect(created == 2 && restored->completed_tick() == game->completed_tick(), "live load creates fresh native adapter once at saved boundary");
    for (auto* owner : {game.get(), restored.get()}) { owner->advance_to(9); expect(owner->wait_for(9, 5s), "native live continuation progresses"); owner->stop(); }
    const auto control = game->tick_hashes(), resumed = restored->tick_hashes();
    expect(std::vector<std::string>(control.begin() + 5, control.end()) == resumed,
        "live disk save continuation equals uninterrupted same-boundary control");
    take(game->delete_save("native"));
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected an output directory");
        codecs(); std::vector<std::string> reference;
        for (auto workers : platform::determinism_worker_counts()) {
            checkpoint_boundaries(workers, reference);
            transit_boundary(workers);
            auto_return(workers);
            live_save(workers, std::filesystem::path(argv[1]) / std::to_string(workers));
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    if (failures) return 1;
    std::cout << "native AI codecs, checkpoint/child/return and live disk continuation passed on 1/2/4/8 workers\n";
    return 0;
}

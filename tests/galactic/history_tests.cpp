#include "galactic_support.hpp"
#include "ability_fixture.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/campaign/game_session.hpp"
#include "../../src/sim/galactic/checkpoint_fields_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using namespace galactic_test;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
using Fixed = eawr::sim::math::Fixed;
using Bytes = std::vector<std::uint8_t>;
gc::HistoryValue h(float value) { return {std::bit_cast<std::uint32_t>(value)}; }
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void pass(eawr::core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
class Probe final : public auth::GalacticScriptEngine {
public:
    std::function<void(const gc::GalacticSession&)> before;
    std::function<void()> after;
    eawr::core::Result<auth::ServiceOptions> before_service(const eawr::sim::PartitionExecutor&,
        const gc::GalacticSession& world, const gc::GalacticTick&, auth::ScriptScheduler&) override {
        if (before) before(world);
        return eawr::core::Result<auth::ServiceOptions>::success({});
    }
    eawr::core::Result<void> after_service(auth::ServiceReport&) override {
        if (after) after();
        return eawr::core::Result<void>::success();
    }
};
void binary32_contracts() {
    const auto scale = Fixed::scale;
    expect(gc::HistoryValue::from_fixed(Fixed::from_raw(scale+1)).bits == h(1).bits
        && gc::HistoryValue::from_fixed(Fixed::from_raw(scale+3)).bits == 0x3f800002U,
        "fixed input rounds binary32 halfway cases to even without native floating arithmetic");
    const auto large = (std::int64_t{1} << 62U)+(std::int64_t{1} << 38U)+1;
    expect(gc::HistoryValue::from_fixed(Fixed::from_raw(large)).bits == 0x52800001U
        && gc::HistoryValue::from_fixed(Fixed::from_raw(-large)).bits == 0xd2800001U,
        "full-range fixed conversion rounds directly to binary32 without an intermediate false tie");
    for (const auto a : {-7.75F, -1.25F, 0.F, 1.25F, 3.5F, 8192.F})
        for (const auto b : {-7.75F, -1.25F, 0.F, 1.25F, 3.5F, 8192.F}) {
            expect((h(a)+h(b)).bits == h(a+b).bits, "software History addition matches binary32 signed/fractional sums");
            expect(gc::history_half(h(a)+h(b)).bits == h((a+b)*.5F).bits, "software pair mean rounds the sum and half as binary32");
        }
    expect(gc::history_half({1}).bits == 0 && gc::history_half({3}).bits == 2
        && gc::history_half({0x00800001U}).bits == 0x00400000U
        && gc::history_half({0x00800003U}).bits == 0x00400002U,
        "halving retains subnormal nearest-even behavior at zero and normal boundaries");
    gc::checkpoint_detail::Writer wire;
    wire(gc::HistorySample{30, 2, 1, h(1.25F), h(3.5F)});
    expect(wire.bytes.size() == 32 && wire.bytes[8] == 0 && wire.bytes[10] == 0xa0 && wire.bytes[11] == 0x3f,
        "sample wire stores IEEE binary32 economy before integer planets and technology");
}

gc::GalacticSetup fixture() {
    auto s = ability_fixture::base_setup();
    s.ability_host.reset(); s.economy_corruption.reset();
    auto& control = ability_fixture::add_control(s);
    control.types = {{1000, 0, false, false}};
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 1000;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; row.starbase = {planet.owner, 1};
        control.planets.push_back(row);
    }
    auto& observer = ability_fixture::add_observer(s, 1, {1000, 2000, 3000});
    observer.history_players = {1, 2};
    s.census->types[0].history = gc::HistoryType{Fixed::from_raw(Fixed::scale * 7 / 2), false, false, false};
    gc::CensusType fleet; fleet.type = 2000; fleet.history = gc::HistoryType{{}, true, false, false};
    gc::CensusType planet; planet.type = 3000; planet.history = gc::HistoryType{{}, false, false, false};
    s.census->types.push_back(fleet); s.census->types.push_back(planet);
    for (const auto& p : s.planets) s.census->objects.push_back(ability_fixture::census_actor(p.id, 3000, p.owner, p.id, 0));
    for (const auto& f : s.fleets) s.census->objects.push_back(ability_fixture::census_actor(f.id, 2000, f.owner, f.orbit, 0));
    std::sort(s.census->objects.begin(), s.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    s.player_economy->players[0].credits = Fixed::from_raw(Fixed::scale * 9 / 4);
    s.player_economy->players[1].credits = Fixed::from_raw(Fixed::scale * 15 / 2);
    return s;
}
void cadence_and_provider() {
    Bytes reference; gc::HistoryArchive archive_reference;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
        auto s = fixture(); auto world = take(gc::GalacticSession::create(s));
        expect(world.history_archive().size() == 3 && world.history_archive().at(1).empty(), "fresh player archives exist and are empty");
        const std::array<std::uint64_t, 8> ticks{0, 1, 2, 3, 29, 30, 31, 30};
        const std::array<std::size_t, 8> counts{1, 2, 3, 3, 3, 4, 4, 5};
        for (std::size_t i = 0; i < ticks.size(); ++i) {
            pass(world.service_admitted_history(ticks[i], pool));
            expect(world.history_archive().at(1).size() == counts[i], "local admitted gate covers 0/1/2/3/29/30/31 and repeated 30");
        }
        const auto& rows = world.history_archive().at(1);
        expect(rows[3].frame == 30 && rows[4].frame == 30, "qualifying same-tick calls are not deduplicated");
        expect(rows[0].economy == h(2.25F) && rows[0].technology == 2 && rows[0].planets == 2,
            "producer records current fractional balance, raw technology and owned planetary objects");
        expect(rows[0].military == h(10.5F), "military sums ordinary and direct fleet contributions, rather than unit count");
        expect(world.history_archive().at(2).size() == 5 && world.history_archive().at(3).size() == 5,
            "recorder services nonlocal players including players excluded from the chart configuration");
        const auto state = world.canonical_state_bytes();
        if (reference.empty()) { reference = state; archive_reference = world.history_archive(); }
        else expect(state == reference && world.history_archive() == archive_reference, "archive and authoritative bytes equal at 1/2/4/8 workers");
        const auto checkpoint = take(world.save_checkpoint_state());
        auto restored = take(gc::GalacticSession::create(s)); pass(restored.load_checkpoint_state(checkpoint));
        expect(take(restored.save_checkpoint_state()) == checkpoint && restored.history_archive() == world.history_archive(),
            "direct owner checkpoint roundtrip preserves repeated ticks and stored order without resampling");
        pass(world.service_admitted_history(31, pool)); pass(restored.service_admitted_history(31, pool));
        expect(world.canonical_state_bytes() == restored.canonical_state_bytes(), "nonsampling first admitted turn matches after restore");
        pass(world.service_admitted_history(30, pool)); pass(restored.service_admitted_history(30, pool));
        expect(world.canonical_state_bytes() == restored.canonical_state_bytes(), "qualifying first admitted turn appends after restore");
    }
}
void compact_and_restore() {
    using namespace gc::checkpoint_detail;
    for (const std::size_t size : {99U, 100U, 101U, 102U}) {
        gc::HistoryArchive original;
        for (std::size_t i = 0; i < size; ++i)
            original[7].push_back({static_cast<std::uint64_t>(size-i), 2+i%2, 1+static_cast<std::int64_t>(i%2),
                h(i%2 ? 2.5F : 1.25F), h(i%2 ? 7.75F : 3.5F)});
        Writer save; save(original); Reader load(save.bytes); gc::HistoryArchive restored; load(restored);
        expect(!load.remaining() && restored == original, "checkpoint preserves 99/100/101/102 stored records including descending tick order");
        gc::service_history(restored.at(7), {31, 0, 0, {}, {}});
        const auto expected = size%2 == 0 ? size/2 : size;
        expect(restored.at(7).size() == expected, "every admitted nonsampling turn compacts only even archives of at least 100");
        if (size%2 == 0) {
            const auto& row = restored.at(7)[0];
            expect(row.frame == size-1 && row.planets == 2 && row.technology == 1,
                "frame, planet and technology pair means use integer division");
            expect(row.economy == h(1.875F) && row.military == h(5.625F), "economy and military means retain their fractions");
        }
        Writer compacted; compacted(restored); Reader reload(compacted.bytes); gc::HistoryArchive again; reload(again);
        expect(again == restored, "compacted fractional records survive checkpoint restore exactly");
        auto appended = original;
        gc::service_history(appended.at(7), {30, 9, 9, h(9.5F), h(8.25F)});
        expect(appended.at(7).size() == ((size+1)%2 == 0 ? (size+1)/2 : size+1), "append precedes resulting-size compaction");
    }
}
void owned_membership() {
    auto s = fixture();
    for (auto& row : s.census->objects) if (row.owner == 1 && row.original_type != 3000) {
        row.dead = true; row.delete_pending = true;
    }
    auto world = take(gc::GalacticSession::create(s)); eawr::sim::InlineExecutor executor;
    pass(world.service_admitted_history(0, executor));
    expect(world.history_archive().at(1).front().military == h(10.5F),
        "owned-list membership supplies History contributions without an extra death or pending-deletion filter");
}
void planetary_type_inputs() {
    eawr::data::galactic::Planet planet;
    const auto unknown = take(eawr::campaign::prepare_census_planet(planet, 3000));
    expect(!unknown.history, "missing resolved planetary combat-power input remains unavailable");
    const auto supplied = take(eawr::campaign::prepare_census_planet(planet, 3000,
        Fixed::from_raw(Fixed::scale * 7 / 4)));
    expect(supplied.history && supplied.history->power == Fixed::from_raw(Fixed::scale * 7 / 4),
        "planet adapter consumes separately resolved power without an ordinary starting-force type row");
    auto s = fixture(); s.census->types.back() = supplied;
    auto world = take(gc::GalacticSession::create(s)); eawr::sim::InlineExecutor executor;
    pass(world.service_admitted_history(0, executor));
    expect(world.history_archive().at(1).front().military == h(17.5F),
        "two owned planets contribute their supplied power to both metric queries");
}
void step_and_first_restore() {
    const auto s = fixture(); eawr::sim::InlineExecutor executor;
    auto world = take(gc::GalacticSession::create(s));
    for (unsigned boundary = 0; boundary < 2; ++boundary) {
        const auto bytes = take(world.save_checkpoint_state());
        auto restored = take(gc::GalacticSession::create(s)); pass(restored.load_checkpoint_state(bytes));
        expect(restored.history_archive() == world.history_archive(), "restoring before/after first service preserves the archive before admission");
        const auto old_snapshot = restored.snapshot();
        const auto a = take(world.step(executor)), b = take(restored.step(executor));
        expect(a.state_hash.get() == b.state_hash.get() && a.snapshot->canonical_bytes() == b.snapshot->canonical_bytes(),
            "first service after restore reproduces state and immutable publication");
        expect(world.history_archive().at(1).size() == boundary+1, "standalone service records pre-advance active ticks 0 then 1");
        expect(old_snapshot->history() && old_snapshot->history()->lines.front().samples.size() == boundary,
            "previous published History snapshot stays immutable");
    }
    auth::SessionConfig config; config.galactic_clock = true;
    auto ai = take(auth::ScriptScheduler::create(config, {}));
    auto story = take(auth::ScriptScheduler::create(config, {}));
    auto scripted = take(auth::ScriptedGalacticSession::create(take(gc::GalacticSession::create(s)), std::move(ai), std::move(story)));
    auto ai_probe = std::make_shared<Probe>(), story_probe = std::make_shared<Probe>();
    ai_probe->before = [](const auto& live) {
        expect(live.history_archive().at(1).size() == live.completed_tick()-1,
            "deferred recorder has not run at the AI boundary");
    };
    ai_probe->after = [&] {
        const std::array values{std::pair<gc::PlayerId, Fixed>{1, Fixed::from_raw(Fixed::scale * 21 / 4)}};
        pass(scripted.parent_world().apply_battle_credits(values));
    };
    story_probe->before = [](const auto& live) {
        expect(live.history_archive().at(1).size() == live.completed_tick()
            && live.history_archive().at(1).back().economy == h(5.25F),
            "History records committed AI account changes before the story reader");
    };
    pass(scripted.set_ai_engine(ai_probe)); pass(scripted.set_story_engine(story_probe));
    take(scripted.step(executor)); take(scripted.step(executor));
    expect(scripted.world().history_archive().at(1).size() == 2,
        "scripted owner records once after AI, without the standalone recorder duplicating the turn");
}
void privacy_and_global_axes() {
    gc::HistoryArchive archives{{1, {{30,2,1,h(20),h(4)},{60,3,2,h(40),h(8)}}},
        {2, {{0,5,3,h(100),h(20)},{90,6,4,h(200),h(40)}}}};
    const std::array<gc::PlayerId, 2> factions{1,2};
    const auto view = gc::publish_history(archives, factions, 9, 30, 1, true);
    expect(view.available && view.lines.size() == 1 && view.lines[0].player == 1, "multiplayer History copies only observer-authorized local lines");
    expect(view.first_frame == 0 && view.last_frame == 90 && view.maximum_economy == h(200) && view.maximum_military == h(40),
        "global frame bounds and maxima include suppressed faction archives");
    expect(view.planetary_objects == 9 && view.maximum_planets == 6, "planetary object count and recorded maximum stay distinct");
    expect(gc::publish_history(archives, factions, 9, 30, 0, true).lines.empty(), "unassigned observer gets no private archive line");
    archives.at(2).resize(1);
    expect(!gc::publish_history(archives, factions, 9, 30, 1, true).available, "hidden faction with fewer than two samples still blocks chart construction");
    auto s = fixture(); mark_multiplayer(s); s.observer->solo_reference_human = 0; s.observer->human_observers = {1,2};
    auto world = take(gc::GalacticSession::create(s)); eawr::sim::InlineExecutor executor;
    take(world.step(executor)); take(world.step(executor));
    const auto local = world.view_snapshot({1}), other = world.view_snapshot({2}), denied = world.view_snapshot({3});
    expect(local && other && local->history() && other->history() && !denied, "session publication checks the setup-authorized human observer roster");
    expect(local && other && local->history() && other->history() && local->history()->lines.size() == 1 && local->history()->lines[0].player == 1
        && other->history()->lines.size() == 1 && other->history()->lines[0].player == 2,
        "each seat publication contains its own faction only");
}
void campaign_install_and_restore() {
    namespace campaign = eawr::campaign;
    std::vector<std::string> reference;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto setup = fixture();
        const auto schedulers = [] {
            auth::SessionConfig config; config.galactic_clock = true;
            return std::pair{take(auth::ScriptScheduler::create(config, {})),
                take(auth::ScriptScheduler::create(config, {}))};
        };
        auto game = take(campaign::GameSession::create(setup));
        auto initial = schedulers();
        pass(game.install_galactic_scripts(std::move(initial.first), std::move(initial.second)));
        take(game.step(executor));
        expect(game.galaxy().history_archive().at(1).size() == 1,
            "WGP-146 fresh campaign installation records one History sample per admitted turn");
        const auto owner = take(game.save_checkpoint_state());
        const auto scripts = take(game.save_galactic_scripts());
        auto restored = take(campaign::GameSession::create(setup));
        pass(restored.load_checkpoint_state(owner));
        auto resumed = schedulers();
        pass(restored.install_galactic_scripts(std::move(resumed.first), std::move(resumed.second), scripts));
        std::vector<std::string> hashes;
        for (unsigned frame = 0; frame < 90; ++frame) {
            const auto control = take(game.step(executor));
            const auto loaded = take(restored.step(executor));
            hashes.push_back(control.state_hash.get());
            expect(loaded.state_hash.get() == control.state_hash.get()
                && restored.galaxy().history_archive() == game.galaxy().history_archive(),
                "campaign History and composed hashes continue equally after fresh and restored script installation");
        }
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "campaign History continuation equals on 1/2/4/8 workers");
    }
}
}
int main() {
    try { binary32_contracts(); cadence_and_provider(); compact_and_restore(); owned_membership(); planetary_type_inputs(); step_and_first_restore(); privacy_and_global_axes(); campaign_install_and_restore(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    if (failures) return 1;
    std::cout << "History provider, checkpoint and admission contracts passed\n";
}

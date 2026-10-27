#include "viewer_lan_fixtures.hpp"
#include "lan_match.hpp"
#include "skirmish_prepare.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "fake_transport.hpp"
#include "setup_test_support.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/setup.hpp"
#include "eawr/net/tactical_input.hpp"
#include "eawr/net/transport.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace viewer = eawr::presentation::godot_backend;
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;

namespace viewer_lan_test {
void expect(bool value, const std::string& message) { setup_test::expect(value, message); }
sim::math::Fixed units(std::int64_t value) { return setup_test::units(value); }
sim::MatchAgreement identity() { return setup_test::identity(); }
tactical::TacticalSetup setup() { return setup_test::setup(); }
tactical::TacticalContent content() { return setup_test::content(); }
net::SetupOffer offer() { return setup_test::offer(); }
tactical::TacticalSetup human_setup(unsigned count) { return setup_test::human_setup(count); }

void player_row_parser() {
    expect(require(viewer::skirmish_player_ids("1,2,3,4,5,6,7,8")).size() == 8, "eight local faction rows parse");
    expect(require(viewer::skirmish_player_ids("1,2,3,4,5,6,7,8,9")).size() == 9, "nine local faction rows parse");
    for (const auto text : {"1,2,3,4,5,6,7,8,9,10", "1,2,", "1,1", "0,2", "1", "1,x"})
        expect(!viewer::skirmish_player_ids(text), "invalid local rows refuse at parse time");
}

struct StagingIngress {
    std::shared_ptr<viewer::LanMatch> host{make_match()}, client{make_match()};
    std::unique_ptr<net::TcpTransport> source;
    const std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::now() + std::chrono::seconds(20)};
    StagingIngress() {
        host->configure_seats({1, 2, 3}); host->host(0);
        net::TcpOptions options; options.build_identity = identity().build; options.peers = 3; options.seat = 1;
        source = require(net::TcpTransport::connect("127.0.0.1", host->view().port, options));
        wait([&] { host->pump_setup({}); (void)source->poll(); return source->self() == 1; });
        client->configure_seats({1, 2, 3}); client->join("127.0.0.1:" + std::to_string(host->view().port));
        wait([&] { pump(); return host->view().connected && client->view().connected && source->link(2) == net::LinkState::open; });
    }
    ~StagingIngress() { client->cancel(); host->cancel(); }
    template<class F> void wait(F predicate) {
        while (!predicate()) {
            expect(std::chrono::steady_clock::now() < deadline, "viewer early-frame test deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void pump() { host->pump_setup({}); (void)source->poll(); client->pump_setup({}); }
    void send(const Bytes& frame) {
        auto sent = source->send(2, frame);
        while (!sent && sent.error().code == "EAWR-NET-0205") {
            pump(); expect(std::chrono::steady_clock::now() < deadline, "early-frame backpressure deadline");
            sent = source->send(2, frame);
        }
        require(std::move(sent)); pump();
    }
};

void staging_ingress_bounds() {
    stage = "viewer early hash count and byte bounds";
    eawr::testing::FakeNetwork network(3);
    eawr::testing::FakeTransport source(network, 1);
    net::DesyncMonitor monitor(source);
    auto world = require(tactical::TacticalSession::create(setup(), content()));
    require(monitor.record(0, world.state_sha256(), world.canonical_state_bytes(), {}, false));
    const auto hash = network.inbox[2].front().received.frame;
    for (const bool byte_bound : {false, true}) {
        StagingIngress ingress;
        auto frame = hash;
        if (byte_bound) frame.resize(net::max_batch_bytes);
        const auto count = byte_bound ? 8U : 8192U;
        for (unsigned index = 0; index < count; ++index) {
            ingress.send(frame);
            expect(ingress.client->view().phase != viewer::LanPhase::ended, "early hash at its bound does not end human review");
        }
        ingress.send(hash);
        ingress.wait([&] { ingress.pump(); return ingress.client->view().phase == viewer::LanPhase::ended; });
        expect(ingress.client->view().reason == viewer::LanReason::refused
            && ingress.client->view().message == "Unexpected message while waiting for match setup."
            && !ingress.client->offered_setup(), "one further early hash fails at the count or byte bound without accepting setup");
    }
    stage = "untrusted offer does not change viewer owner";
    StagingIngress ingress;
    auto selected = offer(); selected.setup = require(net::encode_skirmish_setup(human_setup(3)));
    selected.seats.push_back({2, sim::SeatId{15}, 3});
    const auto before = ingress.client->view();
    ingress.send(require(net::encode_setup_offer(selected)));
    ingress.wait([&] { ingress.pump(); return ingress.client->view().phase == viewer::LanPhase::ended; });
    expect(!ingress.client->offered_setup() && ingress.client->view().player == before.player
        && ingress.client->view().human_owners == before.human_owners && ingress.client->view().seat == before.seat,
        "a non-host offer is rejected before changing offered setup or local owner");
}

void refused_rosters() {
    stage = "unsupported command-seat roster";
    for (const auto& owners : std::vector<std::vector<tactical::PlayerId>>{
        std::vector<tactical::PlayerId>(10, 1), {1, 1}, {0, 2}}) {
        auto match = make_match();
        match->configure_seats(owners); match->host(0);
        expect(match->view().phase == viewer::LanPhase::ended && match->view().diagnostic_code == "EAWR-NET-1300",
            "ten seats, shared owners and zero owners fail before transport admission");
    }
}

void separate_mesh(std::uint32_t count, bool coop, bool ability_triage, unsigned workers) {
    stage = "separate-owner viewer mesh";
    auto initial = ability_triage ? setup() : human_setup(count);
    initial.players[0].faction_id = eawr::skirmish::faction_id("Rebel");
    initial.players[1].faction_id = eawr::skirmish::faction_id("Empire");
    if (!ability_triage) {
        for (auto& player : initial.players) {
            if (coop) player.team_id = 1;
            player.faction_id = eawr::skirmish::faction_id(player.team_id == 1 ? "Rebel" : "Empire");
        }
        if (coop) {
            initial.players.push_back({count + 1, 2, eawr::skirmish::faction_id("Empire"), tactical::player_flag_commandable});
            initial.skirmish->slots.push_back({count + 1, false, count, {}});
            auto unit = initial.units.back(); unit.entity_id = count + 1; unit.owner = count + 1;
            unit.position = {units(10000), units(10000), {}};
            initial.units.push_back(unit);
        }
    }
    if (ability_triage) {
        initial.players.resize(2); initial.units.resize(2); initial.skirmish->slots.resize(2);
        initial.queue_identities = true;
    }
    std::vector<tactical::PlayerId> owners;
    for (std::uint32_t peer = 0; peer < count; ++peer) owners.push_back(peer + 1);
    std::vector<std::shared_ptr<viewer::LanMatch>> matches;
    for (std::uint32_t peer = 0; peer < count; ++peer) {
        auto match = make_match(); match->configure_seats(owners); matches.push_back(std::move(match));
    }
    matches[0]->host(0);
    for (std::uint32_t peer = 1; peer < count; ++peer)
        matches[peer]->join("127.0.0.1:" + std::to_string(matches[0]->view().port));
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map;
    selected.slots.emplace();
    for (const auto& player : initial.players)
        selected.slots->push_back({player.player_id,
            player.faction_id == eawr::skirmish::faction_id("Rebel") ? "Rebel" : "Empire",
            player.team_id, player.player_id <= count, {}, player.player_id - 1});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto wait = [&](const auto& predicate) {
        while (!predicate()) {
            expect(std::chrono::steady_clock::now() < deadline, "separate viewer mesh deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    wait([&] {
        for (auto& match : matches) match->pump_setup(selected);
        return std::all_of(matches.begin(), matches.end(), [](const auto& match) { return match->view().connected; });
    });
    matches[0]->start(selected);
    expect(matches[0]->take_start().has_value(), "mesh host hands over separate owner rows");
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto rules = content();
    if (ability_triage) {
        tactical::AbilityProfile ability;
        ability.kind = tactical::AbilityKind::spoiler_lock;
        tactical::UnitAbilityProfile unit;
        unit.type_id = 1; unit.abilities = {ability};
        rules.abilities.profiles = {unit};
        rules.abilities.humans = {1, 2};
        auto ship_motion = rules.motion.profiles.front(); ship_motion.type_id = 2;
        rules.motion.profiles.push_back(ship_motion);
        rules.sensors.push_back({2, units(1000)});
        tactical::BuildOption ship;
        ship.type = 2; ship.price = units(100); ship.build_frames = 12; ship.ai_build_frames = 12;
        ship.population = 1; ship.available = true;
        for (const auto& player : initial.players) {
            tactical::EconomyPlayer economy;
            economy.player = player.player_id; economy.credits = units(1000); economy.population_cap = 20;
            rules.economy.players.push_back(economy);
            tactical::StationMenu menu;
            menu.station = 1; menu.faction = player.faction_id; menu.options = {ship};
            rules.economy.menus.push_back(menu);
        }
        std::sort(rules.economy.menus.begin(), rules.economy.menus.end(), [](const auto& a, const auto& b) {
            return std::pair{a.station, a.faction} < std::pair{b.station, b.faction};
        });
    }
    std::vector<tactical::TacticalSession> worlds;
    std::vector<eawr::platform::LiveStepContext> contexts(count);
    std::vector<eawr::platform::LiveStepDriver> drivers(count);
    std::vector<std::shared_ptr<eawr::presentation::ui::CommandScheduler>> schedulers(count);
    std::vector<eawr::platform::LiveEventLog> event_logs;
    for (std::uint32_t peer = 0; peer < count; ++peer) event_logs.emplace_back(eawr::platform::LiveEventLog::Bounds{});
    for (std::uint32_t peer = 0; peer < count; ++peer) worlds.push_back(require(tactical::TacticalSession::create(initial, rules)));
    if (ability_triage) for (std::uint32_t peer = 0; peer < count; ++peer) {
        schedulers[peer] = std::make_shared<eawr::presentation::ui::CommandScheduler>(owners[peer], 0, 0,
            [&, peer] { return worlds[peer].completed_tick(); });
        schedulers[peer]->present_snapshot(0);
        matches[peer]->set_input_epoch_provider([&, peer] { return schedulers[peer]->pending_intent_epoch(); });
        matches[peer]->set_submission_observer([&, peer](std::uint64_t sequence, std::uint64_t tick) {
            schedulers[peer]->note_execution(sequence, tick);
            std::cout << "SUBMIT shared=" << coop << " workers=" << workers << " peer=" << peer
                << " local_sequence=" << sequence << " execution_tick=" << tick << '\n';
        });
    }
    for (std::uint32_t peer = 0; peer < count; ++peer) {
        auto& context = contexts[peer]; context.advance_allowed = false;
        context.completed_tick = [&, peer] { return worlds[peer].completed_tick(); };
        context.state_hash = [&, peer] { return worlds[peer].state_sha256(); };
        context.canonical_state = [&, peer] { return worlds[peer].canonical_state_bytes(); };
        context.install = [&, peer](std::span<const tactical::PlayerCommand> commands) {
            for (const auto& command : commands) {
                if (ability_triage) std::cout << "INSTALL shared=" << coop << " workers=" << workers
                    << " peer=" << peer << " tick=" << command.key.tick << " owner=" << command.key.player_id
                    << " owner_sequence=" << command.key.sequence << " ability="
                    << std::holds_alternative<tactical::AbilityPayload>(command.payload) << '\n';
                require(worlds[peer].submit(command));
            }
            return eawr::core::Result<void>::success();
        };
        context.step = [&, peer] {
            require(worlds[peer].step(executor));
            if (ability_triage) {
                const auto snapshot = worlds[peer].snapshot();
                // Use the actual filtered platform log, not snapshot command results.
                event_logs[peer].record(*snapshot);
                for (const auto& record : event_logs[peer].after(0, snapshot->completed_tick()).ticks)
                    for (const auto& event : record.events) expect(event.kind != tactical::EventKind::order_accepted
                        && event.kind != tactical::EventKind::order_rejected, "platform result events are intentionally filtered");
                schedulers[peer]->present_snapshot(snapshot->completed_tick());
                for (const auto& event : snapshot->events()) {
                    std::cout << "EVENT shared=" << coop << " workers=" << workers << " peer=" << peer
                        << " tick=" << event.tick << " owner=" << event.player << " owner_sequence="
                        << event.sequence << " kind=" << tactical::to_string(event.kind) << '\n';
                }
                for (const auto& instance : snapshot->instances()) for (const auto& ability : instance.abilities)
                    std::cout << "STATE shared=" << coop << " workers=" << workers << " peer=" << peer
                        << " completed=" << snapshot->completed_tick() << " unit=" << instance.entity_id
                        << " active=" << ability.active << " ready=" << ability.ready
                        << " remaining=" << ability.remaining_frames << " pending="
                        << schedulers[peer]->reservations().size() << '\n';
            }
            return eawr::core::Result<void>::success();
        };
        context.snapshot = [&, peer] { return worlds[peer].snapshot(); };
    }
    std::uint64_t now{};
    drivers[0] = matches[0]->driver(initial);
    wait([&] {
        ++now;
        for (std::uint32_t peer = 0; peer < count; ++peer) {
            if (drivers[peer]) require(drivers[peer](contexts[peer], {}, now));
            else {
                matches[peer]->pump_setup(selected);
                if (matches[peer]->view().accept_enabled) matches[peer]->accept();
                if (matches[peer]->take_start()) drivers[peer] = matches[peer]->driver(initial);
            }
        }
        return std::all_of(drivers.begin(), drivers.end(), [](const auto& driver) { return !!driver; });
    });
    for (auto& context : contexts) context.advance_allowed = true;
    if (ability_triage) {
        namespace ui = eawr::presentation::ui;
        const auto advance = [&](std::uint64_t target) {
            wait([&] {
                ++now;
                for (std::uint32_t peer = 0; peer < count; ++peer) if (worlds[peer].completed_tick() < target) {
                    auto commands = schedulers[peer]->take(worlds[peer].completed_tick());
                    require(drivers[peer](contexts[peer], commands, now));
                }
                return std::all_of(worlds.begin(), worlds.end(), [target](const auto& world) { return world.completed_tick() == target; });
            });
            expect(worlds[0].state_sha256() == worlds[1].state_sha256(), "ability repro peers agree on every script boundary");
        };
        ui::TacticalIntent first;
        first.verb = ui::TacticalVerb::ability; first.units = {1}; first.unit_ability = tactical::AbilityKind::spoiler_lock;
        require(schedulers[0]->issue(first));
        advance(8);
        expect(!schedulers[0]->consumed(first), "first sole-issuer activation clears on its own owner");
        auto second = first; second.units = {owners[1]};
        second.ability_action = tactical::AbilityAction::activate;
        require(schedulers[1]->issue(second));
        advance(16);
        expect(!schedulers[1]->consumed(second), "joiner releases its completed ability without filtered results");
        first.ability_action = tactical::AbilityAction::deactivate;
        require(schedulers[0]->issue(first));
        advance(24);
        expect(!schedulers[0]->consumed(first), "host releases its later ability despite owner sequence remapping");
        advance(32);
        // Test-only scalar adapter for a singleton button; mirrors the production pending override.
        struct ButtonState final : ui::AbilityState {
            ui::UnitAbilityState current;
            std::optional<ui::UnitAbilityState> state(sim::EntityId, std::uint32_t) const override { return current; }
        };
        for (std::uint32_t peer = 0; peer < count; ++peer) {
            const auto& intent = peer == 0 ? first : second;
            const auto snapshot = worlds[peer].snapshot();
            const auto instance = std::find_if(snapshot->instances().begin(), snapshot->instances().end(),
                [&](const auto& unit) { return unit.entity_id == intent.units.front(); });
            expect(instance != snapshot->instances().end() && instance->abilities.size() == 1, "synthetic ability is published");
            const auto& status = instance->abilities.front();
            ButtonState state;
            state.current.status = schedulers[peer]->consumed(intent) ? ui::AbilityStatus::disabled
                : status.active ? ui::AbilityStatus::active : ui::AbilityStatus::ready;
            ui::AbilityButton button; button.ability = ui::ability_index("SPOILER_LOCK"); button.units = intent.units;
            const auto click = ui::ability_click(button, false, state);
            const bool clickable = click.has_value();
            expect(clickable, "each owner button accepts a re-click");
            expect(worlds[peer].record().commands.size() == 3, "three gestures are installed exactly once, never duplicated or deduped away");
            std::cout << "RESULT shared=" << coop << " workers=" << workers << " peer=" << peer
                << " pending=" << schedulers[peer]->reservations().size() << " clickable=" << clickable
                << " sim_active=" << status.active << " sim_ready=" << status.ready
                << " commands=" << worlds[peer].record().commands.size() << " hash=" << worlds[peer].state_sha256() << '\n';
            auto retry = intent;
            retry.ability_action = click->kind == ui::AbilityRequest::Kind::deactivate
                ? tactical::AbilityAction::deactivate : tactical::AbilityAction::activate;
            require(schedulers[peer]->issue(retry));
        }
        advance(40);
        for (const auto& scheduler : schedulers) expect(scheduler->reservations().empty(), "both real re-clicks finish without pending abilities");
        const auto ledger = [&](std::uint32_t peer) -> tactical::EconomyView {
            const auto snapshot = worlds[peer].snapshot();
            for (const auto& view : snapshot->economy()) if (view.player == 1) return view;
            throw std::runtime_error("missing synthetic player economy");
        };
        const auto* option = rules.economy.menu(1, initial.players[0].faction_id)->find(2);
        const auto option_of = [&](sim::EntityId, tactical::TypeId) { return option; };
        ui::TacticalIntent buy; buy.verb = ui::TacticalVerb::buy; buy.units = {1}; buy.type = 2;
        require(schedulers[0]->issue(buy));
        const auto preview = ui::pending_economy(ledger(0), schedulers[0]->reservations(), option_of, worlds[0].completed_tick());
        expect(preview.queues[0].size() == 1 && preview.queues[0].front().entry_id == 0, "only unexecuted purchase has one preview without cancel authority");
        ui::TacticalIntent cancel; cancel.verb = ui::TacticalVerb::cancel;
        expect(!schedulers[0]->issue(cancel), "a preview entry cannot issue a cancellation");
        advance(44);
        for (std::uint32_t peer = 0; peer < count; ++peer) {
            const auto view = ledger(peer);
            const auto shown = ui::pending_economy(view, schedulers[peer]->reservations(), option_of, worlds[peer].completed_tick());
            expect(view.queues[0].size() == 1 && shown.queues[0] == view.queues[0]
                && view.queues[0].front().entry_id != 0 && view.credits == units(900), "purchase executes once and has no ghost preview on either peer");
        }
        cancel.queue_entry_id = ledger(0).queues[0].front().entry_id;
        require(schedulers[0]->issue(cancel)); advance(48);
        expect(ledger(0).queues[0].empty() && ledger(1).queues[0].empty() && ledger(0).credits == units(1000), "authoritative cancellation removes one purchase and refunds once");
        require(schedulers[0]->issue(buy)); advance(68);
        expect(ledger(0).queues[0].empty() && ledger(1).queues[0].empty()
            && ledger(0).pool == std::vector<tactical::TypeId>{2} && ledger(1).pool == ledger(0).pool,
            "second purchase completes once without a tile stuck at 100 percent");
        // The previous viewer attached a skirmish token to this campaign-only wire route.
        const tactical::SeatBinding bound{sim::SeatId{1}, 1, true, false};
        tactical::ReinforcePayload identity_drop{2, {units(1000), {}, {}}, ledger(0).pool_tokens.front()};
        const auto old_input = net::encode_tactical_input({68, bound.seat, 0}, bound, {{}, identity_drop});
        expect(!old_input && old_input.error().code == net::session_errors::admission,
            "ordinary skirmish identity token reproduces the misleading synchronization refusal");
        ui::TacticalIntent reinforce; reinforce.verb = ui::TacticalVerb::reinforce;
        reinforce.type = 2; reinforce.destination = {units(1000), {}, {}};
        // The producer may wait for peer synchronization after a click; its reservation survives the hold.
        for (auto& context : contexts) context.advance_allowed = false;
        viewer::LanCommandSink sink(*schedulers[0], matches[0]);
        require(sink.issue(reinforce));
        for (std::uint32_t peer = 0; peer < count; ++peer) {
            const auto commands = schedulers[peer]->take(worlds[peer].completed_tick());
            expect(!require(drivers[peer](contexts[peer], commands, ++now)), "held synchronization pump cannot advance the world");
        }
        expect(!schedulers[0]->reservations().empty(), "unexecuted reinforcement remains reserved during synchronization hold");
        for (auto& context : contexts) context.advance_allowed = true;
        advance(240);
        for (std::uint32_t peer = 0; peer < count; ++peer) {
            const auto snapshot = worlds[peer].snapshot();
            const auto arrived = std::find_if(snapshot->instances().begin(), snapshot->instances().end(),
                [](const auto& unit) { return unit.type_id == 2 && unit.owner == 1; });
            expect(arrived != snapshot->instances().end() && !arrived->arrival
                && ledger(peer).pool.empty() && ledger(peer).population == 1,
                "MP skirmish reinforcement is admitted once and finishes arrival on both peers");
            expect(schedulers[peer]->reservations().empty(), "reinforcement reservation clears after synchronized execution");
            expect(matches[peer]->view().input_feedback.empty(), "valid skirmish reinforcement produces no synchronization-cancel notice");
        }
        static std::map<bool, std::string> first_hash;
        const auto [baseline, inserted] = first_hash.emplace(coop, worlds[0].state_sha256());
        expect(inserted || baseline->second == worlds[0].state_sha256(), "ability repro is identical across 1/2/4/8 workers");
        for (auto& match : matches) match->cancel();
        return;
    }
    // Every owner submits its own unit order through the real setup/router/lockstep path.
    for (std::uint32_t peer = 0; peer < count; ++peer) {
        const auto owner = matches[peer]->local_player();
        const tactical::PlayerCommand move{{0, owner, 0}, {owner},
            tactical::MovePayload{{units(static_cast<std::int64_t>(owner) * 1000 + 100), units(3000), {}}}};
        require(drivers[peer](contexts[peer], std::span(&move, 1), ++now));
    }
    wait([&] {
        ++now;
        for (std::uint32_t peer = 0; peer < count; ++peer)
            if (worlds[peer].completed_tick() < 120) require(drivers[peer](contexts[peer], {}, now));
        return std::all_of(worlds.begin(), worlds.end(), [](const auto& world) { return world.completed_tick() == 120; });
    });
    for (std::uint32_t peer = 0; peer < count; ++peer) {
        expect(matches[peer]->local_player() == owners[matches[peer]->view().seat], "local fog and asset owner come from the assigned offer seat");
        expect(matches[peer]->view().human_owners == owners && matches[peer]->view().connected_seats == count,
            "all viewers present the complete separate-owner roster");
        expect(worlds[peer].state_sha256() == worlds.front().state_sha256()
            && worlds[peer].record().commands.size() == count, "separate owners install every order exactly once and agree at 120 ticks");
    }
    static std::map<std::pair<unsigned, bool>, std::string> first_hash;
    const auto [baseline, inserted] = first_hash.emplace(std::pair{count, coop}, worlds[0].state_sha256());
    expect(inserted || baseline->second == worlds[0].state_sha256(), "allied human match equals across 1/2/4/8 workers");
    std::cout << "separate owners=" << count << " coop=" << coop << " workers=" << workers
        << " ticks=120 hash=" << worlds[0].state_sha256() << '\n';
    if (!coop && workers == 1) {
        // Real drivers, real router: the pauser's own feedback and the other peers' label for it.
        stage = "separate-owner pause feedback";
        const auto pauser = count - 1;
        const auto pump_until = [&](const auto& predicate) {
            wait([&] {
                ++now;
                for (std::uint32_t peer = 0; peer < count; ++peer) require(drivers[peer](contexts[peer], {}, now));
                return predicate();
            });
        };
        const auto all_paused = [&](bool paused) {
            return std::all_of(matches.begin(), matches.end(), [paused](const auto& match) { return match->view().clock_paused == paused; });
        };
        const auto cycle = [&] {
            expect(matches[pauser]->request_pause(true), "pauser's request is accepted");
            pump_until([&] { return all_paused(true); });
        };
        cycle();
        expect(matches[pauser]->view().input_feedback.empty(), "the waiting notice clears once the pause is agreed");
        for (std::uint32_t peer = 0; peer < count; ++peer)
            if (peer != pauser) expect(!matches[peer]->view().pause_name.empty() && matches[peer]->view().pause_name != "You",
                "other peers see a label that names the pauser, not themselves");
        if (count == 3) expect(matches[0]->view().pause_name == matches[1]->view().pause_name, "two non-pausing peers agree on the pauser's label");
        const auto resume = [&] {
            expect(matches[0]->request_pause(false), "any active human may resume");
            pump_until([&] { return all_paused(false); });
        };
        // #4574: a pause/resume pair spends no quota; the unit contracts cover repeated cycles and the debounce.
        resume();
        expect(matches[pauser]->view().input_feedback.find("Pause repeated too quickly") == std::string::npos,
            "a pause and resume pair is never refused");
    }
    for (auto& match : matches) match->cancel();
}

void shared_builder_matches_former_live_encoding() {
    stage = "real shared skirmish builder parity";
    for (const bool opponent_human : {false, true}) {
        eawr::units::UnitTables tables;
        eawr::skirmish::Fixture fixture;
        fixture.map = "data/art/maps/parity.ted"; fixture.map_sha256 = std::string(64, 'a'); fixture.seed = 83;
        fixture.pre_built_base = fixture.free_starting_units = false;
        fixture.slots = {{1, "Parity_A", 0, true, {}, 2}, {2, "Parity_B", 1, opponent_human, {}, 4}};
        eawr::skirmish::StartInputs inputs; inputs.map = fixture.map; inputs.map_sha256 = fixture.map_sha256;
        inputs.tables = &tables; inputs.lobby_colours.resize(9);
        for (unsigned team = 0; team < 2; ++team) {
            eawr::skirmish::StartFaction faction; faction.name = team == 0 ? "Parity_A" : "Parity_B"; faction.playable = true;
            inputs.factions.push_back(std::move(faction));
            eawr::skirmish::MapPlacement marker; marker.record = team;
            marker.type = "Team_0" + std::to_string(team) + "_Spawn_Point_Marker";
            marker.marker = true; marker.position = sim::math::Vec3{}; marker.orientation_degrees = sim::math::Vec3{};
            inputs.placements.push_back(std::move(marker));
        }
        // Former live path: build_start -> recording_setup -> LAN departure metadata -> queue identities.
        const auto former_start = require(eawr::skirmish::build_start(fixture, inputs));
        auto former_setup = eawr::skirmish::recording_setup(fixture, former_start);
        former_setup.departure_policy.emplace();
        for (const auto& row : former_setup.skirmish->slots) if (row.human) former_setup.departure_policy->humans.push_back(row.player);
        former_setup.queue_identities = true;
        auto shared = require(viewer::prepare_skirmish(fixture, std::move(tables), std::move(inputs)));
        auto moved = std::move(shared);
        expect(moved.inputs.tables == &moved.tables, "shared inputs bind their current owning tables after a move");
        require(eawr::skirmish::build_start(fixture, moved.inputs)); // Exercise the rebound pointer, not only its address.
        const auto shared_setup = viewer::lan_recording_setup(fixture, moved.start);
        expect(require(net::encode_skirmish_setup(former_setup)) == require(net::encode_skirmish_setup(shared_setup)), "shared and former live canonical setup bytes are identical");
        net::SetupOffer former_offer; former_offer.seed = fixture.seed;
        former_offer.setup = require(net::encode_skirmish_setup(former_setup));
        former_offer.identity = {std::string(eawr::core::build_identity()), *former_setup.match_identity};
        former_offer.seats = {{0, sim::SeatId{1}, 1}, {1, sim::SeatId{2}, opponent_human ? 2U : 1U}};
        auto shared_offer = former_offer; shared_offer.setup = require(net::encode_skirmish_setup(shared_setup)); shared_offer.identity.content = *shared_setup.match_identity;
        if (opponent_human)
            expect(require(net::encode_setup_offer(former_offer)) == require(net::encode_setup_offer(shared_offer)), "production shared builder matches former live SetupOffer bytes and digest");
        else
            expect(!net::encode_setup_offer(former_offer) && !net::encode_setup_offer(shared_offer),
                "both builders refuse a shared-owner skirmish offer");
    }
}

void lan_overflow_human_keeps_its_seat() {
    stage = "LAN human seat beyond its team's marker count";
    eawr::units::UnitTables tables;
    eawr::skirmish::Fixture fixture;
    fixture.map = "data/art/maps/lan-overflow.ted"; fixture.map_sha256 = std::string(64, 'a'); fixture.seed = 84;
    fixture.pre_built_base = fixture.free_starting_units = false;
    fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}}, {3, "Empire", 1, true, {}}};
    eawr::skirmish::SetupMap map; map.path = fixture.map;
    map.metadata.capacity = 3; map.metadata.new_markers = true; map.metadata.start_positions.emplace(2);
    map.teams = {{0, {"Rebel"}, 1}, {1, {"Empire"}, 1}};
    eawr::skirmish::SetupSelection selection; selection.map = map.path; selection.slots = fixture.slots;
    const auto options = require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1)));
    expect(options.slots && options.slots->back().human, "overflow guest keeps human control through lobby validation");
    eawr::skirmish::StartInputs inputs; inputs.map = fixture.map; inputs.map_sha256 = fixture.map_sha256;
    inputs.tables = &tables; inputs.lobby_colours.resize(3);
    for (unsigned team = 0; team < 2; ++team) {
        eawr::skirmish::StartFaction faction; faction.name = team == 0 ? "Rebel" : "Empire"; faction.playable = true;
        inputs.factions.push_back(std::move(faction));
        eawr::skirmish::MapPlacement marker; marker.record = team;
        marker.type = "Team_0" + std::to_string(team) + "_Spawn_Point_Marker";
        marker.marker = true; marker.position = sim::math::Vec3{}; marker.orientation_degrees = sim::math::Vec3{};
        inputs.placements.push_back(std::move(marker));
    }
    const auto prepared = require(viewer::prepare_skirmish(fixture, std::move(tables), std::move(inputs)));
    expect(prepared.start.players.size() == 3 && prepared.start.players.back().human
        && std::none_of(prepared.start.markers.begin(), prepared.start.markers.end(),
            [](const auto& marker) { return marker.player == 3; }),
        "LAN preparation retains an overflow guest without inventing a spawn");
    const auto canonical = viewer::lan_recording_setup(fixture, prepared.start);
    auto offered = offer(); offered.seed = fixture.seed;
    offered.setup = require(net::encode_skirmish_setup(canonical));
    offered.identity = {std::string(eawr::core::build_identity()), *canonical.match_identity};
    offered.seats = {{0, sim::SeatId{1}, 1}, {1, sim::SeatId{2}, 3}};
    const auto decoded = require(net::decode_setup_offer(require(net::encode_setup_offer(offered))));
    expect(decoded.seats.back().player == 3 && canonical.departure_policy
        && canonical.departure_policy->humans == std::vector<tactical::PlayerId>{1, 3},
        "canonical LAN offer preserves the overflow player's command seat and departure policy");
}

void lan_ai_defaults_follow_authored_constraints() {
    stage = "LAN AI defaults prefer free markers without refusing overflow";
    eawr::skirmish::SetupMap map; map.path = setup().skirmish->map;
    map.metadata.capacity = 4; map.metadata.new_markers = true; map.metadata.start_positions.emplace(4);
    map.teams = {{0, {"Rebel", "Empire"}, 2}, {1, {"Rebel", "Empire"}, 2}, {2, {"Rebel", "Empire"}, 1}};
    eawr::skirmish::SetupSelection initial; initial.map = map.path; initial.match = setup().skirmish->match;
    initial.slots = {{1, "Rebel", 0, true, {}, 0}, {2, "Empire", 1, true, {}, 1}};
    auto selection = initial;
    for (const auto item : {2, 4, 3}) {
        require(viewer::select_lan_ai_level(selection, map, 3, 8, item));
        const auto level = item == 2 ? tactical::AiLevel::easy : item == 4 ? tactical::AiLevel::hard : tactical::AiLevel::normal;
        expect(selection.slots.back().difficulty == level && selection.slots.size() == 3,
            "Easy/Hard/Medium create then update the same AI row");
        net::LobbySnapshot snapshot;
        snapshot.game = require(viewer::encode_lan_lobby_game(require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1)))));
        expect(require(viewer::decode_lan_lobby_game(snapshot)).slots->back().difficulty == level,
            "the editable snapshot preserves each AI level");
        snapshot.game.options.front() = '1';
        const auto old = viewer::decode_lan_lobby_game(snapshot);
        expect(!old && old.error().message == "Unsupported skirmish staging options version; all peers must use the same build.",
            "the old staging codec is refused with a named version error");
    }
    const auto valid = require(viewer::encode_lan_lobby_game(require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1)))));
    expect(!viewer::select_lan_ai_level(selection, map, 1, 8, 2)
        && !viewer::select_lan_ai_level(selection, map, 3, 8, 1), "human rows and Open are not AI difficulty edits");
    expect(require(viewer::encode_lan_lobby_game(require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1))))) == valid,
        "refused controller edits preserve all row bytes");
    auto invalid = require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1)));
    invalid.slots->back().difficulty = static_cast<tactical::AiLevel>(3);
    expect(!viewer::encode_lan_lobby_game(invalid), "out-of-range AI levels are refused before publication");
    invalid.slots->back().difficulty = tactical::AiLevel::easy; invalid.slots->back().human = true;
    expect(!viewer::encode_lan_lobby_game(invalid), "a human row cannot publish an AI level");
    {
        auto editable = initial;
        editable.slots[1].human = false;
        for (const auto kind : {viewer::LanSeatKind::open, viewer::LanSeatKind::closed, viewer::LanSeatKind::ai,
            viewer::LanSeatKind::open, viewer::LanSeatKind::ai}) {
            require(viewer::select_lan_seat_kind(editable, map, 2, 8, kind, tactical::AiLevel::easy));
            eawr::skirmish::FixtureOptions options; options.map = editable.map; options.slots = editable.slots;
            options.open_slots = editable.open_slots; options.match = editable.match;
            net::LobbySnapshot state; state.game = require(viewer::encode_lan_lobby_game(options));
            const auto decoded = require(viewer::decode_lan_lobby_game(state));
            expect(viewer::lan_seat_kind(decoded, 2) == kind, "every row-2 vacancy transition round trips without becoming a different controller");
            if (kind == viewer::LanSeatKind::ai) expect(decoded.slots->back().difficulty == tactical::AiLevel::easy, "AI level remains part of the same row");
        }
        eawr::skirmish::FixtureOptions options; options.map = initial.map; options.match = initial.match; options.slots = initial.slots;
        net::LobbySnapshot state; state.game = require(viewer::encode_lan_lobby_game(options));
        state.seats = {{0, sim::SeatId{1}, 1, {"host", "Rebel", 0, 0, 1}, false}};
        auto departed = require(viewer::decode_lan_lobby_game(state));
        expect(departed.slots->size() == 1 && viewer::lan_seat_kind(departed, 2) == viewer::LanSeatKind::open,
            "a departed human leaves an Open row and creates no AI player");
        for (const auto version : {'1', '2'}) {
            state.game.options.front() = static_cast<std::uint8_t>(version);
            const auto old = viewer::decode_lan_lobby_game(state);
            expect(!old && old.error().message.starts_with("Unsupported skirmish staging options version"), "v1 and v2 are refused by name");
        }
        options.open_slots = {1};
        expect(!viewer::encode_lan_lobby_game(options), "a row cannot be both occupied and Open");
        options.open_slots = {4, 3};
        expect(!viewer::encode_lan_lobby_game(options), "vacant rows have one canonical ascending order");
    }
    selection = initial;
    require(viewer::append_lan_ai_row(selection, map, 3, 8, true));
    expect(selection.slots.back().team == 2, "a new planned human owner prefers an unused loaded team");
    selection = initial;
    require(viewer::append_lan_ai_row(selection, map, 3, 8));
    expect(selection.slots.back().team == 1 && selection.slots.back().faction == "Empire" && !selection.slots.back().human,
        "new AI keeps the occupied opposing team's faction instead of taking the first allowed faction");
    require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1)));
    map.teams[0].spawn_capacity = 1; map.teams[1].spawn_capacity = 1;
    selection = initial;
    require(viewer::append_lan_ai_row(selection, map, 3, 8));
    expect(selection.slots.back().team == 2, "a full opposing side falls back to an authored side with a valid free spawn");
    map.teams.pop_back(); selection = initial;
    map.teams[0].spawn_capacity = 2;
    require(viewer::append_lan_ai_row(selection, map, 3, 8));
    expect(selection.slots.back().team == 0 && selection.slots.back().faction == "Rebel",
        "project AI-add default prefers a host-team free marker to opposing overflow");
    map.teams[0].spawn_capacity = 1; selection = initial;
    require(viewer::append_lan_ai_row(selection, map, 3, 8));
    expect(selection.slots.back().team == 1 && selection.slots.back().faction == "Empire",
        "when every side is full, AI-add accepts overflow on the opposing team");
    require(viewer::append_lan_ai_row(selection, map, 4, 8));
    const auto before = require(viewer::encode_lan_lobby_game(require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1)))));
    expect(!viewer::append_lan_ai_row(selection, map, 5, 8), "an AI edit is refused beyond total map capacity");
    expect(require(viewer::encode_lan_lobby_game(require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1))))) == before,
        "a refused AI edit preserves the exact prior selection bytes");
}

void lobby_refusal_and_placeholder_contracts() {
    stage = "typed admission and collision-free temporary names";
    for (const auto& [reason, code] : std::vector<std::pair<net::LobbyReason, std::string>>{
        {net::LobbyReason::full, "EAWR-NET-1604"}, {net::LobbyReason::frozen, "EAWR-NET-1607"},
        {net::LobbyReason::authority, "EAWR-NET-1601"}, {net::LobbyReason::resource_limit, "EAWR-NET-1614"}}) {
        const auto error = net::lobby_diagnostic(reason, "admission refusal");
        const auto mapped = viewer::lan_lobby_reason(error);
        expect(mapped == reason && net::lobby_diagnostic(mapped, "").code == code, "admission reverse mapping preserves the real typed reason");
    }
    eawr::testing::FakeNetwork network(3);
    eawr::testing::FakeTransport transport(network, 0);
    net::LobbyHooks hooks;
    hooks.validate = [](const auto&, bool) { return eawr::core::Result<void>::success(); };
    hooks.offer = [](const auto&, const std::vector<net::SetupSeat>& seats) { auto value = offer(); value.seats = seats; return eawr::core::Result<net::SetupOffer>::success(std::move(value)); };
    hooks.setup = [](net::MessageTransport&, const auto&) { return eawr::core::Result<net::LobbySetupContext>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "unused test provider")); };
    hooks.remove_peer = [](net::PeerIndex, net::LobbyReason) {};
    auto session = require(net::LobbySession::host(transport, "reason-test", 2, {"host", "Rebel", 0, 0, 1}, 1, {setup().skirmish->map, {}}, hooks));
    require(session.admit(1, "first-edge", {"first", "Empire", 1, 1, 2}, 2, session.snapshot().revision));
    const auto full = session.admit(2, "third-edge", {"third", "Empire", 1, 1, 2}, 2, session.snapshot().revision);
    expect(!full && viewer::lan_lobby_reason(full.error()) == net::LobbyReason::full, "actual full-room admission retains 1604");
    require(session.close());
    const auto frozen = session.admit(2, "third-edge", {"third", "Empire", 1, 1, 2}, 2, session.snapshot().revision);
    expect(!frozen && viewer::lan_lobby_reason(frozen.error()) == net::LobbyReason::frozen, "actual frozen-room admission retains 1607");
    net::LobbySnapshot room;
    for (const auto& name : {"2", "2-1", "2-2"}) { net::LobbySeat seat; seat.selection.name = name; room.seats.push_back(std::move(seat)); }
    expect(viewer::lan_admission_name(room, 1) == "2-3", "temporary admission label steps past every occupied name");
}

void terminal_lobby_commands_are_inert() {
    stage = "terminal staging leave and commands";
    const auto initial = setup();
    eawr::skirmish::FixtureOptions selected; selected.map = initial.skirmish->map; selected.match = initial.skirmish->match;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{{1, "Rebel", 1, true, {}, 2}, {2, "Empire", 2, true, {}, 4}};
    eawr::skirmish::SetupMap map; map.path = *selected.map; map.metadata.capacity = 2;
    map.metadata.new_markers = true; map.metadata.start_positions.emplace(3);
    map.teams = {{1, {"Rebel"}, 1}, {2, {"Empire"}, 1}};
    const viewer::LanLobbyBuilder builder = [initial](const auto&) { return eawr::core::Result<tactical::TacticalSetup>::success(initial); };
    auto host = make_match(), join = make_match();
    host->configure_lobby("Host", 2, {map}, builder); join->configure_lobby("Missing map", 4, {}, builder);
    host->host(0); join->join("127.0.0.1:" + std::to_string(host->view().port));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (join->view().phase != viewer::LanPhase::ended) {
        expect(std::chrono::steady_clock::now() < deadline, "missing-map refusal deadline");
        host->pump_setup(selected); join->pump_setup({}); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(join->view().diagnostic_code == net::lobby_diagnostic(net::LobbyReason::invalid_choice, "").code, "local map refusal has a typed lobby diagnostic");
    join->lobby_leave();
    expect(!join->lobby_ready(true) && !join->lobby_chat("after failure") && !join->lobby_kick(0)
        && !join->lobby_select({"name", "Rebel", 2, 1, 1}) && !join->lobby_game(selected), "terminal lobby mutators cannot touch drain-owned I/O");
    join->cancel(); host->cancel();
}

void close_one_mesh_edge() {
    stage = "targeted close-peer";
    net::TcpOptions policy; policy.peers = 3; policy.build_identity = eawr::core::build_identity();
    auto host = require(net::TcpTransport::host("127.0.0.1", 0, policy));
    require(host->close_peer(1)); // Vacant admission is a no-op.
    expect(!host->close_peer(0), "close_peer refuses self with a typed result");
    auto first = require(net::TcpTransport::connect("127.0.0.1", host->port(), policy));
    auto second = require(net::TcpTransport::connect("127.0.0.1", host->port(), policy));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const auto fully_open = [&] {
        for (const auto* transport : {host.get(), first.get(), second.get()})
            for (net::PeerIndex peer = 0; peer < transport->peer_count(); ++peer)
                if (transport->link(peer) != net::LinkState::open) return false;
        return true;
    };
    while (!fully_open()) {
        expect(std::chrono::steady_clock::now() < deadline, "close-peer mesh deadline");
        (void)host->poll(); (void)first->poll(); (void)second->poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto removed_identity = host->edge_identity(1);
    auto* removed = first->self() == 1 ? first.get() : second.get();
    require(removed->send(0, Bytes{92}));
    bool before_close = false;
    while (!before_close) {
        expect(std::chrono::steady_clock::now() < deadline, "pre-close frame deadline");
        (void)removed->poll(); (void)(removed == first.get() ? second->poll() : first->poll());
        if (auto frame = host->poll()) { expect(frame->from == 1 && frame->frame == Bytes{92}, "known frame arrives from the soon-removed edge"); before_close = true; }
    }
    require(removed->send(0, Bytes{93})); (void)removed->poll();
    require(host->close_peer(1)); require(host->close_peer(1));
    expect(removed_identity != 0 && host->edge_identity(1) == 0, "only admitted open edge has an identity");
    for (unsigned i = 0; i < 10; ++i) {
        if (auto frame = host->poll()) expect(frame->from != 1, "locally closed peer never yields queued or late application frames");
        (void)removed->poll();
    }
    expect(host->link(1) == net::LinkState::closed && host->link(2) == net::LinkState::open, "ordered close affects exactly the selected edge");
    auto* survivor = first->self() == 2 ? first.get() : second.get();
    require(survivor->send(0, Bytes{94}));
    bool after_close = false;
    while (!after_close) {
        expect(std::chrono::steady_clock::now() < deadline, "post-close live polling deadline");
        (void)survivor->poll(); (void)removed->poll();
        if (auto frame = host->poll()) { expect(frame->from == 2 && frame->frame == Bytes{94}, "only the surviving edge yields application input after close"); after_close = true; }
    }
    require(host->send(2, Bytes{91}));
    bool received = false;
    while (!received) {
        expect(std::chrono::steady_clock::now() < deadline, "surviving edge continues after targeted close");
        (void)host->poll(); (void)(survivor == first.get() ? second->poll() : first->poll());
        if (auto frame = survivor->poll()) received = frame->frame == Bytes{91};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

#include "galactic_support.hpp"
#include "ability_fixture.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/presentation/audio/galactic.hpp"
#include <array>
#include <chrono>

using namespace galactic_test;
using eawr::platform::LiveGameSession;
using namespace std::chrono_literals;

void approach_feedback() {
    using Notice = galactic::GalacticFeedbackNotice;
    eawr::platform::GalacticApproachThrottle clock;
    expect(!clock.admit(1, 4999) && clock.admit(1, 5000), "approach guard admits exactly 5000ms");
    expect(!clock.admit(1, 9999) && clock.admit(2, 5000) && clock.admit(1, 10000),
        "approach clocks are per player and stamp only admitted warnings");
    expect(!clock.admit(0, 20000) && !clock.admit(1, 0), "null recipient and regressed clocks stay silent");
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto control : {0, 1, 2, 3, 4}) {
        auto setup = ability_fixture::base_setup();
        const auto target = setup.planets[0].id, source = setup.planets[1].id;
        const auto attacker = setup.fleets[1].id;
        setup.fleets[1].orbit = source;
        for (auto& actor : setup.census->objects) if (actor.owner == 2) actor.planet = source;
        auto& political = ability_fixture::add_control(setup);
        political.types = {{1000, 0, false, false}};
        for (const auto& planet : setup.planets) {
            galactic::ControlPlanet row; row.planet = planet.id; row.type = 1000;
            row.surface_accessible = row.retains_residual = true; political.planets.push_back(row);
        }
        ability_fixture::add_nested_creation(setup, {1000});
        ability_fixture::add_observer(setup, control == 1 ? 2 : 1, {1000});
        auto& movement = setup.movement.emplace();
        movement.mode_modifier = ability_fixture::whole(1);
        movement.types = {{1000, ability_fixture::whole(10000), ability_fixture::whole(100), 0, true, false, false, false}};
        for (const auto player : ability_fixture::all_players) movement.players.push_back({player, ability_fixture::whole(1), {}, false});
        for (const auto& planet : setup.planets) movement.planets.push_back({planet.id, {}, false});
        for (const auto player : ability_fixture::all_players) for (const auto& planet : setup.planets)
            movement.access.push_back({player, planet.id, true, false});
        if (control == 2) for (auto& relation : setup.production->nested_creation->relations) relation.enemy = false;
        if (control == 3) for (auto& actor : setup.census->objects) if (actor.owner == 2) actor.abilities.combat_skip = true;
        if (control == 4) {
            setup.fleets[0].orbit = source;
            for (auto& actor : setup.census->objects) if (actor.owner == 1) actor.planet = source;
        }
        auto made = galactic::GalacticSession::create(setup);
        expect(bool(made), "approach fixture is valid");
        if (!made) { std::cerr << eawr::core::format_diagnostic(made.error()) << '\n'; continue; }
        auto game = std::move(made).value();
        auto silent = galactic::GalacticSession::create(setup).value();
        std::vector<Notice> notices;
        game.set_feedback_notice_handler([&](const auto& notice) { notices.push_back(notice); });
        const galactic::GameCommand move{{0, galactic::SeatId{2}, 1}, galactic::MoveFleet{attacker, {source, target}, {}}};
        expect(bool(game.submit(move)) && bool(silent.submit(move)), "approach order queues");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto tick = game.step(executor).value();
        expect(std::any_of(tick.snapshot->events().begin(), tick.snapshot->events().end(), [](const auto& e) {
            return e.kind == galactic::GalacticEventKind::movement_begin;
        }), "approach controls all perform an admitted departure");
        expect(tick.state_hash.get() == silent.step(executor).value().state_hash.get(), "approach feedback is presentation-free deterministic state");
        expect(notices.size() == (control == 0 ? 1U : 0U), "approach requires exact local allegiance, enemy, non-skip and local orbit");
        if (!notices.empty()) expect(notices[0].kind == Notice::Kind::enemy_approaching
            && notices[0].planet == target && notices[0].player == 1 && notices[0].preceding_events >= 2,
            "enroute commit carries recipient/final planet after its departure events");
        if (control == 0) {
            LiveGameSession::Options options; options.workers = workers; options.pacing = LiveGameSession::Pacing::driven;
            auto live = LiveGameSession::start(setup, options).value();
            live->submit(move); live->advance_to(4);
            expect(live->wait_for(4, 10s), "approach feed advances past skipped presentation frames");
            const auto batch = live->galactic_events_after(0);
            std::vector<eawr::platform::LiveGalacticEvent> retained;
            for (const auto& published : batch.events) if (published.feedback
                && published.feedback->kind == Notice::Kind::enemy_approaching) retained.push_back(published);
            expect(retained.size() == 1, "one qualified approach remains retained after arrival");
            if (!retained.empty()) {
                const auto suffix = live->galactic_events_after(retained.front().cursor);
                expect(std::none_of(suffix.events.begin(), suffix.events.end(), [](const auto& e) {
                    return e.feedback && e.feedback->kind == Notice::Kind::enemy_approaching;
                }), "exclusive cursor consumes approach without semantic retries");
            }
            expect(live->galactic_events_after(batch.through).events.empty(), "approach batch does not repeat");
            live->stop();
        }
        for (int i = 0; i < 4; ++i) (void)game.step(executor).value();
        expect(notices.size() == (control == 0 ? 1U : 0U), "continued enroute and clearing on arrival do not repeat approach");
    }
}

galactic::GalacticSetup feedback_setup(std::uint32_t local) {
    auto setup = ability_fixture::base_setup();
    auto& host = *setup.ability_host;
    host.profiles[0].has_market = false;
    host.profiles[0].corruption = galactic::CorruptionAbilityProfile{"CUSTOM_CORRUPT", true};
    host.activated_corruption_abilities = {"CUSTOM_CORRUPT"};
    setup.economy_corruption->planets[0].corruption = galactic::corruption_fixture::Type::none;
    setup.economy_corruption->players[1].benefits = true;
    auto& control = ability_fixture::add_control(setup);
    control.types = {{1000, 0, false, true}};
    for (std::size_t i = 0; i < setup.planets.size(); ++i) {
        const auto& planet = setup.planets[i];
        const auto type = 3000 + i;
        setup.census->types.push_back({type, {}, 0});
        galactic::ControlPlanet row;
        row.planet = planet.id; row.type = type;
        row.surface_accessible = true; row.retains_residual = true;
        row.capture_reward = ability_fixture::whole(17);
        control.planets.push_back(row);
        galactic::CorruptionPlanet location;
        location.planet = planet.id; location.type = type; location.retains_influence = true;
        location.slots[0].type = galactic::corruption_fixture::Type::piracy;
        host.corruption_planets.push_back(location);
    }
    for (const auto id : ability_fixture::all_players) {
        galactic::CorruptionPlayer player; player.player = id;
        host.corruption_players.push_back(player);
    }
    if (local) ability_fixture::add_observer(setup, local, {1000, 3000, 3001, 3002, 3003});
    return setup;
}

void corruption_feedback() {
    using Notice = galactic::GalacticFeedbackNotice;
    using Type = galactic::corruption_fixture::Type;
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto local : {0U, 1U, 2U, 3U})
        for (const bool removed : {false, true}) {
            auto setup = feedback_setup(local);
            if (removed) setup.economy_corruption->planets[0].corruption = Type::piracy;
            auto game = galactic::GalacticSession::create(setup).value();
            auto silent = galactic::GalacticSession::create(setup).value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<Notice> notices;
            game.set_feedback_notice_handler([&](const auto& notice) { notices.push_back(notice); });
            const auto target = removed ? Type::none : Type::piracy;
            expect(game.begin_corruption(setup.planets[0].id, target, {}).value()
                && silent.begin_corruption(setup.planets[0].id, target, {}).value(), "due corruption transition starts");
            expect(notices.empty(), "starting a transition does not request completion feedback");
            for (int i = 0; i < 4; ++i)
                expect(game.step(executor).value().state_hash.get() == silent.step(executor).value().state_hash.get(),
                    "feedback subscription changes no deterministic state or continuation");
            expect(notices.size() == (local == 1 || local == 2 ? 1U : 0U),
                "only local owner or otherwise local beneficiary hears one completion, never other observers");
            if (!notices.empty()) {
                expect(notices[0].player == local && notices[0].planet == setup.planets[0].id
                    && notices[0].kind == (removed ? Notice::Kind::corruption_removed : Notice::Kind::planet_corrupted),
                    "completion retains its actual recipient, planet and resulting type");
            }
        }
    // When local owner and beneficiary coincide, precedence still emits once.
    auto setup = feedback_setup(1);
    setup.economy_corruption->players[0].benefits = true;
    setup.economy_corruption->players[1].benefits = false;
    auto game = galactic::GalacticSession::create(setup).value();
    std::size_t notices{};
    game.set_feedback_notice_handler([&](const auto&) { ++notices; });
    expect(game.begin_corruption(setup.planets[0].id, Type::piracy, {}).value(), "overlapping recipient transition starts");
    eawr::sim::InlineExecutor executor;
    (void)game.step(executor).value();
    expect(notices == 1, "owner/beneficiary overlap is one notification");
}

void native_staging_feedback() {
    using Notice = galactic::GalacticFeedbackNotice;
    using Type = galactic::corruption_fixture::Type;
    const std::array<galactic::PlayerId, 1> players{2};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto setup = feedback_setup(2);
        setup.fleets[0].orbit = setup.planets[1].id;
        setup.fleet_control->types[0].transport = true;
        const auto fleet = setup.fleets[1].id;
        const auto actor = setup.fleets[1].members.front().id;
        const auto planet = setup.planets[0].id;
        auto game = galactic::GalacticSession::create(setup).value();
        std::vector<Notice> notices;
        game.set_feedback_notice_handler([&](const auto& notice) { notices.push_back(notice); });
        const auto before = game.canonical_state_bytes();
        auto barrier = std::move(game.begin_native_barrier(players, 16)).value();
        barrier->select_player(2).value();
        expect(game.admit_native_fleet(2, galactic::RemoveFleetMembers{fleet, {actor}}, false).value().admitted,
            "native capture fixture stages the transport removal");
        expect(game.land_native_transport(2, actor, planet).value().linked,
            "native capture fixture stages an actual surface arrival");
        expect(notices.empty() && game.canonical_state_bytes() == before,
            "staged surface arrival emits no committed capture notice or world mutation");
        expect(!game.step(executor) && notices.empty() && game.canonical_state_bytes() == before,
            "world service cannot publish a capture while native staging is open");
        barrier->begin_tail().value(); barrier->commit().value(); barrier.reset();
        expect(notices.empty(), "native action replay itself does not perform political capture service");
        for (unsigned tick = 0; tick < 4; ++tick) (void)game.step(executor).value();
        expect(game.snapshot()->planets()[0].owner == 2 && notices.size() == 1
            && notices[0].kind == Notice::Kind::capture_reward && notices[0].planet == planet,
            "one committed native-arrival capture emits exactly one reward notice");

        auto corruption = galactic::GalacticSession::create(feedback_setup(2)).value();
        notices.clear();
        corruption.set_feedback_notice_handler([&](const auto& notice) { notices.push_back(notice); });
        expect(corruption.begin_corruption(planet, Type::piracy, {}).value(), "due corruption fixture starts before native staging");
        const auto due = corruption.canonical_state_bytes();
        barrier = std::move(corruption.begin_native_barrier(players, 16)).value();
        barrier->select_player(2).value();
        (void)corruption.add_credits(2, ability_fixture::whole(1), ability_fixture::whole(1)).value();
        expect(notices.empty() && !corruption.step(executor) && corruption.canonical_state_bytes() == due,
            "staged cash cannot run due corruption service or emit its notice");
        barrier->begin_tail().value(); barrier->commit().value(); barrier.reset();
        expect(notices.empty(), "closing native staging does not impersonate corruption completion");
        (void)corruption.step(executor).value();
        expect(notices.size() == 1 && notices[0].kind == Notice::Kind::planet_corrupted && notices[0].planet == planet,
            "due corruption emits once after authoritative world service commits");
    }
}

void retained_feedback() {
    namespace audio = eawr::presentation::audio;
    using Notice = galactic::GalacticFeedbackNotice;
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto local : {0U, 1U, 2U, 3U})
        for (const bool capped : {false, true}) {
        auto setup = feedback_setup(local);
        setup.fleets[0].orbit = setup.planets[1].id;
        // LandFleet admits transport members; the shared corruption fixture's
        // ordinary actor is not one until this capture scenario supplies it.
        setup.fleet_control->types[0].transport = true;
        if (capped) setup.fiscal->credit_cap_per_planet = {};
        auto live = LiveGameSession::start(setup, {.workers = workers, .pacing = LiveGameSession::Pacing::driven}).value();
        const auto planet = setup.planets[0].id;
        live->submit({{0, galactic::SeatId{2}, 1}, galactic::LandFleet{setup.fleets[1].id, planet}});
        live->advance_to(4); expect(live->wait_for(4, 10s), "first capture reaches retained feedback");
        expect(live->frame().latest->player_economy()->players[1].credits == ability_fixture::whole(capped ? 0 : 1017),
            "first capture actually commits its authored credit award");
        const auto batch = live->galactic_events_after(0);
        expect(std::any_of(batch.events.begin(), batch.events.end(), [&](const auto& row) {
            return !row.feedback && row.event.kind == galactic::GalacticEventKind::command_executed
                && row.event.subject == setup.fleets[1].id && row.event.value == planet;
        }), "first-capture fixture admits the real transport landing command");
        expect(live->frame().latest->planets()[0].owner == 2, "the landing actually transfers control before testing reward feedback");
        const std::vector<audio::Field> reward_audio{{"SFXEvent_Planet_Capture_Bonus_Reward", "custom_capture"}};
        std::size_t rewards{};
        std::uint64_t ownership_cursor{};
        for (const auto& published : batch.events)
            if (!published.feedback && published.event.kind == galactic::GalacticEventKind::planet_owner_changed
                && published.event.subject == planet) ownership_cursor = published.cursor;
        for (const auto& published : batch.events) if (published.feedback) {
            ++rewards;
            expect(ownership_cursor && ownership_cursor < published.cursor, "ownership feedback precedes its capture award");
            expect(published.feedback->kind == Notice::Kind::capture_reward && published.feedback->player == 2,
                "only the credited first-capture owner is published");
            const auto cue = audio::galactic_event_sound(published.event, local, {}, {}, false, {}, published.feedback);
            expect(cue == audio::GalacticSound::capture_bonus, "retained award selects the global capture-bonus cue");
            expect(cue && audio::galactic_sound_event(*cue, {}, reward_audio) == "custom_capture",
                "capture sound follows renamed global data");
        }
        expect(rewards == (local == 2 ? 1U : 0U), "first-capture feedback is exact-local only");
        live->advance_to(8); expect(live->wait_for(8, 10s), "captured continuation advances");
        const auto later = live->galactic_events_after(batch.through);
        expect(std::none_of(later.events.begin(), later.events.end(), [](const auto& row) { return row.feedback.has_value(); }),
            "a settled award never repeats, including after skipped presentation frames");
        live->stop();
    }
    // Real prepared corruption -> due completion -> retained recipient -> cue.
    auto setup = feedback_setup(2);
    auto live = LiveGameSession::start(setup, {.workers = 2, .pacing = LiveGameSession::Pacing::driven}).value();
    live->submit({{0, galactic::SeatId{2}, 1}, galactic::PrepareGalacticAbility{
        setup.fleets[1].members[0].id, setup.planets[0].id, galactic::AbilityKind::corruption}});
    live->advance_to(1); expect(live->wait_for(1, 10s), "live corruption preparation completes");
    const auto snapshot = live->frame().latest;
    const auto* menu = snapshot->corruption_menu();
    expect(menu && !menu->choices.empty() && menu->choices[0].live_prepared, "prepared corruption is observable");
    if (menu && !menu->choices.empty() && menu->choices[0].live_prepared) {
        const auto prepared = *menu->choices[0].live_prepared;
        live->submit({{1, galactic::SeatId{2}, 2}, galactic::ExecutePreparedAbility{prepared}});
        live->advance_to(2); expect(live->wait_for(2, 10s), "live corruption starts");
        const auto before = live->galactic_events_after(0);
        expect(std::none_of(before.events.begin(), before.events.end(), [](const auto& row) { return row.feedback.has_value(); }),
            "ability success does not masquerade as due corruption completion");
        live->advance_to(160); expect(live->wait_for(160, 10s), "live corruption reaches completion");
        const auto after = live->galactic_events_after(before.through);
        std::size_t completed{};
        for (const auto& row : after.events) if (row.feedback) {
            ++completed;
            expect(audio::galactic_event_sound(row.event, 2, {}, {}, false, {}, row.feedback) == audio::GalacticSound::planet_corrupted,
                "retained due completion selects corrupted cue");
        }
        expect(completed == 1 && live->galactic_events_after(after.through).events.empty(),
            "corruption completion is retained once with an exclusive cursor");
    }
    live->stop();
}

void construction_unlock_cues() {
    namespace audio = eawr::presentation::audio;
    const auto whole = [](std::int64_t value) { return eawr::sim::math::Fixed::from_raw(value * eawr::sim::math::Fixed::scale); };
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto local : {0U, 1U, 2U}) for (const bool unlock : {false, true}) {
        auto setup = galactic::synthetic_galaxy({.seed = 21, .planets = 4, .players = 2,
            .fleets_per_player = 1, .members_per_fleet = 1});
        setup.rules.fiscal_cycle_frames = 1000;
        setup.rules.maximum_fleet_movement_distance = whole(10000);
        setup.player_economy = galactic::PlayerEconomySetup{{{1, whole(1000), whole(10000), 2, 5},
            {2, whole(1000), whole(10000), 2, 5}}};
        setup.fiscal = galactic::FiscalSetup{whole(10000), {{1, true, whole(1)}, {2, true, whole(1)}}, {}};
        for (const auto& planet : setup.planets) setup.fiscal->planets.push_back({planet.id, {}});
        setup.production.emplace();
        setup.production->maximum_human_queue = 5;
        setup.fleet_control = galactic::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
        for (const auto& player : setup.players) {
            const bool human = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& seat) {
                return seat.player == player.player_id && seat.kind == eawr::sim::SeatKind::human;
            });
            setup.production->players.push_back({player.player_id, human, !human, human, whole(100), {}, {}, {}, {}});
        }
        // Two newly unlocked registry types, absent from every producer's offers.
        // Equal-tech, unaffiliated and non-slicer controls never qualify.
        setup.production->technology_types = {{8100, 2, unlock, true, {1}}, {8101, 0, unlock, true, {1}},
            {8102, 3, true, true, {1}}, {8103, 0, true, true, {2}}, {8104, 0, true, false, {1}}};
        for (const auto& planet : setup.planets) {
            galactic::PlanetProduction producer;
            producer.planet = planet.id; producer.target_owner = producer.ground_occupier = planet.owner;
            producer.enabled = producer.surface_accessible = true;
            producer.ground_slots = producer.space_slots = 3; producer.maximum_ground_forces = 10;
            galactic::ProductionOffer offer;
            offer.type = 8200; offer.kind = galactic::ProductionKind::technology;
            offer.price = offer.authored_price = whole(1); offer.seconds = {};
            offer.tech = 2; offer.target_tech = 3; offer.affiliated = offer.direct_build_allowed = true;
            producer.offers.push_back(offer);
            offer.type = 8201; offer.tech = 3; // repeat the same-level setter through another authored research
            producer.offers.push_back(offer); setup.production->planets.push_back(producer);
        }
        if (local) {
            auto& observer = setup.observer.emplace();
            observer.local_player = local; observer.reveal_delay_seconds = whole(1);
            observer.players = {{1, false, false, 0, 1, {}}, {2, false, false, 0, 1, {}}};
            observer.types = {{1000, false, false, false}};
            for (const auto& planet : setup.planets) {
                galactic::ObserverPlanet facts;
                facts.planet = planet.id; facts.allegiance = facts.target_owner = planet.owner;
                facts.current_owner = facts.current_controller = planet.owner;
                observer.planets.push_back(facts);
                for (const auto player : {1U, 2U}) observer.records.push_back({player, planet.id, true, 0, 0, 0, {}});
            }
            std::sort(observer.records.begin(), observer.records.end(), [](const auto& a, const auto& b) {
                return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
            });
        }
        const auto planet = std::find_if(setup.planets.begin(), setup.planets.end(), [](const auto& p) { return p.owner == 1; })->id;
        auto live = LiveGameSession::start(setup, {.workers = workers, .pacing = LiveGameSession::Pacing::driven});
        expect(bool(live), "unlock cue fixture starts");
        if (!live) { std::cerr << live.error().message << '\n'; continue; }
        auto& session = *live.value();
        const auto purchase = galactic::ProductionCommand{planet, 8200, galactic::ProductionAction::buy,
            galactic::ProductionDomain::ground, 0};
        session.submit({{0, galactic::SeatId{1}, 1}, purchase});
        session.advance_to(4); expect(session.wait_for(4, 10s), "technology completion reaches the retained feed");
        expect(session.frame().latest->player_economy()->players[0].current_tech == 3,
            "the real production command completed its tech assignment");
        const auto batch = session.galactic_events_after(0);
        std::vector<std::string> cues;
        const std::vector<audio::Field> faction{{"SFXEvent_New_Construction_Options_Available", "renamed_unlock_cue"}};
        for (const auto& published : batch.events) {
            if (!published.construction_options_player) continue;
            const auto cue = audio::galactic_event_sound(published.event, local, {}, {}, false,
                published.construction_options_player);
            if (cue) cues.push_back(audio::galactic_sound_event(*cue, faction, {}));
            expect(!audio::galactic_event_sound(published.event, 2, {}, {}, false, published.construction_options_player),
                "another presentation recipient cannot hear the local request");
        }
        expect(cues == (local == 1 && unlock ? std::vector<std::string>{"renamed_unlock_cue"} : std::vector<std::string>{}),
            "two new types produce exactly one authored local cue; no new unlock or nonlocal owner is silent");
        expect(session.galactic_events_after(batch.through).events.empty(), "consumed unlock cursor does not replay");
        auto repeated_purchase = purchase; repeated_purchase.type = 8201;
        session.submit({{4, galactic::SeatId{1}, 2}, repeated_purchase});
        session.advance_to(8); expect(session.wait_for(8, 10s), "repeated technology completion finishes");
        const auto repeated = session.galactic_events_after(batch.through);
        expect(std::any_of(repeated.events.begin(), repeated.events.end(), [](const auto& p) {
            return !p.construction_options_player && p.event.kind == galactic::GalacticEventKind::production_completed
                && p.event.value == 8201;
        }), "same-level research actually completes before asserting silence");
        expect(std::none_of(repeated.events.begin(), repeated.events.end(), [](const auto& p) {
            return p.construction_options_player.has_value();
        }), "already unlocked types do not emit another cue");
        session.stop();
    }
}

void frozen_galaxy_control_events(const galactic::GalacticSetup& initial) {
    namespace campaign = eawr::campaign;
    namespace tactical = eawr::sim::tactical;
    campaign::ConflictRecord entry;
    entry.id = campaign::GameSession::create(initial).value().galaxy().next_entity_id();
    entry.planet = initial.planets.front().id; entry.invader = 1; entry.defender = 2;
    entry.tactical.content_identity = initial.content_identity;
    entry.tactical.first_free_entity_id = entry.id + 1;
    entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    for (const auto& fleet : initial.fleets) {
        const auto member = fleet.members.front();
        entry.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        tactical::UnitState unit;
        unit.entity_id = member.id; unit.type_id = member.type; unit.owner = fleet.owner;
        entry.tactical.units.push_back(unit);
    }
    for (const auto& seat : initial.seats) entry.seats.push_back({seat.seat_id, seat.player});
    tactical::TacticalContent content;
    tactical::DurabilityProfile hull;
    hull.type_id = entry.forces.front().type;
    hull.max_hull = eawr::sim::math::Fixed::from_integer(100).value();
    content.durability.profiles.push_back(hull);
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2}; content.victory.humans = {1};
    LiveGameSession::Options options;
    options.pacing = LiveGameSession::Pacing::driven;
    options.entries.push_back({0, entry, content, campaign::ScriptRequirement::none});
    auto live = LiveGameSession::start(initial, options).value();
    live->advance_to(1);
    expect(live->wait_for(1, 10s), "child entry reaches its readiness gate");
    const auto frozen_tick = live->frame().latest->completed_tick();
    const auto cursor = live->galactic_events_after(0).through;
    auto feed = live->battle_feed(entry.id, galactic::SeatId{1});
    expect(static_cast<bool>(feed), "child supplies a participant feed");
    if (!feed) { live->stop(); return; }
    feed->set_paused(false); live->battle_ready(entry.id);
    live->submit(eawr::platform::LiveGameOrder{galactic::SeatId{1}, galactic::SaveRequest{0}, {}});
    live->advance_to(2);
    expect(live->wait_for(2, 10s), "session control executes during child service");
    expect(live->frame().latest->completed_tick() == frozen_tick, "galaxy tick remains frozen across session control");
    const auto controls = live->galactic_events_after(cursor);
    expect(std::any_of(controls.events.begin(), controls.events.end(), [](const auto& published) {
        return published.event.kind == galactic::GalacticEventKind::save_requested;
    }), "retained output includes a new control snapshot at the same galaxy tick");
    live->advance_to(3);
    expect(live->wait_for(3, 10s), "another child tick advances");
    expect(live->galactic_events_after(controls.through).events.empty(), "frozen snapshot control events are not repeated");
    live->stop();
}

int main() {
    const auto initial = galactic::synthetic_galaxy({.seed = 21, .planets = 8, .players = 2,
        .fleets_per_player = 1, .members_per_fleet = 1, .debug_commands = true});
    auto live = LiveGameSession::start(initial, {.workers = 2, .pacing = LiveGameSession::Pacing::driven}).value();
    const auto planet = initial.planets.front().id;
    const auto owner = initial.planets.front().owner;
    const auto other = owner == 1 ? 2U : 1U;
    live->submit({{0, galactic::SeatId{1}, 0}, galactic::DebugSetPlanetOwner{planet, other}});
    live->submit({{1, galactic::SeatId{1}, 1}, galactic::DebugSetPlanetOwner{planet, owner}});
    live->advance_to(20);
    expect(live->wait_for(20, 10s), "skipped presentation ticks reach the target");
    const auto batch = live->galactic_events_after(0);
    expect(!batch.incomplete && batch.through != 0, "events retained across empty ticks");
    std::vector<eawr::platform::LiveGalacticEvent> ownership;
    for (const auto& published : batch.events)
        if (published.event.kind == galactic::GalacticEventKind::planet_owner_changed) ownership.push_back(published);
    expect(ownership.size() == 2, "both skipped-tick owner cues retained exactly once");
    if (ownership.size() == 2) {
        expect(ownership[0].previous_owner == owner && ownership[0].event.value == other, "first event keeps initial owner");
        expect(ownership[1].previous_owner == other && ownership[1].event.value == owner, "next event keeps committed prior owner");
        const auto suffix = live->galactic_events_after(ownership[0].cursor);
        expect(!suffix.events.empty() && suffix.events.front().cursor > ownership[0].cursor, "cursor is exclusive");
    }
    expect(live->galactic_events_after(batch.through).events.empty(), "consumed cursor does not repeat cues");
    // More events than the presentation retention budget report a gap explicitly.
    for (std::uint64_t index = 0; index < 4100; ++index)
        live->submit({{20, galactic::SeatId{1}, index + 2}, galactic::DebugSetPlanetOwner{planet, index % 2 ? owner : other}});
    live->advance_to(21);
    expect(live->wait_for(21, 10s), "event burst completes");
    const auto gap = live->galactic_events_after(0);
    expect(gap.incomplete && gap.events.size() == 4096, "retention bounded with explicit incomplete marker");
    expect(!live->galactic_events_after(gap.through).incomplete, "caught-up cursor clears incomplete marker");
    live->stop();
    const auto retained = live->galactic_events_after(gap.through - 1);
    expect(retained.events.size() == 1, "event output remains readable after stop");
    frozen_galaxy_control_events(initial);
    construction_unlock_cues();
    corruption_feedback();
    approach_feedback();
    native_staging_feedback();
    retained_feedback();
    return failures == 0 ? 0 : 1;
}

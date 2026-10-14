#include "galactic_support.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/presentation/audio/galactic.hpp"
#include <chrono>

using namespace galactic_test;
using eawr::platform::LiveGameSession;
using namespace std::chrono_literals;

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
    options.entries.push_back({0, entry, content});
    auto live = LiveGameSession::start(initial, options).value();
    live->advance_to(1);
    expect(live->wait_for(1, 10s), "child entry reaches its readiness gate");
    const auto frozen_tick = live->frame().latest->completed_tick();
    const auto cursor = live->galactic_events_after(0).through;
    auto feed = live->battle_feed(entry.id, galactic::SeatId{1});
    expect(static_cast<bool>(feed), "child supplies a participant feed");
    if (!feed) { live->stop(); return; }
    feed->set_paused(false); live->battle_ready(entry.id);
    live->submit(eawr::platform::LiveGameOrder{galactic::SeatId{1}, galactic::SaveRequest{0}, {}, {}});
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
    return failures == 0 ? 0 : 1;
}

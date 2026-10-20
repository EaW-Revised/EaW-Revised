#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "../../apps/viewer/src/galactic/save_input.hpp"
#include "../../apps/viewer/src/galactic/save_dialog_model.hpp"
#include "../../src/platform/galactic/live_save_transport.hpp"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
using namespace galactic_test;
namespace platform = eawr::platform;
namespace campaign = eawr::campaign;
namespace tactical = eawr::sim::tactical;
using Fixed = eawr::sim::math::Fixed;
using namespace std::chrono_literals;
void save_input_contract() {
    using namespace eawr::viewer::galactic;
    const SaveInputProfile profile;
    expect(save_input_action(profile, "F6", true, false, false) == SaveInputAction::quick_save
        && save_input_action(profile, "F5", true, false, false) == SaveInputAction::quick_load,
        "WGS-97 observed profile routes F6 save and F5 load");
    expect(save_input_action({"F8", "F9"}, "F8", true, false, false) == SaveInputAction::quick_save
        && save_input_action({"F8", "F9"}, "F5", true, false, false) == SaveInputAction::none,
        "quick routes follow the configured profile rather than fixed physical keys");
    for (const auto action : {save_input_action(profile, "F6", false, false, false),
        save_input_action(profile, "F6", true, true, false), save_input_action(profile, "F6", true, false, true),
        save_input_action({"F6", "F6"}, "F6", true, false, false)})
        expect(action == SaveInputAction::none, "release/repeat/modifier/colliding bindings do not issue quick input");
    expect(save_menu_enabled({}).open && save_menu_enabled({}).save && save_menu_enabled({}).load,
        "WGS-152 solo galaxy admits Escape/save/load");
    for (auto member : {&SaveMenuAdmission::cinematic, &SaveMenuAdmission::letterbox,
        &SaveMenuAdmission::tutorial, &SaveMenuAdmission::multiplayer_skirmish}) {
        SaveMenuAdmission facts; facts.*member = true; const auto menu = save_menu_enabled(facts);
        expect(!menu.save && !menu.load, "each WGS-152 both-entry gate works independently");
    }
    SaveMenuAdmission child; child.tactical = true;
    expect(save_menu_enabled(child).save && save_menu_enabled(child).load,
        "active solo tactical is not a general refusal");
    SaveMenuAdmission pending; pending.pending_battle = true;
    expect(save_menu_enabled(pending).open && save_menu_enabled(pending).save && save_menu_enabled(pending).load,
        "WGS-P01 pending choice/progress allows both manual recovery routes");
    pending.tactical = true;
    expect(save_menu_enabled(pending).save && save_menu_enabled(pending).load,
        "WGS-P01 pending and tactical publications together still allow solo recovery");
    child.multiplayer_campaign = true;
    expect(!save_menu_enabled(child).save && !save_menu_enabled(child).load, "multiplayer child refuses both");
    child.tactical = false;
    expect(save_menu_enabled(child).save && !save_menu_enabled(child).load, "multiplayer galaxy refuses only load");
    expect(!save_menu_enabled({true}).open, "cinematic blocks Escape activation");
    expect(selected_save_overwrite("Alpha", "Alpha") && !selected_save_overwrite("alpha", "Alpha")
        && !selected_save_overwrite("", "Alpha"), "WGS-157 selected description comparison is exact");
    expect(reserved_save_name("[AutoSave]") && reserved_save_name("[QuickSave]")
        && !reserved_save_name("[Quick Save]"), "reserved description identities are exact");
    expect(save_list_key("[AutoSave]", 2026, 10, 10, 12, 0, 0) == 0
        && save_list_key("[QuickSave]", 2026, 10, 10, 12, 0, 0) == 1,
        "WGS-156 reserved keys do not depend on file time");
    expect(save_list_key("A", 2026, 10, 10, 12, 0, 1) < save_list_key("B", 2026, 10, 10, 12, 0, 0)
        && save_list_key("A", 2026, 10, 10, 12, 0, 0) == save_list_key("B", 2026, 10, 10, 12, 0, 0),
        "ordinary newest first and equal-time rows have no filename key");

}
using eawr::testing::result_diagnostic::take;
galactic::GalacticSetup setup() {
    galactic::SyntheticGalaxyOptions spec; spec.planets = 8; spec.players = 2;
    spec.fleets_per_player = 1; spec.members_per_fleet = 2; spec.debug_commands = true;
    auto result = galactic::synthetic_galaxy(spec);
    result.seats[1].kind = galactic::SeatKind::ai; result.seats[1].human_side_id = 0;
    return result;
}
platform::LiveGameSession::Options options(std::size_t workers, const std::string& directory) {
    platform::LiveGameSession::Options value;
    value.workers = workers; value.pacing = platform::LiveGameSession::Pacing::driven;
    value.saves.directory = directory; return value;
}
void retained_invasion_input() {
    const auto initial = setup();
    platform::save_detail::Input input;
    // GCMP-04: a deferred invasion acceptance is an ordinary typed order; no side metadata carries it.
    input.orders = {{{1}, galactic::InvasionAccept{90}, 5}, {{1}, galactic::ResumeGalaxy{}, 6}};
    auto section = take(platform::save_detail::encode_input(initial, 4, input));
    expect(section.version == 3, "the live-input section is version 3 (invasion intent metadata retired)");
    eawr::checkpoint::Checkpoint saved;
    saved.header.identities = eawr::checkpoint::game_identities(initial);
    saved.header.seed = initial.seed; saved.header.completed_tick = 4;
    saved.sections.push_back(section);
    const auto restored = take(platform::save_detail::decode_input(saved));
    expect(restored.orders.size() == 2 && restored.orders[0].tick == 5
        && restored.orders[0].payload == galactic::CommandPayload{galactic::InvasionAccept{90}}
        && restored.orders[1].payload == galactic::CommandPayload{galactic::ResumeGalaxy{}},
        "saved deferred invasion acceptance round-trips as its typed command beside ordinary input");
    saved.sections.front().bytes.pop_back();
    expect(!platform::save_detail::decode_input(saved), "a truncated deferred-order blob refuses the save");
}
std::uint64_t save(platform::LiveGameSession& live, std::string name, bool quick = false,
    bool wait_before_poll = false) {
    // A paused owner may service the save as soon as request_save wakes it.
    // Fix the expected frontier before admission, while the driven owner is idle.
    const auto target = live.completed_tick() + 1;
    platform::LiveSaveRequest request; request.seat = {1}; request.name = std::move(name); request.overwrite = true;
    request.quick = quick;
    request.presentation_hints = {7, 9};
    const auto token = live.request_save(std::move(request));
    if (wait_before_poll) {
        expect(live.wait_save(token, 5s), "paused save completes before the caller polls");
        expect(live.completed_tick() == target, "completed paused save publishes its admitted frontier");
    }
    const auto immediate = live.terminal_save_outcome(token);
    expect(!live.terminal_save_outcome(0) && (!immediate || (immediate->phase != platform::LiveSavePhase::requested
        && immediate->phase != platform::LiveSavePhase::captured)),
        "poll exposes only terminal outcomes, including when the live worker completes immediately");
    live.advance_to(target);
    expect(live.wait_for(target, 5s) && live.wait_save(token, 5s), "save reaches actual disk completion");
    const auto terminal = live.terminal_save_outcome(token);
    expect(terminal && terminal->token == token && terminal->phase == platform::LiveSavePhase::completed,
        "terminal poll returns actual completed publication");
    const auto outcomes = live.save_outcomes();
    const auto found = std::find_if(outcomes.begin(), outcomes.end(), [token](const auto& row) { return row.token == token; });
    expect(found != outcomes.end() && found->phase == platform::LiveSavePhase::completed
        && found->completed_tick == target, "save outcome separates capture from completed file");
    return token;
}
void galaxy_roundtrip(std::size_t workers, const std::string& directory) {
    auto initial = setup(); auto live = take(platform::LiveGameSession::start(initial, options(workers, directory)));
    live->advance_to(4); expect(live->wait_for(4, 5s), "live galaxy reaches save boundary");
    live->submit(platform::LiveGameOrder{{1}, galactic::DebugSetPlanetOwner{initial.planets[1].id, 2}, 9});
    live->submit(galactic::GameCommand{{11, {2}, 20}, galactic::SetSpeed{false, Fixed::from_raw(2 * Fixed::scale)}});
    expect(!live->save_menu_facts().pending_battle && !live->save_menu_facts().tactical,
        "idle admission copies only session-owned mode facts");
    (void)save(*live, "galaxy"); const auto barrier = live->completed_tick();
    auto prepared = take(live->prepare_load("galaxy", false));
    expect(prepared->completed_tick() == barrier && prepared->presentation_hints() == std::vector<std::uint8_t>({7, 9}),
        "opaque prepared owner retains completed frontier and optional hints");
    auto restored = take(platform::LiveGameSession::start(std::move(prepared), options(workers, directory)));
    expect(restored->completed_tick() == barrier && restored->frame().latest->sha256() == live->frame().latest->sha256(),
        "first restored live frame is the saved galaxy, without startup or a tick-zero publication");
    for (auto* owner : {live.get(), restored.get()}) {
        owner->submit(platform::LiveGameOrder{{1}, galactic::DebugSetPlanetOwner{initial.planets[2].id, 2}, {}});
        owner->advance_to(14); expect(owner->wait_for(14, 5s), "continued input runs after runtime restore"); owner->stop();
    }
    const auto a = live->tick_hashes(), b = restored->tick_hashes();
    expect(std::vector<std::string>(a.begin() + static_cast<std::ptrdiff_t>(barrier), a.end()) == b,
        "continued hashes preserve held future input and deferred seat sequences");
    expect(live->record().commands == restored->record().commands && restored->rejected_orders().empty(),
        "restored live command keys equal uninterrupted keys");
    const auto lifetime = live->lifetime(); const auto before = live->frame().latest;
    { std::ofstream damaged(std::filesystem::path(directory) / "corrupt.eawrsave", std::ios::binary); damaged << "corrupt"; }
    expect(!live->prepare_load("corrupt") && !live->prepare_load("missing")
        && !lifetime.expired() && live->frame().latest == before, "bad/missing load preserves original owner/publication");
    auto other = initial; other.content_identity[0] ^= 1;
    auto wrong = take(platform::LiveGameSession::start(other, options(workers, directory)));
    expect(!wrong->prepare_load("galaxy") && wrong->completed_tick() == 0, "wrong identity refuses without replacement");
    wrong->stop();
    const auto slots = take(live->save_slots());
    expect(std::any_of(slots.begin(), slots.end(), [barrier](const auto& row) { return row.name == "galaxy" && row.description == "galaxy" && row.last_write_seconds.has_value()
        && row.completed_tick == barrier && !row.diagnostic; }),
        "save list metadata describes the completed file");
    expect(!live->delete_save("../galaxy") && !live->prepare_load("../galaxy"), "repository forbids paths as slot names");
    expect(bool(live->delete_save("corrupt")), "delete removes only the selected slot");
}
void discovery_roundtrip(std::size_t workers, const std::string& directory) {
    for (const bool paused : {false, true}) {
        auto initial = setup();
        auto& observer = initial.observer.emplace(); observer.local_player = 1;
        observer.reveal_delay_seconds = take(Fixed::from_decimal("0.5"));
        for (const auto& player : initial.players) {
            galactic::ObserverPlayer row; row.player = player.player_id; row.ai = row.player != 1;
            row.allies = {row.player}; row.default_intelligence = galactic::bit(galactic::Intelligence::ownership);
            observer.players.push_back(row);
        }
        for (const auto& planet : initial.planets) {
            galactic::ObserverPlanet row; row.planet = planet.id;
            row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
            observer.planets.push_back(row);
        }
        for (const auto& fleet : initial.fleets) for (const auto& member : fleet.members)
            if (std::none_of(observer.types.begin(), observer.types.end(), [&](const auto& row) { return row.type == member.type; }))
                observer.types.push_back({member.type, false, false, false});
        std::sort(observer.types.begin(), observer.types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
        const auto initialized = galactic::initialize_campaign_observer(initial, initial.planets.front().id);
        if (!initialized) throw std::runtime_error(eawr::core::format_diagnostic(initialized.error()));
        auto live = take(platform::LiveGameSession::start(initial, options(workers, directory)));
        if (paused) live->submit(platform::LiveGameOrder{{1}, galactic::PauseGalaxy{}, {}});
        live->advance_to(1); expect(live->wait_for(1, 5s), "initial discovery publication settles before save");
        const auto before = live->frame().latest;
        const auto discovered = [](const auto& snapshot) {
            std::vector<galactic::PlanetId> result;
            for (const auto& row : snapshot.observations()) if (row.player == 1 && row.discovered) result.push_back(row.planet);
            return result;
        };
        const auto expected = discovered(*before);
        expect(expected.size() > 1 && expected.size() < initial.planets.size(),
            "save fixture includes newly discovered own planets and hidden foreign controls");
        const auto name = paused ? "discovery-paused" : "discovery-live";
        (void)save(*live, name);
        const auto saved = live->frame().latest;
        expect(discovered(*saved) == expected, "paused and unpaused disk capture preserve the settled discovery set");
        for (const bool player_load : {false, true}) {
            auto prepared = take(live->prepare_load(name, player_load));
            auto restored = take(platform::LiveGameSession::start(std::move(prepared), options(workers, directory)));
            const auto first = restored->frame().latest;
            expect(discovered(*first) == expected,
                "first exact/player load publication preserves discovery before any new tick");
            if (!player_load) {
                expect(std::ranges::equal(first->observations(), saved->observations()),
                    "exact restore preserves every observer fact");
                expect(first->sha256() == saved->sha256(), "exact restore retains the full state hash");
            } else {
                // WGUI-102: player load refreshes local intelligence at the active
                // mode clock. That may change owner knowledge, never discovery.
                std::size_t local_rows{};
                for (const auto& row : first->observations()) if (row.player == 1) {
                    ++local_rows;
                    const auto planet = std::find_if(initial.planets.begin(), initial.planets.end(),
                        [&](const auto& value) { return value.id == row.planet; });
                    expect(planet != initial.planets.end() && row.known_owner == planet->owner,
                        "player-load refresh publishes the fixture's actual owner even at undiscovered planets");
                }
                expect(local_rows == initial.planets.size(), "player-load ownership check covers every local observer row");
            }
            restored->stop();
        }
        live->stop();
    }
}
std::vector<std::vector<std::uint8_t>> saved_bytes(const std::string& directory) {
    std::vector<std::vector<std::uint8_t>> result;
    for (const auto* name : {"galaxy", "paused", "pending", "Quick Save", "child", "returned", "identity", "identity-again", "discovery-paused", "discovery-live"}) {
        std::ifstream file(std::filesystem::path(directory) / (std::string(name) + ".eawrsave"), std::ios::binary);
        expect(bool(file), "cross-worker save fixture exists");
        result.emplace_back(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    return result;
}
void player_load(std::size_t workers, const std::string& directory) {
    auto live = take(platform::LiveGameSession::start(setup(), options(workers, directory)));
    live->submit(platform::LiveGameOrder{{1}, galactic::PauseGalaxy{}, {}}); live->advance_to(1);
    expect(live->wait_for(1, 5s), "player pause admitted");
    // Exercise the scheduler order that formerly made the helper wait for an
    // extra paused frame after the save had already completed.
    (void)save(*live, "paused", false, true);
    auto prepared = take(live->prepare_load("paused"));
    auto restored = take(platform::LiveGameSession::start(std::move(prepared), options(workers, directory)));
    expect(restored->frame().latest->clock().pause_depth == 0 && restored->completed_tick() == live->completed_tick(),
        "player load applies explicit normalization once before the first live frame");
    auto exact = take(live->prepare_load("paused", false));
    expect(exact->completed_tick() == restored->completed_tick(), "runtime restore retains original barrier");
    // Compare to the checkpoint's explicit player prologue, never an implicit viewer resume.
    std::ifstream file(std::filesystem::path(directory) / "paused.eawrsave", std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    eawr::checkpoint::GameLoadOptions load; load.script_requirement = eawr::campaign::ScriptRequirement::none; load.player_load = true; const eawr::sim::InlineExecutor executor;
    auto reference = take(eawr::checkpoint::load_game_checkpoint(bytes, eawr::checkpoint::game_identities(setup()), load, executor));
    std::vector<std::string> hashes;
    while (reference.session.completed_tick() < 8) hashes.push_back(take(reference.session.step(executor)).state_sha256);
    restored->advance_to(8); expect(restored->wait_for(8, 5s), "normalized player load continues"); restored->stop(); live->stop();
    expect(hashes == restored->tick_hashes(), "player continuation equals reference with the same explicit prologue");
}
platform::LiveGameSession::Options child_options(std::size_t workers, const std::string& directory) {
    const auto initial = setup(); auto value = options(workers, directory);
    campaign::ConflictRecord entry; auto game = take(campaign::GameSession::create(initial));
    entry.id = game.galaxy().next_entity_id(); entry.planet = initial.planets.front().id;
    entry.invader = 1; entry.defender = 2; entry.tactical.content_identity = initial.content_identity;
    entry.tactical.first_free_entity_id = entry.id + 1; entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    for (const auto& fleet : initial.fleets) {
        const auto member = fleet.members.front(); entry.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        tactical::UnitState unit; unit.entity_id = member.id; unit.type_id = member.type; unit.owner = fleet.owner;
        entry.tactical.units.push_back(unit);
    }
    entry.seats = {{{1}, 1}, {{2}, 2}};
    tactical::TacticalContent content; tactical::DurabilityProfile hull; hull.type_id = entry.forces.front().type;
    hull.max_hull = Fixed::from_integer(100).value(); content.durability.profiles.push_back(hull);
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2};
    content.victory.humans = {1}; content.victory.countdown_frames = 2; content.victory.relevant_types = {hull.type_id};
    value.entries.push_back({1, entry, content, eawr::campaign::ScriptRequirement::none}); return value;
}
void child_roundtrip(std::size_t workers, const std::string& directory) {
    auto original_options = child_options(workers, directory); const auto entry = original_options.entries.front();
    const auto parent_setup = setup();
    auto live = take(platform::LiveGameSession::start(parent_setup, original_options));
    live->advance_to(2); expect(live->wait_for(2, 5s), "child activation barrier published");
    auto feed = live->battle_feed(entry.conflict.id, {1}); live->battle_ready(entry.conflict.id);
    const auto before_save = feed->completed_tick();
    expect(!parent_setup.seat_control, "save-only accounting proof has no role-readiness waits");
    // A recorded control-only boundary followed by the actual save creates two
    // parent ticks that must both be excluded from the exact child elapsed time.
    const auto first_barrier = live->completed_tick() + 1;
    live->submit(platform::LiveGameOrder{{1}, galactic::SaveRequest{91, false, false, false}, {}});
    // Raw replay input has no disk request to wake the paused live pacer.
    // Drive this boundary while unpaused; GameSession still freezes the child.
    feed->set_paused(false);
    live->advance_to(first_barrier);
    expect(live->wait_for(first_barrier, 5s) && feed->completed_tick() == before_save,
        "raw SaveRequest-only barrier advances parent without child world/Lua");
    feed->set_paused(true);
    expect(live->save_menu_facts().tactical && !live->save_menu_facts().pending_battle,
        "active child admission is distinct from the pending prompt");
    (void)save(*live, "child"); // The feed remains paused while the save request executes.
    expect(feed->completed_tick() == before_save, "a paused save-only control barrier does not advance the child world");
    const auto frontier = live->completed_tick(), child_tick = feed->completed_tick();
    auto prepared = take(live->prepare_load("child", false));
    auto restored = take(platform::LiveGameSession::start(std::move(prepared), options(workers, directory)));
    expect(restored->frame().battle && restored->frame().battle->completed_tick() == child_tick
        && restored->completed_tick() == frontier, "first restored child frame has saved tactical/session frontiers");
    auto restored_feed = restored->battle_feed(entry.conflict.id, {1});
    expect(restored_feed && restored_feed->completed_tick() == feed->completed_tick(), "restored feed is immediately bindable");
    for (auto* owner : {live.get(), restored.get()}) {
        auto current = owner->battle_feed(entry.conflict.id, {1}); current->set_paused(false); owner->battle_ready(entry.conflict.id);
        current->submit({1, {entry.conflict.forces.back().id}, tactical::DamagePayload{Fixed::from_integer(1000).value()}, {}});
        owner->advance_to(frontier + 7); expect(owner->wait_for(frontier + 7, 5s), "saved child completes ordered return");
        expect(!owner->frame().battle && owner->frame().result, "post-return galaxy and result publication retained");
    }
    const auto a = live->tick_hashes(), b = restored->tick_hashes();
    expect(std::vector<std::string>(a.begin() + static_cast<std::ptrdiff_t>(frontier), a.end()) == b,
        "child and return continuation matches uninterrupted live owner");
    (void)save(*live, "returned");
    auto returned = take(live->prepare_load("returned", false));
    auto resumed = take(platform::LiveGameSession::start(std::move(returned), options(workers, directory)));
    expect(!resumed->frame().battle && resumed->completed_tick() == live->completed_tick(), "post-return save restores galaxy without resurrecting child");
    expect(resumed->frame().result == live->frame().result && resumed->frame().result_conflict
        && live->frame().result_conflict && *resumed->frame().result_conflict == *live->frame().result_conflict,
        "first post-return publication retains the complete result and its roster context");
    live->stop(); restored->stop(); resumed->stop();
    feed->submit({1, {}, tactical::StopPayload{}, {}});
    expect(!live->failure(), "stopped owner's feed cannot submit a later child command");
}
void pending_roundtrip(std::size_t workers, const std::string& directory) {
    const auto fixture = child_options(workers, directory).entries.front();
    auto configured = options(workers, directory);
    auto services = std::make_shared<platform::CampaignServiceConfiguration>();
    auto content = [fixture](const auto&) { return eawr::core::Result<tactical::TacticalContent>::success(fixture.content); };
    campaign::GameSession::ConflictService admission = [](const auto&, const auto&, auto) {
        return eawr::core::Result<std::optional<campaign::RecordedAdmission>>::success({});
    };
    services->configure = [fixture, content, admission](auto& game) {
        game.set_conflict_service(admission, content, {}, eawr::campaign::ScriptRequirement::none);
        return game.queue_conflict(fixture.conflict, {campaign::ConflictRoute::pending_tactical, false, false});
    };
    services->restore.content = content; services->restore.conflict_service = admission;
    services->restore.script_requirement = eawr::campaign::ScriptRequirement::none;
    configured.services = services;
    auto live = take(platform::LiveGameSession::start(setup(), configured));
    live->advance_to(2); expect(live->wait_for(2, 5s) && live->frame().pending_conflict, "admitted pending conflict is published");
    expect(live->save_menu_facts().pending_battle && !live->save_menu_facts().tactical,
        "pending admission does not fabricate a tactical child");
    (void)save(*live, "pending");
    (void)save(*live, "Quick Save", true);
    (void)save(*live, "Quick Save", true); // Reserved quick slot overwrites without a manual confirmation.
    const auto frontier = live->completed_tick();
    auto restored = take(platform::LiveGameSession::start(take(live->prepare_load("Quick Save", false)), options(workers, directory)));
    auto player = take(platform::LiveGameSession::start(take(live->prepare_load("Quick Save", true)), options(workers, directory)));
    expect(player->frame().pending_conflict && player->frame().latest->clock().pause_depth == 1,
        "player load reestablishes the pending battle's one pause token");
    expect(restored->completed_tick() == frontier && restored->frame().pending_conflict
        && restored->frame().pending_conflict->conflict == live->frame().pending_conflict->conflict
        && restored->frame().pending_conflict->choice_ready == live->frame().pending_conflict->choice_ready,
        "first restored frame retains pending choice and its conflict pause");
    for (auto* owner : {live.get(), restored.get(), player.get()}) {
        owner->submit(platform::LiveGameOrder{{1}, galactic::BattleChoice{fixture.conflict.id, true}, {}});
        // WGS-99/142: the choice waits one tick behind the reserved prebattle autosave.
        owner->advance_to(frontier + 2); expect(owner->wait_for(frontier + 2, 5s), "restored pending choice selects its recorded child");
        expect(owner->frame().battle && !owner->frame().pending_conflict, "pending load does not recreate campaign startup");
        const auto child = owner->battle_feed(fixture.conflict.id, {1});
        expect(bool(child), "selected child exposes its local presentation feed");
        if (child) child->set_paused(false);
        owner->battle_ready(fixture.conflict.id);
        owner->advance_to(frontier + 3); expect(owner->wait_for(frontier + 3, 5s), "selected child continues once");
        expect(owner->frame().battle && owner->frame().conflict->id == fixture.conflict.id
            && !owner->frame().pending_conflict, "consumed prompt does not return on the next publication");
        owner->stop();
    }
    expect(live->tick_hashes().back() == restored->tick_hashes().back(), "pending continuation preserves canonical selection");
    expect(live->tick_hashes().back() == player->tick_hashes().back(), "pending player-load prologue preserves the same child selection");
}
// WGS-99/142/143/145/165: Begin on the pending space battle writes the reserved "Auto Save" first.
void prebattle_autosave(std::size_t workers, const std::string& directory) {
    const auto fixture = child_options(workers, directory).entries.front();
    auto content = [fixture](const auto&) { return eawr::core::Result<tactical::TacticalContent>::success(fixture.content); };
    campaign::GameSession::ConflictService admission = [](const auto&, const auto&, auto) {
        return eawr::core::Result<std::optional<campaign::RecordedAdmission>>::success({});
    };
    const auto pending_options = [&](const std::string& root) {
        auto configured = options(workers, root);
        auto services = std::make_shared<platform::CampaignServiceConfiguration>();
        services->configure = [fixture, content, admission](auto& game) {
            game.set_conflict_service(admission, content, {}, eawr::campaign::ScriptRequirement::none);
            return game.queue_conflict(fixture.conflict, {campaign::ConflictRoute::pending_tactical, false, false});
        };
        services->restore.content = content; services->restore.conflict_service = admission;
        services->restore.script_requirement = eawr::campaign::ScriptRequirement::none;
        configured.services = services; return configured;
    };
    const auto autosaves = [](platform::LiveGameSession& live) {
        std::size_t count{};
        for (const auto& row : live.save_outcomes()) if (row.name == "Auto Save") ++count;
        return count;
    };
    std::filesystem::remove_all(directory);
    auto live = take(platform::LiveGameSession::start(setup(), pending_options(directory)));
    live->advance_to(2); expect(live->wait_for(2, 5s) && live->frame().pending_conflict, "prebattle fixture waits at the prompt");
    expect(autosaves(*live) == 0, "arrival at the prompt does not invoke the producer");
    const auto frontier = live->completed_tick();
    live->submit(platform::LiveGameOrder{{1}, galactic::BattleChoice{fixture.conflict.id, true}, {}});
    live->advance_to(frontier + 1); expect(live->wait_for(frontier + 1, 5s), "the autosave tick runs");
    expect(live->frame().pending_conflict && !live->frame().battle, "the choice has not entered the child during the autosave tick");
    const auto saved_hash = live->frame().latest->sha256();
    std::uint64_t token{};
    for (const auto& row : live->save_outcomes()) if (row.name == "Auto Save") token = row.token;
    expect(token != 0 && autosaves(*live) == 1 && live->wait_save(token, 5s), "exactly one autosave is requested and reaches disk");
    const auto outcome = live->terminal_save_outcome(token);
    expect(outcome && outcome->phase == platform::LiveSavePhase::completed, "the prebattle autosave is a completed publication");
    live->advance_to(frontier + 2); expect(live->wait_for(frontier + 2, 5s), "the choice follows the autosave");
    expect(live->frame().battle && !live->frame().pending_conflict && autosaves(*live) == 1, "Begin enters the child with no second autosave");
    const auto slots = take(live->save_slots());
    const auto at = std::find_if(slots.begin(), slots.end(), [](const auto& slot) { return slot.name == "Auto Save"; });
    expect(at != slots.end() && at->description == "[AutoSave]", "the Load list shows the reserved autosave label");
    auto runtime = take(platform::LiveGameSession::start(take(live->prepare_load("Auto Save", false)), options(workers, directory)));
    expect(runtime->frame().latest->sha256() == saved_hash && runtime->frame().pending_conflict,
        "restoring the autosave reaches the hash of the pre-battle state");
    runtime->stop();
    auto restored = take(platform::LiveGameSession::start(take(live->prepare_load("Auto Save", true)), options(workers, directory)));
    expect(static_cast<bool>(restored->frame().pending_conflict), "the autosave restores at the pending prompt");
    // WGS-142: the first Begin after loading the autosave itself is not a second producer.
    const auto restored_tick = restored->completed_tick();
    restored->submit(platform::LiveGameOrder{{1}, galactic::BattleChoice{fixture.conflict.id, true}, {}});
    restored->advance_to(restored_tick + 1); expect(restored->wait_for(restored_tick + 1, 5s), "restored Begin runs");
    expect(restored->frame().battle && autosaves(*restored) == 0, "loading an autosave suppresses the next prebattle producer");
    restored->stop();
    live->stop();
    // WGS-165: the quit producer is the same reserved slot through an ordinary autosave request,
    // issued from the paused galaxy (the Escape menu's pause) and finished before the host leaves.
    auto quitter = take(platform::LiveGameSession::start(setup(), pending_options(directory + "-quit")));
    quitter->advance_to(2); expect(quitter->wait_for(2, 5s), "quit fixture runs");
    platform::LiveSaveRequest quit; quit.seat = {1}; quit.name = "Auto Save"; quit.overwrite = true; quit.autosave = true;
    const auto quit_token = quitter->request_save(std::move(quit)); quitter->advance_to(3);
    expect(quitter->wait_for(3, 5s) && quitter->wait_save(quit_token, 5s), "the quit autosave request terminates");
    const auto quit_outcome = quitter->terminal_save_outcome(quit_token);
    expect(quit_outcome && quit_outcome->phase == platform::LiveSavePhase::completed, "the quit autosave reaches disk");
    const auto quit_slots = take(quitter->save_slots());
    expect(std::any_of(quit_slots.begin(), quit_slots.end(), [](const auto& slot) { return slot.name == "Auto Save" && slot.description == "[AutoSave]"; }),
        "the quit autosave is listed with the reserved label");
    quitter->stop();
    // WGS-143: multiplayer sessions produce no autosave.
    auto coop = setup(); coop.seats[1].kind = galactic::SeatKind::human; coop.seats[1].human_side_id = 2;
    auto multi = take(platform::LiveGameSession::start(coop, pending_options(directory + "-multi")));
    multi->advance_to(2); expect(multi->wait_for(2, 5s), "multiplayer fixture runs");
    multi->submit(platform::LiveGameOrder{{1}, galactic::BattleChoice{fixture.conflict.id, true}, {}});
    multi->advance_to(3); expect(multi->wait_for(3, 5s) && autosaves(*multi) == 0, "multiplayer never writes the prebattle autosave");
    multi->stop();
    // GCMP-24/WGS-33: the saved multiplayer flag decides, so a multiplayer game down to one human still writes none.
    auto flagged = setup(); galactic_test::mark_multiplayer(flagged);
    auto lone = take(platform::LiveGameSession::start(flagged, pending_options(directory + "-flag")));
    lone->advance_to(2); expect(lone->wait_for(2, 5s), "flagged fixture runs");
    lone->submit(platform::LiveGameOrder{{1}, galactic::BattleChoice{fixture.conflict.id, true}, {}});
    lone->advance_to(3); expect(lone->wait_for(3, 5s) && autosaves(*lone) == 0, "the multiplayer flag alone suppresses the prebattle autosave");
    lone->stop();
}
// #3035: the authored identity rides with the save (unhashed section 0x8002), lists without a
// restore and survives a restore; a fresh owner of the same campaign restores the same state.
void campaign_identity(std::size_t workers, const std::string& directory) {
    const auto initial = setup();
    auto with_identity = options(workers, directory);
    platform::LiveSaveCampaignSource source;
    source.campaign = "Mod Campaign_Outer-Rim"; source.campaign_set = "Mod_Set"; source.difficulty = "Mod_Hard";
    source.custom = campaign::CampaignCustomOptions{12000, 4, 5, true};
    for (const auto& player : initial.players) source.factions.push_back({player.player_id, "Mod_Faction_" + std::to_string(player.player_id)});
    with_identity.saves.campaign = source;
    auto live = take(platform::LiveGameSession::start(initial, with_identity));
    live->advance_to(3); expect(live->wait_for(3, 5s), "identity galaxy reaches its save boundary");
    (void)save(*live, "identity");
    const platform::LiveSaveCampaign expected{source.campaign, source.campaign_set,
        "Mod_Faction_" + std::to_string(initial.seats[0].player), source.difficulty, 1, false, source.custom};
    const auto slot = take(platform::read_save_slot(directory, "identity"));
    expect(!slot.diagnostic && slot.campaign == expected && slot.completed_tick == live->completed_tick(),
        "session-free slot read returns the saving seat's authored campaign, set, faction and difficulty");
    const auto slots = take(live->save_slots());
    expect(std::any_of(slots.begin(), slots.end(), [&](const auto& row) { return row.name == "identity" && row.campaign == expected; })
        && std::any_of(slots.begin(), slots.end(), [](const auto& row) { return row.name == "galaxy" && !row.campaign && !row.diagnostic; }),
        "the list publishes identity per slot; a synthetic galaxy's save has none");
    expect(!platform::read_save_slot(directory, "absent") && !platform::read_save_slot(directory, "../identity")
        && !platform::read_save_slot("", "identity"), "slot reads refuse missing slots, paths and no directory");
    // In-campaign load vs a fresh owner of the same campaign (the restore-at-start route).
    auto running = take(live->prepare_load("identity"));
    auto fresh_owner = take(platform::LiveGameSession::start(initial, with_identity));
    auto at_start = take(fresh_owner->prepare_load("identity"));
    expect(!running->state_sha256().empty() && running->state_sha256() == at_start->state_sha256()
        && running->completed_tick() == at_start->completed_tick(),
        "restoring from a fresh campaign owner lands in the in-campaign load's state");
    fresh_owner->stop();
    auto restored = take(platform::LiveGameSession::start(std::move(at_start), options(workers, directory)));
    (void)save(*restored, "identity-again");
    expect(take(platform::read_save_slot(directory, "identity-again")).campaign == expected,
        "a restored owner keeps writing its campaign identity");
    restored->stop(); live->stop();
    // A malformed identity is a listed diagnostic, never a silently absent campaign.
    std::ifstream file(std::filesystem::path(directory) / "identity.eawrsave", std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    auto parsed = take(eawr::checkpoint::parse_checkpoint(bytes));
    expect(take(platform::save_detail::decode_campaign(parsed)) == expected, "section decodes to the listed identity");
    const auto section = std::find_if(parsed.sections.begin(), parsed.sections.end(), [](const auto& s) {
        return s.id == static_cast<std::uint16_t>(eawr::checkpoint::SectionId::campaign_identity); });
    expect(section != parsed.sections.end(), "the identity section is stored");
    expect(section->version == 2 && slot.campaign->custom == source.custom,
        "WGP-190 session-free main-menu metadata retains the original custom startup options");
    section->bytes.push_back(0);
    expect(!platform::save_detail::decode_campaign(parsed), "trailing identity bytes refuse");
    section->bytes.resize(6);
    expect(!platform::save_detail::decode_campaign(parsed), "truncated identity refuses");
    const auto damaged = take(eawr::checkpoint::write_checkpoint(parsed));
    { std::ofstream out(std::filesystem::path(directory) / "bad-identity.eawrsave", std::ios::binary);
      out.write(reinterpret_cast<const char*>(damaged.data()), static_cast<std::streamsize>(damaged.size())); }
    const auto bad = take(platform::read_save_slot(directory, "bad-identity"));
    expect(bad.diagnostic && !bad.campaign, "a malformed identity section is listed with its diagnostic");
    std::filesystem::remove(std::filesystem::path(directory) / "bad-identity.eawrsave");
}
void disk_failure(std::size_t workers, const std::string& directory) {
    const auto blocked = std::filesystem::path(directory) / "blocked-directory";
    { std::ofstream file(blocked, std::ios::binary); file << "keep"; }
    auto live = take(platform::LiveGameSession::start(setup(), options(workers, blocked.string())));
    platform::LiveSaveRequest request; request.seat = {1}; request.name = "failure";
    const auto token = live->request_save(request);
    live->advance_to(1);
    expect(live->wait_for(1, 5s) && live->wait_save(token, 5s), "failed publication still terminalizes the admitted request");
    const auto outcomes = live->save_outcomes();
    const auto found = std::find_if(outcomes.begin(), outcomes.end(), [token](const auto& row) { return row.token == token; });
    expect(found != outcomes.end() && found->phase == platform::LiveSavePhase::failed && found->diagnostic,
        "a captured checkpoint is not reported completed when its directory cannot be created");
    live->advance_to(2); expect(live->wait_for(2, 5s) && !live->failure(), "disk failure preserves the live owner");
    live->stop();
}
}
int case_galactic_live_save_tests(int argc, char** argv) {
    try {
        save_input_contract();
        retained_invasion_input();
        const auto directory = argc > 1 ? std::string(argv[1]) : "live-save-contract";
        std::vector<std::vector<std::uint8_t>> reference_bytes;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto root = (std::filesystem::path(directory) / std::to_string(workers)).string();
            galaxy_roundtrip(workers, root); player_load(workers, root); pending_roundtrip(workers, root); prebattle_autosave(workers, root + "-prebattle"); child_roundtrip(workers, root);
            disk_failure(workers, root); campaign_identity(workers, root); discovery_roundtrip(workers, root);
            const auto bytes = saved_bytes(root);
            if (reference_bytes.empty()) reference_bytes = bytes;
            else expect(reference_bytes == bytes, "all live save boundaries have identical bytes on 1/2/4/8 workers");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures ? 1 : 0;
}

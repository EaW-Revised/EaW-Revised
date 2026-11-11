#include <algorithm>
#include <array>
#include <optional>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <tuple>
#include <vector>
#include <string_view>
#include <string>
#include "../support/env.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/result.hpp"
// WGA-485: bounded stock observation, sharing the campaign gate's real providers.
#include "galactic_support.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "ai_campaign_gate.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "galactic_ai_intent_commit.hpp"
#include "galactic_ai_outbox_state.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "../../src/campaign/galactic_ai_lane_inbox.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/data/retail_values.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <variant>
using eawr::testing::must;

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using namespace galactic_test;

// W1 S2/S3 P4: what the AI's inbox holds (read from the saved async lane block): an actor loss or change, a bill the world decided
// against the billing view.
struct InboxHolds { bool actor_change{}, bill{}; };
InboxHolds inbox_holds(cp::GameSession& game) {
    InboxHolds holds;
    const auto saved = must(game.save_galactic_ai());
    if (!saved.state) return holds;
    const auto lane = saved.state->components.find("galactic-async");
    if (lane == saved.state->components.end()) return holds;
    const auto decoded = must(eawr::script::foc::galactic_ai::decode_outbox_state(lane->second, true));
    if (decoded.tail.empty()) return holds;
    cp::GalacticAiLaneInbox inbox; must(inbox.restore_prefix(decoded.tail)); // the billing view block follows (W1 S3)
    using Change = gc::GalacticSession::NativeActorChange;
    for (const auto& entry : inbox.entries()) {
        if (std::holds_alternative<cp::GalacticAiRecordedBill>(entry)) holds.bill = true;
        const auto* delivery = std::get_if<eawr::script::foc::galactic_ai::NativeActions::Delivery>(&entry);
        const auto* batch = delivery ? std::get_if<eawr::script::foc::galactic_ai::NativeActions::ActorChangesDelivery>(delivery) : nullptr;
        if (batch && std::any_of(batch->changes.begin(), batch->changes.end(),
                [](const auto& change) { return change.kind == Change::Kind::lost || change.kind == Change::Kind::changed; }))
            holds.actor_change = true;
    }
    return holds;
}
std::filesystem::path game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed campaign gate requires EAWR_EAW_GAME_ROOT");
    return std::filesystem::path(std::move(*root));
}
// One stock day on `workers` simulation workers. The P4 sweep runs inside the 4-worker day.
struct DayResult {
    std::string final_hash;
    // overlay_refused, stale, at_once, queued, drained, late, out_of_order, bills_viewed, billing_direct, billing_view_mismatch,
    // marks_outside_window
    std::array<std::uint64_t, 11> tally{};
    bool opponents{true};
};
DayResult run_day(std::size_t workers) {
    DayResult result;
    const auto root = game_root();
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto files = must(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    // Stock single-player authoring supplies Human to eager perception and Lua
    // equations (WGA-417/418/422). Keep both observed AI players and use the
    // actual Equal Footing Underworld campaign's authored human seat.
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto* source = data.campaign(campaign);
    galactic_campaign_gate::check(source && source->starting_active_player == "Underworld", "stock day campaign has its authored human seat");
    cp::CampaignSetupOptions options; options.forced_seed = 1;
    std::map<gc::PlayerId, std::string> names;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        const bool human = faction.name == source->starting_active_player;
        options.players.push_back({{id, id, 0}, faction.name, true}); names[id] = faction.name;
        options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
        if (human) options.solo_reference_human = id;
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    const auto* fog = data.constant("AIUsesFogOfWarGalactic");
    galactic_campaign_gate::check(fog, "stock AI fog policy exists");
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, {},
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers),
        eawr::data::retail::boolean(fog->text).value(), gc::galactic_max_objects, gc::galactic_max_objects, "Normal_Default",
        eawr::script::foc::galactic_ai::retail_service_periods, galactic_campaign_gate::test_schedule()));
    auto observed = std::make_shared<galactic_campaign_gate::Observations>();
    for (const auto& player : setup->lifecycle.players) observed->players.emplace(player.player, galactic_campaign_gate::PlayerProgress{});
    const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
    auto root_owner = must(cp::CampaignRoot::create(plan, services));
    const auto space = must(cp::prepare_stock_space_entry(files, loaded.catalog, data, types, plan));
    const auto usability = must(cp::prepare_stock_galactic_ai_usability(data, plan, setup->perception));
    cp::GalacticAiStockQueryFactory queries = [usability] {
        auto provider = std::make_shared<cp::GalacticAiUsabilityProvider>(usability);
        cp::GalacticAiStockQueries out;
        out.usability = [provider](auto player, const auto& snapshot, auto planet) { return provider->usability(player, snapshot, planet); };
        out.connectivity_publication = [provider](const auto& parent, auto& world, auto& engine, const auto& workers) { return provider->connectivity_publication(parent, world, engine, workers); };
        out.movement = [provider](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto& workers) { return provider->movement(player, snapshot, world, engine, workers); };
        out.external.emplace("GALACTIC_USABILITY", std::move(provider));
        return eawr::core::Result<cp::GalacticAiStockQueries>::success(std::move(out));
    };
    auto factory = must(cp::stock_galactic_ai_providers(setup, services.stock_starting_forces, space, std::move(queries),
        [observed](auto view) { observed->runtimes.push_back(std::move(view)); },
        [observed](const auto& notice) { observed->battle(notice); }));
    eawr::platform::CampaignServiceConfiguration configuration;
    configuration.configure = [&](auto& game) { return cp::configure_live_campaign_conflicts(game, files, loaded.catalog, data, types, plan, space); };
    configuration.restore.configure_session = configuration.configure;
    configuration.restore.content = [space](const auto& conflict) { return space->content(conflict); };
    must(cp::configure_galactic_ai_runtime(configuration, setup, std::move(factory)));
    must(configuration.configure(*root_owner.game()));
    must(configuration.configure_native_ai(*root_owner.game()));

    struct Counts { std::uint64_t started{}, released{}, expired{}, hostile{}, first{}; };
    std::map<gc::PlayerId, Counts> counts;
    std::set<std::tuple<std::uint64_t, gc::PlayerId, std::string, std::string>> logged;
    std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> moves;
    std::map<gc::FleetId, gc::PlayerId> fleet_owners;
    std::map<eawr::sim::EntityId, std::uint64_t> missing_actors; // actor the AI holds that the world never got -> first tick seen
    std::map<eawr::sim::EntityId, std::uint64_t> unmapped_builds; // paid queue entry of an AI builder with no purchase row in the AI state -> first tick seen
    for (const auto& fleet : root_owner.game()->galaxy().snapshot()->fleets()) fleet_owners[fleet.id] = fleet.owner;
    // W1 S2 P4 (async only): from the first tick whose inbox holds an actor loss or change, save the game at each of the next
    // 15 ticks (one whole window, so the sweep crosses the drain), then load each save and continue it past the next drain:
    // every tick must equal the uninterrupted run. Every save is loaded on 1, 2, 4 and 8 workers.
    // W1 S3: a second sweep starts at the first tick whose inbox holds a bill the world decided against the billing view and the AI
    // has not applied yet (such a save restores against a queue that is already billed). The two sweeps run independently.
    namespace ck = galactic_campaign_gate::ck;
    const auto identities = ck::game_identities(root_owner.game()->galaxy().setup());
    const bool async = setup->lifecycle.schedule.apply_delay != 0;
    constexpr std::uint64_t p4_span = 15, p4_follow = 16;
    struct Sweep {
        const char* trigger{};
        std::uint64_t first{}, bill_saves{};
        bool done{};
        std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> saves;
        std::map<std::uint64_t, std::string> hashes;
    };
    std::array<Sweep, 2> sweeps{};
    std::uint64_t published_differs{}, observed_ticks{}; // W1 S4: ticks whose per-tick native digest is not the live composition
    sweeps[0].trigger = "actor_change"; sweeps[1].trigger = "bill";
    for (std::uint64_t tick = 1; tick <= 6000; ++tick) {
        const auto state = must(root_owner.game()->step(*setup->executor));
        result.final_hash = state.state_hash.get();
        const bool sweeping = async && workers == 4 && tick >= 300;
        const auto holds = sweeping && (!sweeps[0].first || !sweeps[1].first || tick < std::max(sweeps[0].first, sweeps[1].first) + p4_span)
            ? inbox_holds(*root_owner.game()) : InboxHolds{};
        if (sweeping && !sweeps[0].first && holds.actor_change) sweeps[0].first = tick;
        if (sweeping && !sweeps[1].first && holds.bill) sweeps[1].first = tick;
        for (auto& sweep : sweeps) {
            if (!sweep.first || sweep.done) continue;
            if (tick < sweep.first + p4_span) {
                if (holds.bill) ++sweep.bill_saves;
                sweep.saves.emplace_back(tick, must(ck::save_game_checkpoint(*root_owner.game(), identities)));
            }
            if (tick > sweep.first && tick <= sweep.first + p4_span + p4_follow) sweep.hashes.emplace(tick, state.state_hash.get());
            if (tick != sweep.first + p4_span + p4_follow) continue;
            // Restored sessions report their own battles to the shared observer: keep the day's counts to the control run.
            const auto players_before = observed->players;
            std::size_t compared{};
            for (const auto& [saved_tick, bytes] : sweep.saves) {
                for (const std::size_t count : {1, 2, 4, 8}) {
                    const eawr::platform::ThreadWorkerAdapter executor(count);
                    auto restored = must(ck::load_game_checkpoint(bytes, identities, configuration.restore, executor));
                    galactic_campaign_gate::check(must(ck::save_game_checkpoint(restored.session, identities)) == bytes,
                        "P4: a save taken while the inbox is non-empty loads and saves the same bytes");
                    for (auto next = saved_tick + 1; next <= sweep.first + p4_span + p4_follow; ++next) {
                        const auto resumed = must(restored.session.step(executor));
                        galactic_campaign_gate::check(resumed.completed_tick == next && resumed.state_hash.get() == sweep.hashes.at(next),
                            "P4: a session saved and loaded inside an async window continues exactly as the uninterrupted run");
                        ++compared;
                    }
                }
            }
            observed->players = players_before;
            std::cout << "GCAI-ASYNC-P4 trigger=" << sweep.trigger << " first_tick=" << sweep.first << " saves=" << sweep.saves.size()
                << " bill_saves=" << sweep.bill_saves << " compared_ticks=" << compared << " equal=true" << std::endl;
            sweep.saves.clear(); sweep.done = true;
        }
        const auto engine = observed->runtimes.front().engine.lock();
        if (!engine) throw std::runtime_error("day-run native engine owner expired");
        for (const auto& row : engine->records()) {
            if (row.event != "assembly_sync_started" && row.event != "assembly_sync_released" && row.event != "assembly_sync_expired") continue;
            if (!logged.emplace(row.tick, row.player, row.event, row.detail).second) continue;
            auto& count = counts[row.player];
            if (row.event == "assembly_sync_started") ++count.started;
            if (row.event == "assembly_sync_released") ++count.released;
            if (row.event == "assembly_sync_expired") ++count.expired;
        }
        for (const auto& fleet : state.galaxy->fleets()) fleet_owners[fleet.id] = fleet.owner;
        for (const auto& event : state.galaxy->events()) {
            if (event.kind != gc::GalacticEventKind::movement_begin || !moves.emplace(event.tick, event.subject, event.value).second) continue;
            const auto planet = std::find_if(state.galaxy->planets().begin(), state.galaxy->planets().end(),
                [&](const auto& row) { return row.id == event.value; });
            const auto player = fleet_owners.find(event.subject);
            if (player == fleet_owners.end() || planet == state.galaxy->planets().end()) throw std::runtime_error("day-run movement attribution missing");
            if (planet->owner != player->second) {
                auto& count = counts[player->second]; ++count.hostile;
                if (!count.first) count.first = event.tick;
            }
        }
        if (tick % 300 == 0) {
            // Every actor the AI freestore holds must be in the galaxy once the lane has applied it; the lane's own pending identities (at or
            // above the lane floor) are not applied yet. A leak here later fails a garrison read and retires the AI player for good.
            const auto actions = observed->runtimes.front().actions.lock();
            const auto store = actions ? actions->freestore_owner().lock() : nullptr;
            if (!store) throw std::runtime_error("day-run freestore owner expired");
            const auto lane_begin = actions->lane_floor();
            const auto rows = state.galaxy->actor_records();
            if (state.galaxy->production()) for (const auto& planet : state.galaxy->production()->planets)
                for (const auto& queue : planet.queues) for (const auto& entry : queue) {
                    const auto& owners = state.galaxy->production()->players;
                    const auto builder = std::find_if(owners.begin(), owners.end(), [&](const auto& row) { return row.player == entry.builder; });
                    if (builder == owners.end() || builder->human) continue;
                    const auto bought = actions->purchases().find(entry.build_id);
                    if (bought == actions->purchases().end() || bought->second.completed || bought->second.failed) {
                        if (unmapped_builds.emplace(entry.build_id, tick).second)
                            std::cout << "UNMAPPED_BUILD id=" << entry.build_id << " builder=" << entry.builder << " planet=" << planet.planet << " type=" << entry.type << " tick=" << tick << (bought == actions->purchases().end() ? " no_row" : " row_closed") << std::endl;
                    }
                }
            for (const auto& [id, facts] : store->inventory()) {
                if ((lane_begin && id >= lane_begin) || facts.dead || facts.deleting) continue;
                const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
                if (at == rows.end() || at->id != id) missing_actors.emplace(id, tick);
            }
        }
        // W1 S4 visibility: on the async schedule the per-tick native digest composes the window's published AI digests (visible
        // from its apply tick), so it differs from the live composition while the AI has changed since; on the reference it never does.
        if (workers == 4) {
            const auto composed = observed->runtimes.front().state.lock();
            if (!composed) throw std::runtime_error("day-run composed native state expired");
            const auto per_tick = must(composed->sync_hash());
            const auto live = eawr::script::foc::galactic_ai::compose_native_sync_digests(must(composed->sync_digests()));
            if (per_tick != live) ++published_differs;
            ++observed_ticks;
        }
        if (tick % 600 == 0) std::cout << "synchronized-day progress tick=" << state.completed_tick << std::endl;
    }
    galactic_campaign_gate::check(root_owner.game()->completed_tick() == 6000, "bounded stock observation reaches its exact tick limit");
    if (workers == 4) {
        std::cout << "GCAI-ASYNC-PUBLISHED workers=4 ticks_reading_publication=" << published_differs << std::endl;
        // Review #6425: the AI-owned state changes on every serviced tick of the stock day, so on the async day every observed tick
        // (6000) reads the publication.
        galactic_campaign_gate::check(observed_ticks == 6000 && (async ? published_differs == observed_ticks : published_differs == 0),
            "W1 S4: the async per-tick native digest reads the window's publication; the reference reads the live state");
    }
    galactic_campaign_gate::check(!async || workers != 4 || sweeps[0].done, "P4: the async day reaches a window whose inbox holds an actor loss or change");
    // Runtime wiring of the identity blocks (W1 S0): on an async schedule every window reserved its entity and invasion blocks in
    // the world when it began, so the world's counters stand at or above the lane frontier (the end of the newest block).
    {
        const auto saved = must(root_owner.game()->save_galactic_ai());
        const auto lane = saved.state ? saved.state->components.find("galactic-async") : decltype(saved.state->components.end()){};
        galactic_campaign_gate::check(saved.state && lane != saved.state->components.end(), "the async lane state is saved");
        const auto decoded = must(eawr::script::foc::galactic_ai::decode_outbox_state(lane->second, true));
        if (decoded.schedule.apply_delay != 0) {
            const auto& galaxy = root_owner.game()->galaxy();
            std::cout << "GCAI-ASYNC-BLOCKS world_next_id=" << galaxy.next_entity_id() << " lane_frontier_id=" << decoded.frontier.id
                << " world_next_invasion=" << galaxy.next_native_invasion_id() << " lane_frontier_invasion=" << decoded.frontier.invasion << std::endl;
            galactic_campaign_gate::check(galaxy.next_entity_id() >= decoded.frontier.id
                && galaxy.next_native_invasion_id() >= decoded.frontier.invasion,
                "every async window reserved its identity blocks in the world when it began");
        }
    }
    // Apply-time refusals of what the async lane decided (the W1 parity hard condition: all zero). Printed before the checks below.
    if (const auto& tally = observed->runtimes.front().apply_tally) {
        std::cout << "GCAI-ASYNC-TALLY overlay_refused=" << tally->overlay_refused << " stale=" << tally->stale;
        for (std::size_t kind = 0; kind < tally->refused.size(); ++kind)
            std::cout << " refused_" << eawr::script::foc::galactic_ai::intent_kind_name(static_cast<eawr::script::foc::galactic_ai::IntentKind>(kind))
                << '=' << tally->refused[kind];
        std::cout << std::endl;
        result.tally = {tally->overlay_refused, tally->stale, tally->delivered_at_once, tally->queued, tally->drained, tally->late,
            tally->out_of_order, tally->bills_viewed, tally->billing_direct, tally->billing_view_mismatch, tally->marks_outside_window};
        const auto windows = std::max<std::uint64_t>(1, 6000 / (async ? setup->lifecycle.schedule.batch_ticks : 1));
        std::cout << "GCAI-ASYNC-BILLING workers=" << workers << " bills_viewed=" << tally->bills_viewed << " direct=" << tally->billing_direct
            << " view_mismatch=" << tally->billing_view_mismatch << " marks_outside_window=" << tally->marks_outside_window
            << " world_side_ns_per_window=" << tally->billing_nanoseconds / windows << std::endl;
        std::cout << "GCAI-ASYNC-DELIVERY workers=" << workers << " at_once=" << tally->delivered_at_once << " queued=" << tally->queued << " drained=" << tally->drained
            << " late=" << tally->late << " out_of_order=" << tally->out_of_order << std::endl;
        // W1 S2: on the async schedule a world delivery outside the AI window waits for the next window start and leaves the inbox
        // in arrival order; on the synchronous reference every delivery applies at once.
        if (async) galactic_campaign_gate::check(tally->queued && tally->drained && !tally->late && !tally->out_of_order,
            "async deliveries are queued, applied at the next window start and in arrival order");
        else galactic_campaign_gate::check(!tally->queued && !tally->drained && tally->delivered_at_once,
            "reference deliveries apply at once");
        // W1 S3: async bills are decided against the billing view (never by the AI outside its window), and right after every drain
        // the AI holds the newest publication with the logged credits and bills replayed. The reference bills the AI at once.
        if (async) galactic_campaign_gate::check(!tally->billing_direct && !tally->billing_view_mismatch,
            "async bills use the billing view, which matches the AI after every drain");
        // No funding notice outside the window writes the AI: marks come only from the AI's own admission inside it.
        galactic_campaign_gate::check(!tally->marks_outside_window, "no production mark is noticed outside the AI window");
        // W1 floors A: no apply-time refusal kind beyond the stored base (main after #6336, see floor_base below).
        if (async) {
            bool refusals_within = tally->overlay_refused <= 5 && tally->stale == 0;
            for (std::size_t kind = 0; kind < tally->refused.size(); ++kind) {
                const std::string name(eawr::script::foc::galactic_ai::intent_kind_name(static_cast<eawr::script::foc::galactic_ai::IntentKind>(kind)));
                const std::uint64_t allowed = name == "fleet" || name == "invasion" ? 1 : name == "orbit-recheck" ? 5 : 0;
                refusals_within = refusals_within && tally->refused[kind] <= allowed;
            }
            galactic_campaign_gate::check(refusals_within, "W1 floors A: no apply-time refusals beyond the stored base");
        }
        else galactic_campaign_gate::check(!tally->bills_viewed, "reference bills are decided by the AI at once");
        if (async && workers == 4) galactic_campaign_gate::check(!tally->bills_viewed || (sweeps[1].done && sweeps[1].bill_saves),
            "P4 covers saves holding a bill the AI has not applied yet");
    }
    for (const auto& [id, tick] : missing_actors) std::cout << "MISSING_ACTOR id=" << id << " first_seen_tick=" << tick << std::endl;
    galactic_campaign_gate::check(unmapped_builds.empty(), "every paid queue entry of an AI builder has its purchase row (checked every 300 ticks)");
    galactic_campaign_gate::check(missing_actors.empty(), "every actor in an AI freestore exists in the galaxy (checked every 300 ticks)");
    std::map<std::string, std::array<std::uint64_t, 3>> observed_floors; // hostile moves, first hostile tick, conflicts
    for (const auto& [player, progress] : observed->players) {
        const auto& count = counts[player];
        galactic_campaign_gate::check(count.expired <= count.released && (count.started || (!count.released && !count.expired)),
            "a countdown expiry always has a release and every release has a started group");
        std::cout << "SYNC_DAY player=" << names[player] << " seed=1 workers=" << workers << " ticks=6000 started=" << count.started
            << " released=" << count.released << " expired=" << count.expired << " hostile_moves=" << count.hostile
            << " first_hostile_tick=" << count.first << " conflicts=" << progress.attacks << std::endl;
        if (names[player] == "Empire" || names[player] == "Rebel")
            result.opponents = result.opponents && count.hostile && count.first && progress.attacks;
        if (async) observed_floors.emplace(names[player], std::array<std::uint64_t, 3>{count.hostile, count.first, progress.attacks});
    }
    // W1 floors A (owner, 11-07) against the stored base: main after #6336 (head 050b1ef37), async, seed 1, 6000 ticks, the same on
    // 1/2/4/8 workers. A step passes when hostile moves and conflicts stay within 2 and the first hostile tick within 300 ticks.
    // A reviewed upstream change that moves the base updates these numbers.
    if (async) {
        struct FloorBase { const char* name; std::uint64_t hostile, first, conflicts; };
        constexpr std::array<FloorBase, 3> floor_base{{{"Rebel", 11, 871, 4}, {"Empire", 3, 1801, 5}, {"Pirates", 0, 0, 0}}};
        const auto within = [](std::uint64_t value, std::uint64_t base, std::uint64_t floor_value) {
            return (value > base ? value - base : base - value) <= floor_value;
        };
        bool floors = true;
        for (const auto& base : floor_base) {
            const auto at = observed_floors.find(base.name);
            const bool held = at != observed_floors.end() && within(at->second[0], base.hostile, 2) && within(at->second[2], base.conflicts, 2)
                && (base.first ? within(at->second[1], base.first, 300) : at->second[1] == 0);
            std::cout << "GCAI-ASYNC-FLOORS workers=" << workers << " player=" << base.name << " pass=" << (held ? "true" : "false") << std::endl;
            floors = floors && held;
        }
        galactic_campaign_gate::check(floors, "W1 floors A: the async stock day stays within the floors of the stored base");
    }
    return result;
}
int run() {
    std::cout << "CTEST_FULL_OUTPUT\n";
    // The day on 4 workers (with the P4 sweep), then on 1, 2 and 8: the world and the delivery tally must not depend on it.
    std::optional<DayResult> first;
    bool opponents = true;
    for (const std::size_t workers : {4, 1, 2, 8}) {
        const auto day = run_day(workers);
        if (!first) first = day;
        else galactic_campaign_gate::check(day.final_hash == first->final_hash && day.tally == first->tally,
            "the stock day and its delivery tally are equal on 1/2/4/8 workers (workers " + std::to_string(workers) + ")");
        opponents = opponents && day.opponents;
    }
    std::cout << "GCAI-ASYNC-WORKERS 4,1,2,8 final_hash_equal=true tally_equal=true" << std::endl;
    galactic_campaign_gate::check(opponents, "both stock opponents produce attributed hostile movement and conflicts by tick6000");
    return galactic_test::failures == 0 ? 0 : 1;
}
}
int case_galactic_ai_synchronized_day_tests() { try { return run(); } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; } }

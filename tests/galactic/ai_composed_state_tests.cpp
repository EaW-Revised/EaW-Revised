// Native ownership/restore infrastructure coverage, not a live opponent proof.
#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/sync_digest.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "galactic_ai.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_plans.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace eawr::script::foc::ai {
// Larger synthetic continuation state, using the selected stock definitions.
// This stages codec workload only, never claims live plan/task admission.
struct EngineContractAccess {
    static std::size_t enlarge(Engine& e, tactical::PlayerId player) {
        auto& p = *e.player_ai(player);
        const auto prototype = p.proposed.front();
        const auto definition = prototype.potential->plan;
        // Supply a synthetic taskforce definition if this authored plan has none.
        if (e.plans_[definition].taskforces.empty()) e.plans_[definition].taskforces.emplace_back();
        for (unsigned i = 0; i < 32; ++i) {
            auto g = prototype; g.id = p.next_goal_id++;
            g.potential->id = e.next_plan_id_++;
            Plan plan; plan.id = e.next_plan_id_++; plan.instance = e.next_instance_++;
            plan.definition = definition; plan.goal = g.id; plan.player = player; plan.target = g.target;
            TaskForce tf; tf.id = e.next_taskforce_id_++; tf.plan = plan.id;
            plan.taskforces = {tf.id}; g.plan = plan.id; g.active = true;
            BuildTask task; task.taskforce = tf.id; task.source = 2;
            p.tasks.push_back(task); p.active.push_back(g);
            e.taskforces_.emplace(tf.id, tf); e.plan_of_instance_.emplace(plan.instance, plan.id);
            e.running_.emplace(plan.id, plan);
        }
        e.mark_native_state_changed(); return p.tasks.size();
    }
};
}

namespace {
using namespace galactic_test;
namespace cp = eawr::campaign;
namespace ga = eawr::script::foc::galactic_ai;
namespace ai = eawr::script::foc::ai;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
namespace core = eawr::core;
using eawr::sim::math::Fixed;
template<class T> T must(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
Fixed whole(std::int64_t value) { return must(Fixed::from_integer(value)); }
core::Result<void> refused(std::string message) {
    core::Diagnostic error; error.code = "test.native-refusal"; error.message = std::move(message);
    return core::Result<void>::failure(std::move(error));
}
constexpr std::array<std::string_view, 5> names{
    "galactic-ai-lifecycle", "GALACTIC_PERCEPTION", "galactic-goals", "galactic-execution", "galactic-production"};
struct Slot final : ga::TrackedStateContributor {
    std::string name;
    std::vector<std::uint8_t> bytes{1};
    std::vector<std::string>* order{};
    bool throws{}, fail_rollback{};
    bool refuse_capture{};
    bool tracked{true};
    std::optional<std::uint64_t> state_revision() const noexcept override {
        return tracked ? TrackedStateContributor::state_revision() : std::nullopt;
    }
    core::Result<ga::NativeState> capture() const override {
        if (refuse_capture) return core::Result<ga::NativeState>::failure(refused("capture refused").error());
        ga::NativeState state; state.components[name] = bytes;
        return core::Result<ga::NativeState>::success(std::move(state));
    }
    core::Result<void> restore(const ga::NativeState& state) override {
        mark_state_changed(); order->push_back(name); bytes = state.components.at(name);
        if (bytes.front() == 255) {
            if (throws) throw std::runtime_error("injected partial write");
            return refused("injected partial write");
        }
        if (fail_rollback) return refused("injected rollback refusal");
        return core::Result<void>::success();
    }
    core::Result<std::string> state_hash() const override {
        throw std::runtime_error("compositor must capture, not hash each contributor");
    }
};
struct Slots {
    std::vector<std::string> order;
    std::array<std::shared_ptr<Slot>, 5> owners;
    std::shared_ptr<ai::GalacticFunding> funding = std::make_shared<ai::GalacticFunding>();
    Slots() {
        for (std::size_t i = 0; i < owners.size(); ++i) {
            owners[i] = std::make_shared<Slot>(); owners[i]->name = names[i]; owners[i]->order = &order;
        }
        must(funding->initialize(1, std::array{ai::EvaluatedFundingTemplate{1, {{1, whole(1)}}, {}}}, whole(100)));
    }
    cp::GalacticAiContributors contributors() const { return {owners[0], owners[1], owners[2], funding, owners[3], owners[4], {}, {}, {}, {}}; }
};
void sync_contracts() {
    expect(core::sync_fingerprint({}) == 0xef46db3751d8e999ULL, "XXH64 empty published algorithm vector");
    const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
    expect(core::sync_fingerprint(abc) == 0x44bc2cf5ad770999ULL, "XXH64 short algorithm vector");
    Slots slots;
    auto state = must(cp::ComposedGalacticAiState::create(slots.contributors()));
    const auto saved = must(state->capture());
    const auto original = must(state->sync_hash());
    expect(original == must(ga::native_sync_state_hash(saved)), "incremental sync includes every actual native component");
    const auto counts = state->sync_encodes();
    for (unsigned i = 0; i < 10; ++i) expect(must(state->sync_hash()) == original, "unchanged sync state is stable");
    expect(counts == state->sync_encodes(), "unchanged sync state serializes no native owners");
    for (const auto& owner : slots.owners) {
        owner->bytes[0] = 2; owner->mark_state_changed();
        expect(must(state->sync_hash()) != original, "sync detects actual native owner mutation");
        expect(must(state->sync_hash()) == must(ga::native_sync_state_hash(must(state->capture()))), "sync equals independent full capture after mutation");
        owner->bytes[0] = 1; owner->mark_state_changed();
        expect(must(state->sync_hash()) == original, "restored bytes give same sync digest with a different local epoch");
    }
    must(state->restore(saved));
    expect(must(state->sync_hash()) == original, "native save/restore retains sync state exactly");
    expect(must(state->state_hash()) == must(ga::native_state_hash(saved)), "full checkpoint digest is still canonical SHA");
}
void contracts() {
    Slots slots;
    auto state = must(cp::ComposedGalacticAiState::create(slots.contributors()));
    const auto saved = must(state->capture());
    expect(saved.components.size() == 6, "all native components captured once");
    const auto original = must(state->state_hash());
    expect(original == must(ga::native_state_hash(saved)), "composed digest equals independent canonical hash");
    for (unsigned i = 0; i < 10; ++i) expect(must(state->state_hash()) == original, "unchanged hash reused");
    expect(state->digest_generations() == 1, "unchanged requests perform one SHA256 generation");
    expect(state->owner_encodes() == std::array<std::uint64_t, 6>{1, 1, 1, 1, 1, 1},
        "unchanged requests encode no owners despite full verification");
    for (const auto& owner : slots.owners) {
        const auto before = state->owner_encodes();
        owner->mark_state_changed(); owner->bytes[0] = 2;
        expect(must(state->state_hash()) != original, "each native mutation invalidates digest");
        const auto after = state->owner_encodes();
        const auto index = static_cast<std::size_t>(std::find(slots.owners.begin(), slots.owners.end(), owner) - slots.owners.begin());
        const auto cache_index = index < 3 ? index : index + 1;
        for (std::size_t i = 0; i < before.size(); ++i) expect(after[i] == before[i] + (i == cache_index ? 1 : 0), "only changed owner encodes");
        owner->mark_state_changed(); owner->bytes[0] = 1;
        expect(must(state->state_hash()) == original, "reverted bytes restore original digest");
    }
    const auto generations = state->digest_generations();
    must(state->restore(saved));
    expect(must(state->state_hash()) == original && state->digest_generations() == generations,
        "identical restore does not rehash unchanged canonical state");
    expect(slots.order == std::vector<std::string>(names.begin(), names.end()), "native restore follows explicit dependency order");
    const auto funding_before = slots.funding->state();
    must(slots.funding->notify({galactic::GalacticSession::CreditNotification::Kind::windfall, 1, whole(101), whole(1), {}}));
    expect(must(state->state_hash()) != original, "real ledger mutation invalidates digest");
    must(state->restore(saved));
    expect(slots.funding->state() == funding_before && must(state->state_hash()) == original, "funding restored through owned codec");
    for (const auto& owner : slots.owners) {
        auto damaged = saved; damaged.components[owner->name][0] = 255;
        // Also change earlier dependencies and the real ledger before refusal.
        damaged.components[std::string(names[0])][0] = owner == slots.owners[0] ? 255 : 2;
        auto changed = funding_before; changed.at(1).categories.at(1).cash = whole(123);
        damaged.components["funding"] = must(ga::encode_funding_state(changed));
        expect(!state->restore(damaged), "rejecting partial native write refused");
        expect(must(ga::encode_native_state(must(state->capture()))) == must(ga::encode_native_state(saved)),
            "all dependencies roll back after refusal");
    }
    slots.owners.back()->throws = true;
    auto damaged = saved; damaged.components[std::string(names.back())][0] = 255;
    expect(!state->restore(damaged) && must(state->state_hash()) == original, "throwing consumer is rolled back");
    slots.owners.front()->tracked = false;
    slots.owners.front()->bytes[0] = 9;
    expect(must(state->state_hash()) != original, "untracked provider safely recomputes without a bump");
    slots.owners.front()->tracked = true;
    must(state->restore(saved));
    slots.owners.front()->mark_state_changed(); slots.owners.front()->bytes[0] = 3;
    slots.owners.back()->mark_state_changed(); slots.owners.back()->refuse_capture = true;
    expect(!state->state_hash(), "late capture refusal after early cache update fails closed");
    slots.owners.back()->refuse_capture = false;
    expect(must(state->state_hash()) == must(ga::native_state_hash(must(state->capture()))), "retry composes already updated earlier cache");
    must(state->restore(saved));
    damaged = saved; damaged.components.erase("funding");
    expect(!state->restore(damaged), "missing component rejected before publication");
    damaged = saved; damaged.components["unknown"] = {1};
    expect(!state->restore(damaged), "unknown component rejected before publication");
    damaged = saved; damaged.version = 2;
    expect(!state->restore(damaged), "unsupported native envelope refused");
    auto incomplete = slots.contributors(); incomplete.execution.reset();
    expect(!cp::ComposedGalacticAiState::create(incomplete), "missing provider cannot remove refusal");
    slots.owners.back()->fail_rollback = true;
    damaged = saved; damaged.components[std::string(names.back())][0] = 255;
    expect(!state->restore(damaged) && !state->capture() && !state->state_hash(), "rollback failure poisons native owner");
}

// This owner cannot service gameplay. Its only purpose here is to retain A's
// actual selected lifecycle codec at a prepared campaign boundary.
struct Unserviced final : ga::Services {
    core::Result<void> initialize(galactic::PlayerId) override { return refused("runtime not composed"); }
    core::Result<void> clear_equations(galactic::PlayerId) override { return refused("runtime not composed"); }
    core::Result<void> apply_funding_writes(galactic::PlayerId, const ga::TemplatePolicy&) override { return refused("runtime not composed"); }
    core::Result<void> apply_policy(galactic::PlayerId, const ga::TemplatePolicy&, ga::PolicyWrite) override { return refused("runtime not composed"); }
    core::Result<void> run(const ga::ServiceInput&, auth::ScriptScheduler&) override { return refused("runtime not composed"); }
};
struct RealOwners {
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    std::shared_ptr<ai::Engine> engine;
    std::shared_ptr<ai::GalacticFunding> funding = std::make_shared<ai::GalacticFunding>();
    std::shared_ptr<ga::ExecutionStateContributor> execution;
    std::shared_ptr<ga::NativeActions> production;
    std::shared_ptr<ga::Manager> lifecycle;
    std::shared_ptr<ai::GoalStateContributor> goals;
    RealOwners(galactic::GalacticSession& galaxy, ga::LifecycleSetup setup, ai::GalacticPerceptionSetup perception,
        std::vector<ai::PlanDef> plans, const eawr::sim::PartitionExecutor& executor) {
        auto data = must(ga::load_data(setup));
        for (const auto& plan : plans) setup.plans.push_back({plan.name, plan.goals});
        host->setup.seed = perception.seed;
        for (const auto& player : perception.players) {
            const auto selected = std::find_if(setup.players.begin(), setup.players.end(), [&](const auto& p) { return p.player == player.player; });
            host->setup.players.push_back({player.player, player.faction, player.neutral, selected != setup.players.end(),
                selected != setup.players.end() ? selected->player_type : "", player.human});
        }
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        world = std::make_shared<ai::GalacticWorld>(std::move(perception)); host->ai_world = world;
        engine = std::make_shared<ai::Engine>(host, data, std::move(plans)); host->engine = engine.get();
        // This prepared boundary has no configured execution runtime. Refuse
        // every retained execution row rather than accepting unresolved IDs.
        execution = std::make_shared<ga::ExecutionStateContributor>([](const ga::ExecutionRetainedState& state) {
            return state.empty() ? core::Result<void>::success() : refused("prepared boundary has no execution runtime identities");
        });
        must(world->bind(galaxy.snapshot(), {}, executor, galaxy.completed_tick(), galaxy.next_entity_id()));
        auto scripts = must(auth::ScriptScheduler::create({}, {})); std::uint64_t sequence{};
        must(engine->begin_galactic_barrier(*world, {galaxy.completed_tick(), galaxy.clock().galactic_frame}, scripts, sequence, setup.players, &executor));
        ga::Capabilities capabilities; capabilities.queries = std::make_shared<ai::GalacticQueries>(*engine, *world);
        capabilities.services = std::make_shared<Unserviced>();
        lifecycle = must(ga::Manager::create(setup, std::move(capabilities)));
        goals = std::make_shared<ai::GoalStateContributor>(engine, core::sha256_hex(std::span<const std::uint8_t>{}));
        std::map<galactic::PlayerId, Fixed> multipliers;
        if (galaxy.snapshot()->player_economy()) for (const auto& player : galaxy.snapshot()->player_economy()->players) {
            multipliers.emplace(player.player, whole(1));
            // Prepared boundary: no runtime template initialization claim.
            must(funding->initialize(player.player, {}, player.credits));
        }
        production = std::make_shared<ga::NativeActions>(galaxy, funding, ga::NativeActions::Placement{}, std::move(multipliers));
        production->bind_handlers();
    }
    cp::GalacticAiContributors contributors() const {
        return {lifecycle, world, goals, funding, execution, production, [manager = engine, bound = world](const auto& ledgers) {
            for (const auto& [player, ledger] : ledgers) {
                const auto players = bound->players();
                if (std::none_of(players.begin(), players.end(), [&](const auto& p) { return p.player_id == player; })) return refused("unknown ledger player");
                for (const auto& [id, goal] : ledger.goals) { static_cast<void>(goal); if (!manager->galactic_goal(player, id)) return refused("unresolved funding goal identity"); }
            }
            return core::Result<void>::success();
        }, {}, {}, {}};
    }
};
galactic::GalacticSetup fixture() {
    galactic::SyntheticGalaxyOptions options; options.players = 2; options.planets = 2; options.fleets_per_player = 0;
    auto setup = galactic::synthetic_galaxy(options);
    setup.player_economy = galactic::PlayerEconomySetup{{{1, whole(100), whole(1000), 0, 0}, {2, whole(100), whole(1000), 0, 0}}};
    return setup;
}
ga::LifecycleSetup fixture_lifecycle() {
    ga::LifecycleSetup setup;
    setup.xml_paths = {"data/xml/difficultyadjustments.xml"};
    setup.xml[setup.xml_paths.front()] = "<Difficulty_Adjustments/>";
    return setup;
}
ai::GalacticPerceptionSetup fixture_perception(const galactic::GalacticSetup& setup) {
    ai::GalacticPerceptionSetup perception;
    for (const auto& player : setup.players) perception.players.push_back({player.player_id, player.faction_id, "", "Normal", false, false, false, 0});
    for (const auto& planet : setup.planets) perception.planets.push_back({planet.id, "P" + std::to_string(planet.id), 0, {}, {}, {}, {}, {}});
    return perception;
}
void real_boundaries() {
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = must(cp::GameSession::create(fixture()));
        if (workers > 1) game.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        std::shared_ptr<RealOwners> owners;
        auto state = must(cp::install_composed_galactic_ai(game, [&](auto& world) {
            expect(&world == &game.galaxy(), "factory receives actual retained GameSession world");
            owners = std::make_shared<RealOwners>(world, fixture_lifecycle(), fixture_perception(world.setup()), std::vector<ai::PlanDef>{}, executor);
            return core::Result<cp::GalacticAiContributors>::success(owners->contributors());
        }));
        auto duplicate = cp::install_composed_galactic_ai(game, owners->contributors());
        expect(!duplicate, "native owner cannot be installed twice");
        const auto prepared = must(state->capture());
        ga::ExecutionStateContributor unsupported;
        must(unsupported.replace({{1, {}}}));
        auto invalid_execution = prepared;
        invalid_execution.components["galactic-execution"] = must(unsupported.capture()).components.at("galactic-execution");
        expect(!state->restore(invalid_execution), "prepared boundary refuses unconfigured execution identities");
        expect(must(ga::encode_native_state(must(state->capture()))) == must(ga::encode_native_state(prepared)),
            "prepared execution refusal rolls all real owners back");
        std::vector<std::string> hashes;
        for (unsigned i = 0; i < 4; ++i) {
            const auto tick = must(game.step(executor)); hashes.push_back(tick.state_hash.get());
            expect(hashes.back() == game.state_sha256(), "native fast path matches independent boundary composition");
        }
        const auto identities = eawr::checkpoint::game_identities(fixture());
        const auto bytes = must(eawr::checkpoint::save_game_checkpoint(game, identities));
        std::shared_ptr<RealOwners> restored_owners;
        eawr::checkpoint::GameLoadOptions options;
        options.configure_native_ai = [&](auto& resumed) {
            auto installed = cp::install_composed_galactic_ai(resumed, [&](auto& world) {
                restored_owners = std::make_shared<RealOwners>(world, fixture_lifecycle(), fixture_perception(world.setup()), std::vector<ai::PlanDef>{}, executor);
                return core::Result<cp::GalacticAiContributors>::success(restored_owners->contributors());
            });
            return installed ? core::Result<void>::success() : core::Result<void>::failure(installed.error());
        };
        auto restored = must(eawr::checkpoint::load_game_checkpoint(bytes, identities, options, executor));
        expect(restored_owners && restored_owners.get() != owners.get(), "checkpoint creates fresh owners before ordered restore");
        expect(must(eawr::checkpoint::save_game_checkpoint(restored.session, identities)) == bytes, "six real contributors canonically recapture checkpoint");
        expect(restored.session.galaxy().cached_state_hash().get() == restored.session.galaxy().state_sha256(),
            "cached world hash immediately after checkpoint restore equals independent recompute");
        for (unsigned i = 0; i < 3; ++i) expect(must(game.step(executor)).state_hash.get() == must(restored.session.step(executor)).state_hash.get(), "real contributor continuation equals uninterrupted owner");
        if (reference.empty()) reference = hashes; else expect(reference == hashes, "real native boundary hashes equal on 1/2/4/8 workers");
        expect(state->digest_generations() == 1, "frozen native owners reuse digest across galaxy frames");
        expect(state->owner_encodes() == std::array<std::uint64_t, 6>{1, 1, 1, 1, 1, 1}, "all six real frozen owners tracked");
        const auto funding_revision = owners->funding->state_revision();
        const auto before_rows = must(state->state_hash());
        auto ledger = *owners->funding->ledger(1);
        ledger = must(ai::funding::fiscal(std::move(ledger), whole(100), whole(3), whole(1)));
        expect(ledger != *owners->funding->ledger(1), "funding publication fixture changes retained interval state");
        must(owners->funding->commit_rows({{1, std::move(ledger)}}, [] { return core::Result<void>::success(); }));
        expect(owners->funding->state_revision() != funding_revision, "immediate funding publication owns its revision bump");
        expect(must(state->state_hash()) != before_rows, "published funding rows invalidate the cached digest");
        expect(must(state->state_hash()) == must(ga::native_state_hash(must(state->capture()))), "published funding rows match full oracle");
        const auto hint_revision = owners->world->state_revision();
        expect(owners->world->insert_hint({1, "P1", "test", ai::real(3)}), "retained hint inserted");
        expect(owners->world->state_revision() != hint_revision, "perception mutation bumps its own epoch");
        expect(must(state->state_hash()) == must(ga::native_state_hash(must(state->capture()))), "real perception mutation matches full oracle");
        const auto manager_revision = owners->lifecycle->state_revision();
        owners->lifecycle->set_admission({ga::Mode::space, true, false});
        expect(owners->lifecycle->state_revision() != manager_revision, "lifecycle entry owns admission bump");
        expect(must(state->state_hash()) == must(ga::native_state_hash(must(state->capture()))), "real lifecycle mutation matches full oracle");
        const auto execution_revision = owners->execution->state_revision();
        must(owners->execution->replace({}));
        expect(owners->execution->state_revision() != execution_revision, "execution replacement owns bump even for same bytes");
        const auto production_revision = owners->production->state_revision();
        must(owners->production->restore(prepared));
        expect(owners->production->state_revision() != production_revision, "production restore owns epoch bump");
        must(state->restore(prepared));
        expect(must(state->state_hash()) == must(ga::native_state_hash(prepared)), "restore returns exact hash despite differing process epochs");
    }
}
struct AfterWorldEffect final : auth::GalacticScriptEngine {
    galactic::GalacticSession* world{};
    core::Result<auth::ServiceOptions> before_service(const eawr::sim::PartitionExecutor&,
        const galactic::GalacticSession&, const galactic::GalacticTick&, auth::ScriptScheduler&) override {
        auto result = world->add_credits(1, whole(3), whole(1));
        if (!result) return core::Result<auth::ServiceOptions>::failure(result.error());
        return core::Result<auth::ServiceOptions>::success({});
    }
};
void world_hash_boundaries() {
    for (const auto count : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(count);
        auto world = must(galactic::GalacticSession::create(fixture()));
        if (count > 1) world.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        const auto before = world.cached_state_hash();
        expect(before.get() == world.state_sha256(), "initial cached world hash matches independent hash");
        const auto stepped = must(world.step(executor)).state_hash;
        expect(stepped.get() == world.state_sha256(), "ordinary cached world hash matches independent hash");
        must(world.add_credits(1, whole(3), whole(1)));
        const auto after = world.cached_state_hash();
        expect(after.get() != stepped.get() && after.get() == world.state_sha256(), "post-step native cash invalidates correct world section");
        expect(before.get() != after.get(), "pending old hash retains its captured generation");
        const auto next = must(world.step(executor)).state_hash.get();
        expect(next == world.state_sha256(), "hash read does not lose dirtiness for next world service");
        auth::SessionConfig config; config.galactic_clock = true; config.tick_duration = {1, 30};
        auto hosted = must(auth::ScriptedGalacticSession::create(must(galactic::GalacticSession::create(fixture())),
            must(auth::ScriptScheduler::create(config, {})), must(auth::ScriptScheduler::create(config, {}))));
        if (count > 1) hosted.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        auto effect = std::make_shared<AfterWorldEffect>(); effect->world = &hosted.parent_world(); must(hosted.set_ai_engine(effect));
        const auto tick = must(hosted.step(executor));
        const auto expected = auth::authoritative_state_sha256(hosted.completed_tick(), hosted.world().state_sha256(), must(hosted.script_state_hash()));
        expect(tick.state_hash.get() == expected, "script bridge includes native cash admitted after its world step");
    }
}
std::string game_root() {
#ifdef _MSC_VER
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") || !value) throw std::runtime_error("installed composition requires game data");
    std::string root(value); std::free(value); return root;
#else
    const auto* value = std::getenv("EAWR_EAW_GAME_ROOT");
    if (!value) throw std::runtime_error("installed composition requires game data");
    return value;
#endif
}
void benchmark(const std::shared_ptr<cp::ComposedGalacticAiState>& state, unsigned workers, std::string_view label) {
    const auto saved = must(state->capture());
    const auto encoded = must(ga::encode_native_state(saved));
    const auto expected = must(ga::native_state_hash(saved));
    expect(must(state->state_hash()) == expected, "benchmark starts at independently verified boundary");
    const auto counts = state->owner_encodes();
    std::vector<double> full, incremental;
    for (unsigned i = 0; i < 120; ++i) {
        auto start = std::chrono::steady_clock::now();
        expect(must(ga::encode_native_state(must(state->capture()))) == encoded, "old full capture/encode/compare unchanged");
        full.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        start = std::chrono::steady_clock::now();
        expect(must(state->state_hash()) == expected, "benchmark incremental equals full oracle");
        incremental.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(full.begin(), full.end()); std::sort(incremental.begin(), incremental.end());
    expect(state->owner_encodes() == counts, "unchanged benchmark does zero contributor encoding");
    std::cout << "digest benchmark: state=" << label << " workers=" << workers << " bytes=" << encoded.size()
        << " before_median_ms=" << full[60] << " after_median_ms=" << incremental[60] << '\n';
}
void installed(bool measure = false) {
    namespace vfs = eawr::vfs;
    std::vector<vfs::MountSpec> mounts;
    for (const auto folder : {"corruption", "GameData"}) mounts.push_back(must(vfs::resolve_manifest_mount(folder, std::filesystem::path(game_root()) / folder / "Data")).mount);
    auto files = must(vfs::Vfs::mount(mounts));
    const auto read = [&](std::string_view path) { const auto bytes = must(files.open(path)); return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()); };
    ga::LifecycleSetup lifecycle;
    for (const auto& path : ga::required_xml()) lifecycle.xml[path] = read(path);
    std::map<std::string, std::string> modules;
    for (const auto& asset : must(files.enumerate("data/scripts", ".lua"))) {
        if (asset.canonical_path.starts_with("data/scripts/library/") || (asset.canonical_path.starts_with("data/scripts/ai/") && asset.canonical_path.substr(16).find('/') == std::string::npos))
            modules.emplace(asset.canonical_path, read(asset.canonical_path));
    }
    const auto plans = ai::galactic_goals::goal_plan_registrations(must(ai::galactic_goals::load_plan_catalog({}, modules)));
    const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Empire";
    const auto* source = data.campaign(campaign_name); if (!source) throw std::runtime_error("stock campaign absent");
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<galactic::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source->starting_active_player;
        if (human) options.local_observer = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({{id}, id, human ? galactic::SeatKind::human : galactic::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
    const auto planned = must(cp::plan_live_campaign_setup(files, data, types, campaign_name, options, "Normal_Default"));
    const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, planned, "Normal_Default"));
    ai::GalacticPerceptionSetup perception; perception.seed = 42;
    galactic::PlayerId empire{};
    for (const auto& binding : planned.player_bindings()) {
        const auto* faction = data.faction(binding.faction);
        ai::GalacticPlayerFacts row{binding.player.player_id, binding.player.player_id, binding.faction, "Normal", faction->is_neutral.value_or(false), binding.faction == "Empire", false, 0};
        if (row.ai) empire = row.player;
        for (const auto& home : planned.homes()) if (home.player == row.player && home.planet) row.home = *home.planet;
        perception.players.push_back(row);
    }
    if (!empire) throw std::runtime_error("stock Empire player absent");
    lifecycle.players = {{empire, "BASICEMPIRE", "NORMAL", {}, {}}};
    for (const auto& location : planned.locations()) if (location.planet) perception.planets.push_back({location.planet->id, location.original_type, ai::crc32(ai::upper_case(location.original_type)), {}, {}, {}, {}, {}});
    for (const auto& binding : planned.type_bindings()) if (const auto* type = planned.original_type(binding.id)) {
        foc::AiType facts; facts.type_id = binding.id; facts.name = ai::upper_case(type->name); perception.types.push_back(std::move(facts));
    }
    std::string reference;
    std::vector<std::string> frame_reference;
    for (const auto count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(count);
        auto campaign = must(cp::CampaignRoot::create(planned, services)); std::shared_ptr<RealOwners> owners;
        auto state = must(cp::install_composed_galactic_ai(*campaign.game(), [&](auto& world) {
            owners = std::make_shared<RealOwners>(world, lifecycle, perception, plans, executor);
            return core::Result<cp::GalacticAiContributors>::success(owners->contributors());
        }));
        const auto ai_data = must(ga::load_data(lifecycle)); std::vector<ga::PlanInput> registered;
        for (const auto& plan : plans) registered.push_back({plan.name, plan.goals});
        const auto& player_type = ai_data.players.at("BASICEMPIRE");
        const auto policy = ga::apply_template(ai_data, registered, {}, ai_data.templates.at(player_type.galactic_templates.front()));
        must(owners->engine->apply_galactic_policy(empire, policy));
        std::vector<ai::GoalFunction> functions;
        for (const auto& name : player_type.function_sets) if (const auto at = ai_data.function_sets.find(name); at != ai_data.function_sets.end())
            for (const auto& f : at->second) if (ai_data.goals.contains(f.goal) && ai_data.goals.at(f.goal).mode == "GALACTIC") functions.push_back(f);
        for (std::size_t index = 0; index < functions.size(); ++index) {
            const auto& goal = ai_data.goals.at(functions[index].goal);
            if (goal.application.contains("GLOBAL")) must(owners->engine->allocate_galactic_goal(empire, index, 0));
            else for (const auto target : *owners->engine->target_list(empire)) {
                if (owners->engine->target_matches(empire, goal.application, owners->engine->target(target))) must(owners->engine->allocate_galactic_goal(empire, index, target));
            }
        }
        expect(!owners->engine->galactic_goals(empire).empty(), "stock boundary contains real positive authored goal allocations");
        const auto hash = must(state->state_hash());
        if (reference.empty()) reference = hash; else expect(reference == hash, "stock composed prepared state equal on 1 and 8 threads");
        const auto saved = must(state->capture());
        const auto saved_bytes = must(ga::encode_native_state(saved)).size();
        if (measure) benchmark(state, count, "stock-prepared");
        std::vector<double> times;
        std::vector<std::string> frame_hashes;
        constexpr unsigned frames = 120;
        const auto request_start = state->digest_requests();
        for (unsigned i = 0; i < frames; ++i) {
            // Real stock world progression with verification on every tick;
            // the gameplay services intentionally remain uncomposed.
            frame_hashes.push_back(must(campaign.game()->step(executor)).state_hash.get());
            const auto start = std::chrono::steady_clock::now();
            expect(must(state->state_hash()) == hash, "stock unchanged native digest reused");
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        const auto requests = state->digest_requests() - request_start;
        expect(requests == 2 * frames, "one native request per game frame plus one measurement request");
        if (frame_reference.empty()) frame_reference = frame_hashes; else expect(frame_reference == frame_hashes, "stock world progression hashes equal on 1/2/4/8 with full hash verification");
        expect(state->digest_generations() == 1, "stock unchanged requests hash once");
        must(state->restore(saved));
        expect(must(state->state_hash()) == hash, "all six real stock owners restore exact prepared state");
        std::cout << "composed stock prepared boundary: workers=" << count << " goals=" << owners->engine->galactic_goals(empire).size()
            << " bytes=" << saved_bytes << " game_requests_per_frame=1 measurement_requests_per_frame=1 median_capture_encode_compare_ms=" << times[times.size() / 2]
            << " digest_generations=" << state->digest_generations() << '\n';
        if (measure) {
            expect(ai::EngineContractAccess::enlarge(*owners->engine, empire) >= 32, "larger synthetic state has several goals and tasks");
            benchmark(state, count, "synthetic-32-goals-tasks");
        }
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") installed();
        else if (argc == 2 && std::string_view(argv[1]) == "--benchmark") installed(true);
        else { sync_contracts(); contracts(); real_boundaries(); world_hash_boundaries(); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures ? 1 : 0;
}

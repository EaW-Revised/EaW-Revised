#include "galactic_support.hpp"
#include "galactic_ai_world.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/sim/galactic/encoding.hpp"

#include <sstream>
#include <stdexcept>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
namespace ai = eawr::script::foc::ai;
namespace ga = eawr::script::foc::galactic_ai;
using galactic_test::expect;
using ai::real;
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}
void done(eawr::core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
eawr::sim::math::Fixed f(std::int64_t n) { return take(eawr::sim::math::Fixed::from_integer(n)); }
gc::GalacticSetup fixture() {
    gc::GalacticSetup s; s.seed = 71; s.debug_commands = true;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}, {{2}, 2, gc::SeatKind::human, 1, true, 2}, {{3}, 1, gc::SeatKind::script, 2, true, 0}};
    s.rules.maximum_fleet_movement_distance = f(10); s.rules.fiscal_cycle_frames = 100;
    for (gc::PlanetId id = 1; id <= 5; ++id) s.planets.push_back({id, {f(static_cast<std::int64_t>(id - 1) * 10), {}, {}}, id % 2 ? 1U : 2U});
    s.fleets = {{50, 2, 2, {{51, 100}}}};
    s.control.emplace(); auto& control = *s.control; control.change_seconds = f(7); control.max_ground_forces = 2;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    control.types = {{100, 0, false, false}};
    for (const auto& a : control.players) for (const auto& b : control.players) control.relations.push_back({a.player, b.player, a.player == b.player});
    s.observer.emplace(); auto& observer = *s.observer; observer.local_player = 2;
    observer.reveal_delay_seconds = f(1000); observer.automatic_reveal_range = f(0);
    observer.players = {{1, true, false, 0, 100, {}}, {2, false, false, 0, 100, {}}, {3, false, false, 0, 100, {}}};
    observer.types = {{100, false, false, false}};
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; control.planets.push_back(row);
        gc::ObserverPlanet facts; facts.planet = planet.id; facts.allegiance = facts.target_owner = planet.owner;
        facts.current_owner = facts.current_controller = planet.owner; observer.planets.push_back(facts);
    }
    for (const auto& player : s.players) for (const auto& planet : s.planets)
        observer.records.push_back({player.player_id, planet.id, true, 0, 0, gc::bit(gc::Intelligence::ownership), {}});
    return s;
}
ai::GalacticPerceptionSetup perception(bool fog) {
    ai::GalacticPerceptionSetup out; out.seed = 71; out.uses_fog = fog;
    out.players = {{1, 1, "EMPIRE", "NORMAL", false, true, false, 1},
        {2, 2, "REBEL", "NORMAL", false, false, true, 2}, {3, 3, "PIRATES", "NORMAL", true, false, false, 0}};
    for (gc::PlanetId id = 1; id <= 5; ++id) out.planets.push_back({id, "P" + std::to_string(id), 100 + id, {}, {}, {}, std::vector<std::pair<std::uint64_t, ai::Real>>{}, {}});
    ai::GalacticForceType force; force.type = 100; force.categories = 1; force.power = real(42); force.object_traits = ai::GalacticObjectTraits{}; out.force_types.push_back(force);
    out.maximum_fleet_distance = real(10); return out;
}
struct Host {
    std::shared_ptr<eawr::script::foc::detail::Host> host = std::make_shared<eawr::script::foc::detail::Host>();
    ai::GalacticWorld world;
    ai::Engine engine;
    explicit Host(const ai::GalacticPerceptionSetup& setup) : world(setup), engine(host, {}, {}) {}
    std::optional<ai::Real> query(std::string_view body, gc::PlayerId player = 1, gc::PlanetId planet = 2, std::optional<gc::PlayerId> enemy = {}) {
        const auto equation = take(ai::parse_equation("CONTRACT", body, {}));
        ai::Target target; target.object = planet; target.name = "OBJECT_" + std::to_string(planet);
        ai::Context context; context.player = player; context.target = &target; context.enemy = enemy;
        ai::AiRandom random; return world.perception(engine, equation, context, random);
    }
};
std::string visibility(unsigned workers, bool fog) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto session = take(gc::GalacticSession::create(fixture()));
    auto setup = perception(fog); Host host(setup);
    done(host.world.bind(session.snapshot(), {}, executor, 1, session.next_entity_id()));
    expect(host.query("Variable_Target.TargetPlayerId") == real(2), "target ownership is independently visible");
    expect(host.query("Variable_Target.EnemyForce.SpaceTotalUnnormalized") == real(42), "hidden first miss evaluates actual force");
    host.world.set_synchronized_frame(31);
    expect(host.query("Variable_Target.EnemyForce.SpaceTotal.TimeLastSeen") == (fog ? real(1) : real(0)), "force count mask owns age, separately from ownership");
    expect(host.query("Variable_Self.AverageAgeOfSpaceIntelligence") == (fog ? real(1) : real(0))
        && host.query("Variable_Self.AverageAgeOfGroundIntelligence") == (fog ? real(1) : real(0)), "average covers every actual foreign planet");
    expect(!host.query("Variable_Enemy.AverageAgeOfSpaceIntelligence", 1, 2, 2), "average rejects a different selected player");
    if (fog) {
        const auto member_visible = [&]() {
            const auto view = host.world.view(); const auto* member = view->find(51); const auto* viewer = view->player(1);
            return member && viewer && (member->visible_to & (std::uint64_t{1} << viewer->snapshot_index)) != 0;
        };
        expect(!member_visible(), "ownership-only first bind never reveals enemy member");
        done(host.world.bind(session.snapshot(), {}, executor, 31, session.next_entity_id()));
        expect(!member_visible(), "ownership cache retained across a second bind does not reveal members");
        auto contents = fixture();
        for (auto& row : contents.observer->records) if (row.player == 1 && row.planet == 2)
            row.mask |= gc::bit(gc::Intelligence::fleet_contents);
        auto gained = take(gc::GalacticSession::create(contents));
        done(host.world.bind(gained.snapshot(), {}, executor, 31, gained.next_entity_id()));
        expect(member_visible(), "qualified fleet-content gain reveals ordinary enemy member");
        auto counts = fixture();
        for (auto& row : counts.observer->records) if (row.player == 1 && row.planet == 2)
            row.mask |= gc::bit(gc::Intelligence::fleet_presence) | gc::bit(gc::Intelligence::fleet_count) | gc::bit(gc::Intelligence::ground_count);
        auto lost = take(gc::GalacticSession::create(counts));
        done(host.world.bind(lost.snapshot(), {}, executor, 31, lost.next_entity_id()));
        expect(!member_visible(), "content loss hides member despite ownership, presence and count knowledge");
        done(host.world.bind(lost.snapshot(), {}, executor, 31, lost.next_entity_id()));
        expect(!member_visible(), "repeated loss bind stays hidden");
    }
    const auto saved = take(host.world.capture()); Host restored(setup);
    done(restored.world.bind(session.snapshot(), {}, executor, 31, session.next_entity_id()));
    done(restored.world.restore(saved));
    expect(take(restored.world.state_hash()) == take(host.world.state_hash()), "qualified masks and clocks restore exactly");
    return take(host.world.state_hash()) + session.state_sha256();
}
void access_checkpoint(const eawr::sim::PartitionExecutor& executor) {
    const auto config = fixture(); auto game = take(cp::GameSession::create(config));
    auto access = *game.galaxy().snapshot()->planet_access(); ++access.generation;
    access.planets[3].restricted = true; access.planets[3].local_revealed = false;
    auto encoded = std::vector<std::uint8_t>{}; gc::detail::encode_planet_access(encoded, access);
    done(game.restore_planet_access(encoded));
    const auto ids = eawr::checkpoint::game_identities(config);
    const auto bytes = take(eawr::checkpoint::save_game_checkpoint(game, ids));
    const auto loaded = take(eawr::checkpoint::load_game_checkpoint(bytes, ids, {}, executor));
    expect(*loaded.session.galaxy().snapshot()->planet_access() == *game.galaxy().snapshot()->planet_access()
        && loaded.session.state_sha256() == game.state_sha256(), "section 19 disk reconstruction preserves exact access state/hash");
    auto owner = std::make_shared<ai::GalacticWorld>(perception(false));
    done(owner->bind(game.galaxy().snapshot(), {}, executor, 0, game.galaxy().next_entity_id()));
    done(game.set_galactic_ai_services({owner, {}}));
    expect(!game.restore_planet_access(encoded), "GameSession refuses access restore after native installation");
    auto standalone = take(gc::GalacticSession::create(config));
    const auto before = standalone.state_sha256();
    expect(take(standalone.set_planet_access(2, true, false)), "authoritative access mutates");
    const auto generation = standalone.snapshot()->planet_access()->generation;
    expect(!take(standalone.set_planet_access(2, true, false)) && generation == standalone.snapshot()->planet_access()->generation, "no-op access preserves generation");
    expect(standalone.state_sha256() != before && !standalone.set_planet_access(999, false, true), "access participates in digest; invalid identity refuses");
    encoded.push_back(0); expect(!standalone.restore_planet_access(encoded), "malformed access restore refuses atomically");
}
}
int main() {
    try {
        for (const bool fog : {false, true}) {
            std::string reference;
            for (const unsigned workers : {1U, 2U, 4U, 8U}) {
                const auto result = visibility(workers, fog);
                if (reference.empty()) reference = result;
                else expect(result == reference, "qualified visibility and hashes equal on 1/2/4/8");
            }
        }
        eawr::sim::InlineExecutor executor; access_checkpoint(executor);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

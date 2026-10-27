#include "galactic_support.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <array>

namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using Fixed = eawr::sim::math::Fixed;

namespace {
Fixed damage(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
gc::ObserverSetup observer() {
    gc::ObserverSetup o;
    o.human_observers = {1}; o.solo_reference_human = 1;
    o.players = {{1, false, false, 0, 0, {3}}, {2, true, false, 0, 0, {}}, {3, true, false, 0, 0, {1}}};
    o.types = {{100, false, false, false}, {200, false, true, false},
        {300, true, false, false}, {400, false, false, true}};
    o.planets = {{10,2,2,2,2}};
    o.planets.front().current_owner = o.planets.front().current_controller = 2;
    return o;
}
std::vector<gc::FleetDisplayType> traits() {
    return {{100, false, false, false, true, damage(10), -1},
        {200, true, true, true, false, damage(20), 1},
        {300, false, false, false, false, damage(10), 2},
        {400, false, false, false, false, damage(1), 3}};
}
gc::GalacticSnapshot snapshot(gc::PlanetKnowledge knowledge, bool discovered = true,
    gc::FleetId enemy = 2, gc::PlayerId allegiance = 2,
    std::shared_ptr<const std::vector<gc::CensusObject>> actors = {}, std::vector<gc::Fleet> fleets = {},
    std::optional<gc::FleetControlSetup> fleet_control = {}) {
    auto rows = std::make_shared<std::vector<gc::ObservedPlanet>>();
    gc::ObservedPlanet row; row.player = 1; row.planet = 10; row.discovered = discovered; row.knowledge = knowledge;
    rows->push_back(row);
    row.player = 2; row.discovered = true; row.knowledge.fleet_contents = true; rows->push_back(row);
    auto current = observer(); current.planets.front().allegiance = allegiance;
    return gc::GalacticSnapshot(0, {}, {{1,1,0}, {2,2,0}, {3,3,0}}, {{10,{},2}}, {},
        fleets.empty() ? std::vector<gc::Fleet>{{1,1,10,{{11,100}}}, {enemy,2,10,{{21,100},{22,200},{23,100},{24,400}}},
         {3,2,10,{{31,300}}}, {4,3,10,{{41,200}}}} : std::move(fleets), {}, {}, std::move(fleet_control), {}, {}, {}, {}, {}, {}, {}, {},
        std::move(current), {}, {}, {}, rows, {}, {}, {}, {}, {}, {}, 0, {}, std::move(actors));
}
gc::FleetIntelligence project(const gc::GalacticSnapshot& s, gc::PlayerId local,
    std::span<const gc::FleetDisplayType> display, std::size_t capacity,
    std::span<const std::pair<eawr::sim::EntityId, std::int64_t>> indices = {}) {
    const std::array<gc::FleetDisplayVisibility,1> sight{{{10,true}}};
    return gc::observed_fleets(s, local, display, capacity, sight, {}, indices);
}
void masking() {
    gc::PlanetKnowledge k; k.fleet_presence = true;
    const auto s = snapshot(k); const auto bytes = s.canonical_bytes();
    const auto presence = project(s, 1, traits(), 8);
    expect(presence.presence.size() == 3 && presence.cards.size() == 2, "presence-only enemy has no detailed card; allies bypass knowledge");
    expect(presence.cards[1].groups[0].type == 200, "allied hero is revealed");
    expect(project(s, 2, traits(), 8).cards.empty(), "foreign player cannot request local projection");
    expect(project(snapshot(k, false), 1, traits(), 8).presence.size() == 3,
        "WGUI-06 visibility admission is independent of the permanent discovery flag");
    k.fleet_count = true;
    const auto counted = project(snapshot(k), 1, traits(), 8);
    expect(counted.cards.size() == 3 && counted.cards[1].count == 4, "enemy count available, all-stealth enemy omitted");
    expect(!counted.cards[1].fleet && !counted.cards[1].orbit_slot,
        "enemy fleet-count knowledge publishes neither fleet ID nor orbit slot");
    const auto& groups = counted.cards[1].groups;
    expect(groups[0].count == 2 && !groups[0].type && !groups[0].representative && !groups[0].transport, "count-only group contains no secret identity or transport metadata");
    expect(groups[1].count == 1 && !groups[1].type && !groups[1].representative, "enemy stealth named hero remains anonymous on discovered planet");
    expect(groups[2].type == 400 && groups[2].superweapon, "superweapon forces its detailed icon");
    k.fleet_contents = true;
    const auto full = project(snapshot(k), 1, traits(), 8);
    expect(full.cards[1].groups[0].type == 100 && full.cards[1].groups[0].gui_slot == 0, "full contents reveals ordinary transport and remaps -1 slot to zero");
    expect(full.cards[1].fleet == 2 && full.cards[1].orbit_slot == 0,
        "contents knowledge admits fleet identity and slot");
    expect(!full.cards[1].groups[1].type, "contents alone never reveals stealth hero");
    k.force_sensitive = true;
    expect(project(snapshot(k), 1, traits(), 8).cards[1].groups[1].type == 200, "force-sensitive category works independently of major detection");
    expect(!project(snapshot(k), 1, {}, 8).cards[1].groups[0].type, "missing hero metadata cannot classify enemy as ordinary");
    expect(s.canonical_bytes() == bytes, "projection does not mutate snapshot or canonical bytes");
}
void overview_image_masking() {
    gc::PlanetKnowledge knowledge; knowledge.fleet_presence = knowledge.fleet_count = true;
    const auto source = snapshot(knowledge);
    const auto bytes = source.canonical_bytes();
    const auto hash = source.sha256();
    const std::array<gc::FleetDisplayVisibility, 1> sight{{{10, true}}};
    const auto health = [](gc::TypeId type) -> std::optional<std::uint32_t> {
        if (type == 100) return 10;
        if (type == 200) return 50;
        if (type == 400) return 1;
        return std::nullopt;
    };
    const auto image = gc::observed_fleets(source, 1, traits(), 8, sight, health);
    const auto& enemy = image.cards[1];
    expect(enemy.overview_type == 200, "count-only overview selects the best ship separately from hidden contents");
    expect(!enemy.fleet && !enemy.orbit_slot && !enemy.groups[1].type && !enemy.groups[1].representative,
        "overview image grants no contents or entity/fleet identity");
    expect(gc::observed_fleets(source, 1, traits(), 0, sight, health).cards[1].overview_type == 200,
        "overview image is independent of contents-list capacity");
    knowledge.fleet_count = false;
    const auto unknown = gc::observed_fleets(snapshot(knowledge), 1, traits(), 8, sight, health);
    expect(std::none_of(unknown.cards.begin(), unknown.cards.end(), [](const auto& card) { return card.owner == 2; }),
        "presence-only enemy never gets an overview image");
    expect(source.canonical_bytes() == bytes && source.sha256() == hash,
        "overview projection changes no canonical bytes or state hash");
}
void live_actor_masking() {
    gc::PlanetKnowledge k; k.fleet_presence = k.fleet_count = k.fleet_contents = true;
    auto records = std::make_shared<std::vector<gc::CensusObject>>();
    for (const auto id : {11U, 21U, 22U, 23U, 24U, 31U, 41U}) {
        gc::CensusObject row; row.id = id; row.combat_role = gc::ActorCombatRole::ordinary;
        row.abilities = {true, true, true, {}}; row.garrison = false; records->push_back(row);
    }
    k.major_stealth = true;
    auto hidden = project(snapshot(k, true, 2, 2, records), 1, traits(), 8);
    expect(hidden.cards.size() == 2 && hidden.presence.size() == 2,
        "live all-stealth enemy remains hidden despite major hero intelligence; allies remain visible");
    expect(std::all_of(hidden.cards.begin(), hidden.cards.end(), [](const auto& card) { return card.all_stealth; }),
        "WGUI-92: every published card of an all-stealth fleet carries the stealth overlay flag");
    (*records)[1].abilities.combat_skip = false; k.major_stealth = false;
    const auto mixed = project(snapshot(k, true, 2, 2, records), 1, traits(), 8);
    expect(mixed.cards.size() == 3 && !mixed.cards[1].groups[1].type,
        "one live ordinary member admits mixed fleet while live stealth hero details remain masked");
    expect(std::any_of(mixed.cards.begin(), mixed.cards.end(), [](const auto& card) { return !card.all_stealth; }),
        "WGUI-92: a fleet with one non-stealth member shows no stealth overlay");
    (*records)[2].abilities.combat_skip = false;
    const auto ordinary = project(snapshot(k, true, 2, 2, records), 1, traits(), 8);
    expect(ordinary.cards[1].groups[1].type == 200,
        "removed live hero stealth overrides static type initial traits for HUD detail admission");
    (*records)[1].combat_role = gc::ActorCombatRole::unavailable;
    expect(project(snapshot(k, true, 2, 2, records), 1, traits(), 8).cards.size() == 2,
        "missing live owner fails closed without type fallback");
}
void strongest_and_capacity() {
    gc::PlanetKnowledge k; k.fleet_presence = k.fleet_count = k.strongest_ship = true;
    auto display = traits(); display[1].damage = damage(10);
    const auto projected = project(snapshot(k), 1, display, 8);
    expect(projected.cards[1].groups[0].strongest && projected.cards[1].groups[0].type == 100, "strict greater-than keeps first tied representative");
    display[1].damage.reset();
    expect(!project(snapshot(k), 1, display, 8).cards[1].groups[0].strongest, "incomplete current-Damage provider cannot claim strongest ship");
    expect(!project(snapshot(k), 1, traits(), 8).cards[1].groups[1].type, "strongest cannot bypass hero masking");
    const auto limited = project(snapshot(k), 1, traits(), 1);
    expect(limited.cards[1].groups.size() == 1 && limited.cards[1].groups[0].count == 1, "new-type limit stops before later duplicate");
    expect(project(snapshot(k), 1, display, 0).cards[1].groups.empty(), "zero capacity has no groups");
    expect(projected.fleets_examined == 4 && projected.observation_rows == 2 && projected.members_examined == 7, "one observation scan and bounded member pass, no fleets x planets work");
}
// WGEP-40/WGEP-66, GQP-11: orbit units become fleets of their own at slot 0, so
// one local slot holds several fleets; the panel shows them as one card.
void slot_merge() {
    gc::PlanetKnowledge k; k.fleet_presence = k.fleet_count = k.fleet_contents = true;
    const auto fleets = [] {
        return std::vector<gc::Fleet>{
            {1,1,10,{{11,100},{12,100}}, 0},
            {2,2,10,{{21,100},{22,100}}, 0},
            {3,3,10,{{31,200}}, 0},
            {5,1,10,{{51,100},{52,300}}, 0},
            {6,1,10,{{61,100}}, 1}};
    };
    const auto merged = project(snapshot(k, true, 2, 2, {}, fleets()), 1, traits(), 8);
    expect(merged.cards.size() == 4, "two local fleets of one slot publish one card; other slots and owners stay apart");
    const auto& local = merged.cards[0];
    expect(local.local && local.fleet == 1 && local.orbit_slot == 0 && local.count == 4,
        "slot card carries the first fleet's identity and the total count");
    expect(local.groups.size() == 2 && local.groups[0].type == 100 && local.groups[0].count == 3
        && local.groups[0].fleet == 1 && local.groups[0].representative == 11,
        "same-type members of both fleets share one group owned by the first fleet");
    expect(local.groups[1].type == 300 && local.groups[1].count == 1 && local.groups[1].fleet == 5
        && local.groups[1].representative == 52 && local.groups[1].gui_slot == 2,
        "a type only the second fleet holds binds that fleet");
    expect(gc::holds_fleet(local, 1) && gc::holds_fleet(local, 5) && !gc::holds_fleet(local, 6) && !gc::holds_fleet(local, 0),
        "a slot card holds the first fleet and every group's fleet");
    expect(!merged.cards[1].local && merged.cards[1].fleet == 2 && merged.cards[1].count == 2
        && merged.cards[1].groups.size() == 1 && merged.cards[1].groups[0].fleet == 2,
        "enemy card in the same slot stays its own fleet");
    expect(!merged.cards[2].local && merged.cards[2].fleet == 3, "allied card is not merged into the local slot card");
    expect(merged.cards[3].local && merged.cards[3].fleet == 6 && merged.cards[3].orbit_slot == 1 && merged.cards[3].count == 1,
        "another local slot keeps its own card");
    expect(merged.fleets_examined == 5 && merged.members_examined == 8, "merge adds no fleet or member passes");
    const auto limited = project(snapshot(k, true, 2, 2, {}, fleets()), 1, traits(), 1);
    expect(limited.cards[0].groups.size() == 1 && limited.cards[0].groups[0].count == 3 && limited.cards[0].count == 4,
        "WGUI-72 new-type limit applies across the merged scan");
    k.fleet_contents = false;
    const auto counted = project(snapshot(k, true, 2, 2, {}, fleets()), 1, traits(), 8);
    expect(counted.cards[0].groups[1].fleet == 5 && !counted.cards[1].fleet && !counted.cards[1].groups[0].fleet,
        "count-only enemy publishes no fleet identity while local groups keep theirs");
    // A count-only enemy superweapon group names no fleet either (review-2959).
    const auto superweapon = project(snapshot(k), 1, traits(), 8);
    expect(superweapon.cards[1].groups.size() == 3 && superweapon.cards[1].groups[2].representative == 24
        && !superweapon.cards[1].groups[2].fleet, "count-only superweapon group publishes no fleet id");
}
// WGUI-72 (#3079): a company in orbit is its transport member (live type 100);
// the card groups and shows it by the actor's original type, the company.
void company_true_type() {
    gc::PlanetKnowledge k; k.fleet_presence = k.fleet_count = k.fleet_contents = true;
    auto records = std::make_shared<std::vector<gc::CensusObject>>();
    for (const auto& [id, original] : std::array<std::pair<eawr::sim::EntityId, gc::TypeId>, 5>{
             {{11, 500}, {12, 500}, {13, 0}, {21, 500}, {22, 500}}}) {
        gc::CensusObject row; row.id = id; row.original_type = original; row.current_type = 100;
        row.owner = id > 20 ? 2 : 1;
        // A live nonstealth enemy, so its fleet is admitted by count knowledge.
        if (id > 20) { row.combat_role = gc::ActorCombatRole::ordinary; row.abilities = {true, false, false, {}}; }
        records->push_back(row);
    }
    const auto fleets = [] {
        return std::vector<gc::Fleet>{{1,1,10,{{11,100},{12,100},{13,100}}, 0}, {2,2,10,{{21,100},{22,100}}, 0}};
    };
    const auto projected = project(snapshot(k, true, 2, 2, records, fleets()), 1, traits(), 8);
    expect(projected.cards.size() == 2 && projected.cards[0].count == 3, "one local card with every member counted");
    const auto& groups = projected.cards[0].groups;
    expect(groups.size() == 2 && groups[0].type == 500 && groups[0].count == 2 && groups[0].representative == 11,
        "transports of one company type group as that company");
    expect(groups[1].type == 100 && groups[1].count == 1 && groups[1].representative == 13,
        "a member without an original type keeps its live type");
    expect(projected.cards[1].groups.size() == 1 && projected.cards[1].groups[0].type == 500,
        "an enemy company with contents knowledge shows as the company");
    k.fleet_contents = false;
    const auto counted = project(snapshot(k, true, 2, 2, records, fleets()), 1, traits(), 8);
    expect(counted.cards.size() == 2 && counted.cards[1].count == 2 && counted.cards[1].groups.size() == 1
        && counted.cards[1].groups[0].count == 2 && !counted.cards[1].groups[0].type
        && !counted.cards[1].groups[0].representative,
        "count-only enemy knowledge never reveals the company type");
}
void worker_parity() {
    std::optional<gc::FleetIntelligence> baseline; std::string hash;
    for (const auto workers : {1U,2U,4U,8U}) {
        gc::GalacticSetup s;
        s.players = {{1,1,0}, {2,2,0}, {3,3,0}};
        s.seats = {{{1},1,gc::SeatKind::human,0,true,1}};
        s.planets = {{10,{},2}}; s.fleets = {{20,2,10,{{21,100},{22,200}}},
            {30,1,10,{{31,100}}}, {35,1,10,{{36,100},{37,300}}}};
        s.observer = observer();
        for (const auto& player : s.players) s.observer->records.push_back({player.player_id,10,true});
        s.observer->players[0].default_intelligence = gc::bit(gc::Intelligence::fleet_count);
        // Already-discovered checkpoint rows retain their published mask until
        // an explicit observer refresh; a default policy alone is not knowledge.
        s.observer->records[0].mask = gc::bit(gc::Intelligence::fleet_count);
        auto created = gc::GalacticSession::create(s);
        if (!created) throw std::runtime_error(created.error().message);
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto stepped = session.step(executor);
        if (!stepped) throw std::runtime_error(stepped.error().message);
        const auto state_hash = session.snapshot()->sha256();
        const std::array<gc::FleetDisplayVisibility, 1> sight{{{10, true}}};
        const auto projection = gc::observed_fleets(*session.snapshot(), 1, traits(), 8, sight,
            [](gc::TypeId type) -> std::optional<std::uint32_t> { return static_cast<std::uint32_t>(type); });
        expect(session.snapshot()->sha256() == state_hash, "overview projection preserves the authoritative hash");
        const auto enemy = std::find_if(projection.cards.begin(), projection.cards.end(),
            [](const auto& card) { return card.owner == 2; });
        expect(enemy != projection.cards.end() && enemy->overview_type == 200,
            "best enemy image exists with every worker count");
        const auto local = std::count_if(projection.cards.begin(), projection.cards.end(),
            [](const auto& card) { return card.local; });
        const auto merged = std::find_if(projection.cards.begin(), projection.cards.end(),
            [](const auto& card) { return card.local; });
        const bool one_card = local == 1 && merged->fleet == 30 && merged->orbit_slot == 0 && merged->count == 3
            && merged->groups.size() == 2 && merged->groups[0].count == 2 && merged->groups[1].fleet == 35;
        if (!one_card) {
            for (const auto& card : projection.cards) {
                std::cerr << "card fleet=" << (card.fleet ? static_cast<long long>(*card.fleet) : -1)
                    << " local=" << card.local << " slot=" << (card.orbit_slot ? int(*card.orbit_slot) : -1)
                    << " count=" << card.count << " groups=" << card.groups.size() << '\n';
            }
        }
        expect(one_card, "local fleets of one slot publish one merged card");
        if (!baseline) { baseline = projection; hash = session.snapshot()->sha256(); }
        expect(*baseline == projection && hash == session.snapshot()->sha256(), "1/2/4/8 workers preserve projection and snapshot hash");
    }
}
void delayed_sight() {
    gc::PlanetKnowledge k; k.fleet_presence = k.fleet_count = k.fleet_contents = true;
    const std::array<gc::FleetDisplayVisibility,1> seen{{{10,true}}}, hidden{{{10,false}}};
    const auto e = snapshot(k, true, 2);
    expect(gc::observed_fleets(e, 1, traits(), 8, seen).cards.size() == 3,
        "enemy E is admitted only with explicit current sight");
    const auto f = snapshot(k, true, 5);
    const auto delayed = gc::observed_fleets(f, 1, traits(), 8, hidden);
    expect(delayed.cards.empty() && delayed.presence.empty(),
        "E leaves and F arrives during delayed reveal: permanent discovery/retained knowledge exposes no new fleet");
    expect(gc::observed_fleets(f, 1, traits(), 8).cards.empty(),
        "absent current-sight provider fails closed independently of allegiance/discovery");
    expect(gc::observed_fleets(snapshot(k, true, 5, 1), 1, traits(), 8).presence.empty(),
        "a currently allied planet with no sight-provider value still exposes no markers");
    const std::array<gc::FleetDisplayVisibility,1> missing{{{10,std::nullopt}}};
    expect(gc::observed_fleets(f, 1, traits(), 8, missing).presence.empty(),
        "an unavailable predicate result exposes no marker even with retained knowledge");
    expect(gc::observed_fleets(f, 1, traits(), 8, seen).cards[1].fleet == 5,
        "F appears after the current-sight owner admits it");
}
}
void gui_index_projection() {
    std::vector<std::pair<eawr::sim::EntityId, std::int64_t>> indices{{11, 4}, {21, 7}};
    gc::PlanetKnowledge k; k.fleet_count = true;
    const std::vector<gc::Fleet> fleets{{1, 1, 10, {{11, 100}, {12, 100}, {13, 100}}},
        {2, 2, 10, {{21, 100}, {22, 100}}}};
    const auto original = snapshot(k, true, 2, 2, {}, fleets);
    const auto canonical = original.canonical_bytes();
    const auto view = project(original, 1, traits(), 8, indices);
    expect(view.cards[0].groups.size() == 2 && view.cards[0].groups[0].count == 1
        && view.cards[0].groups[0].gui_index == 4 && view.cards[0].groups[1].count == 2
        && !view.cards[0].groups[1].gui_index, "one positioned member splits from its unpositioned type stack");
    expect(view.cards[1].groups.size() == 1 && view.cards[1].groups[0].count == 2
        && !view.cards[1].groups[0].gui_index && !view.cards[1].groups[0].representative,
        "enemy count-only grouping does not reveal saved GUI indices");
    indices = {{11, 4}, {12, 4}, {13, 4}};
    const auto stack = project(original, 1, traits(), 8, indices);
    expect(stack.cards[0].groups.size() == 1 && stack.cards[0].groups[0].count == 3
        && stack.cards[0].groups[0].gui_index == 4, "Ctrl-positioned type stack keeps one shared cell");
    expect(original.canonical_bytes() == canonical, "local per-object order changes no simulation bytes");
}
// Duplicate lookup rows keep the first one, as the map-based tables did.
void duplicate_rows_keep_first() {
    gc::PlanetKnowledge k; k.fleet_presence = k.fleet_count = k.fleet_contents = true;
    const auto s = snapshot(k);
    const std::array<gc::FleetDisplayVisibility, 2> sight{{{10, true}, {10, false}}};
    const std::array<gc::FleetDisplayVisibility, 2> hidden{{{10, false}, {10, true}}};
    expect(!gc::observed_fleets(s, 1, traits(), 8, sight).cards.empty(), "first visibility row for a planet wins (visible)");
    expect(gc::observed_fleets(s, 1, traits(), 8, hidden).cards.empty(), "first visibility row for a planet wins (hidden)");
    auto twice = traits(); twice.push_back({100, true, true, true, true, damage(99), 5});
    const std::array<gc::FleetDisplayVisibility, 1> seen{{{10, true}}};
    expect(gc::observed_fleets(s, 1, twice, 8, seen) == gc::observed_fleets(s, 1, traits(), 8, seen),
        "first trait row for a type wins");
}
// Synthetic mod-scale publication (250 planets, 12 factions): prints a digest of the projection and the median
// call time so BASE and HEAD can be compared. The timing is informational and never asserted.
void scale_digest() {
    constexpr gc::PlanetId planets = 250; constexpr gc::PlayerId factions = 12;
    auto setup = observer();
    setup.players.clear(); setup.planets.clear();
    for (gc::PlayerId p = 1; p <= factions; ++p) setup.players.push_back({p, p != 1, false, 0, 0, p % 3 == 1 ? std::vector<gc::PlayerId>{1, 4} : std::vector<gc::PlayerId>{}});
    std::sort(setup.players.begin(), setup.players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    std::vector<gc::Planet> planet_rows;
    auto observed = std::make_shared<std::vector<gc::ObservedPlanet>>();
    std::vector<gc::Fleet> fleets;
    std::vector<gc::FleetDisplayVisibility> sight;
    eawr::sim::EntityId entity = 1000; gc::FleetId fleet = 1;
    for (gc::PlanetId id = 10; id < 10 + planets; ++id) {
        planet_rows.push_back({id, {}, static_cast<gc::PlayerId>(1 + id % factions)});
        setup.planets.push_back({id, 2, 2, 2, 2});
        for (gc::PlayerId p = 1; p <= factions; ++p) {
            gc::ObservedPlanet row; row.player = p; row.planet = id; row.discovered = true;
            row.knowledge.fleet_presence = row.knowledge.fleet_count = true; row.knowledge.fleet_contents = id % 2 == 0;
            observed->push_back(row);
        }
        sight.push_back({id, id % 7 != 0});
        for (unsigned f = 0; f < 3; ++f) {
            gc::Fleet value{fleet++, static_cast<gc::PlayerId>(1 + (id + f) % factions), id, {}};
            value.orbit_slot = static_cast<std::uint8_t>(f % 2);
            for (unsigned m = 0; m < 6; ++m) value.members.push_back({entity++, static_cast<gc::TypeId>(100 * (1 + (id + m + f) % 4))});
            fleets.push_back(std::move(value));
        }
    }
    std::vector<gc::GalacticPlayer> player_rows;
    for (gc::PlayerId p = 1; p <= factions; ++p) player_rows.push_back({p, p, 0});
    const gc::GalacticSnapshot source(0, {}, std::move(player_rows), std::move(planet_rows), {}, std::move(fleets),
        {}, {}, std::nullopt, {}, {}, {}, {}, {}, {}, {}, {}, std::move(setup), {}, {}, {}, observed, {}, {}, {}, {}, {}, {}, 0, {}, {});
    const auto run = [&] { return gc::observed_fleets(source, 1, traits(), 35, sight); };
    const auto result = run();
    std::uint64_t digest = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t v) { digest = (digest ^ v) * 1099511628211ULL; };
    for (const auto& p : result.presence) { mix(p.planet); mix(p.owner); }
    for (const auto& c : result.cards) {
        mix(c.planet); mix(c.owner); mix(c.count); mix(c.local); mix(c.fleet.value_or(0)); mix(c.orbit_slot.value_or(9));
        mix(c.overview_type.value_or(0));
        for (const auto& g : c.groups) { mix(g.count); mix(g.gui_slot); mix(g.type.value_or(0)); mix(g.representative.value_or(0)); mix(g.fleet.value_or(0)); mix(g.transport); mix(g.strongest); mix(g.superweapon); }
    }
    mix(result.observation_rows); mix(result.fleets_examined); mix(result.members_examined);
    std::vector<double> times;
    for (int i = 0; i < 40; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        const auto again = run();
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
        if (again.cards.size() != result.cards.size()) throw std::runtime_error("projection changed between calls");
    }
    std::sort(times.begin(), times.end());
    std::cout << "OBSERVED-FLEETS cards=" << result.cards.size() << " presence=" << result.presence.size()
        << " digest=" << digest << " median_us=" << times[times.size() / 2] << std::endl;
    expect(!result.cards.empty(), "the scale projection publishes cards");
}
int case_galactic_fleet_intelligence_tests() {
    try { masking(); overview_image_masking(); live_actor_masking(); strongest_and_capacity(); slot_merge(); company_true_type(); worker_parity(); delayed_sight(); gui_index_projection(); duplicate_rows_keep_first(); scale_digest(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

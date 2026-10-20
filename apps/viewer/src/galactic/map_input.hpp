#pragma once

#include "galaxy_view.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/graph.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <type_traits>

namespace eawr::viewer::galactic {

// WGUI-47/55: use the faction's published discovery, never ownership or another
// coop seat's numeric ID. A configured observer with no row fails closed.
[[nodiscard]] inline std::optional<bool> observer_discovery(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId player, ObjectId id) {
    if (!snapshot.observer()) return std::nullopt;
    const auto rows = snapshot.observations();
    // The validated complete observer table publishes rows in (player, planet) order.
    const auto row = std::lower_bound(rows.begin(), rows.end(), std::pair{player, id}, [](const auto& value, const auto& key) {
        return value.player < key.first || (value.player == key.first && value.planet < key.second);
    });
    return row != rows.end() && row->player == player && row->planet == id && row->discovered;
}

// WGUI-05/60/62: the alternate surface set one planet draws. An undiscovered galaxy planet shows the
// authored grid shell (alternate 2) in whatever scene layer it sits, but only when its model has one;
// a model with no alternate-2 surface (a mod planet) keeps the camera's rule so it never vanishes.
// Scene-only art and non-planet objects never take the shell. Only the world layer follows the camera.
struct PlanetShellInput {
    bool galaxy_planet{}, scene_art{}, discovered{}, has_shell{}, world_layer{}, camera_target{};
    int camera_alternate{};
};
[[nodiscard]] inline int planet_alternate(const PlanetShellInput& art) {
    if (!art.galaxy_planet) return 0;
    if (!art.scene_art && !art.discovered && art.has_shell) return 2;
    return art.world_layer && art.camera_target ? art.camera_alternate : 0;
}
// A surface tagged with an alternate draws only when it matches; untagged surfaces always draw.
[[nodiscard]] inline bool surface_drawn(int authored_alternate, int desired_alternate) {
    return authored_alternate < 0 || authored_alternate == desired_alternate;
}
// WGUI-05: a model has the grid shell when one of its drawn surfaces is tagged alternate 2. A mod model
// with only alternate 0/1 surfaces has none, and its planet keeps the camera rule while undiscovered.
template <class Surfaces>
[[nodiscard]] inline bool has_shell_surface(const Surfaces& surfaces) {
    return std::any_of(surfaces.begin(), surfaces.end(), [](const auto& surface) {
        return surface.status == "drawn" && surface.alternate == 2; });
}
// WGUI-05: the galaxy planets that show the shell for lack of discovery; scene-only art never does.
// The HUD resubmits the instances exactly when this list changes between publications.
[[nodiscard]] inline std::vector<ObjectId> undiscovered_planets(std::span<const ObjectId> galaxy_planets,
    std::span<const ObjectId> scene_art, const std::function<bool(ObjectId)>& discovered) {
    std::vector<ObjectId> result;
    for (const auto id : galaxy_planets)
        if (std::find(scene_art.begin(), scene_art.end(), id) == scene_art.end() && !discovered(id)) result.push_back(id);
    return result;
}

// Presentation inputs are copied from an observer snapshot and the owning HUD.
// No guesses about widget activation distance, reverse filters or ability opcodes
// (WGUI-U04); those producers supply explicit intents once qualified.
struct InputPlanet {
    ObjectId id{};
    Vec3 position;
    PlayerId allegiance{};
    bool discovered{};
};
struct InputGates {
    bool galactic{true}, hud_enabled{true}, active_mode{true};
    bool action_menu{}, selection_locked{}, tutorial_locked{};
    bool demo{}, cinematic{}, victory_pending{}, tutorial_scroll{true};
    [[nodiscard]] bool world_allowed() const {
        return active_mode && !demo && !cinematic && !victory_pending;
    }
};
struct SelectionEffects {
    bool accepted{}, changed{}, deferred{}, position{}, refocus{}, zoom{}, toggle_zoom{}, tutorial{};
    ObjectId target{};
};
enum class Navigation { next, previous, next_own, next_other };
struct InputWidget {
    ObjectId id{}, object{}, planet{};
    float x{}, y{};
    bool ground{}, drag_select{}, visible{}, enabled{}, stacked_drag{};
};
struct PointerPoint { float x{}, y{}; };
enum class DropKind { move, merge, land, orbit_slot, launch, ability };
using AbilityIntent = std::variant<sim::galactic::PrepareGalacticAbility,
    sim::galactic::ExecutePreparedAbility, sim::galactic::CancelGalacticAbility>;
struct DropIntent {
    DropKind kind{};
    ObjectId source{}, target{}, planet{};
    std::vector<sim::galactic::PlanetId> path;
    std::uint8_t slot{};
    // WGUI-54: a launch drop carries the primary surface unit (`source`) first, then every other
    // multiselected unit in selection order. Empty means the source alone.
    std::vector<ObjectId> members;
    // #3072: a move dropped on a planet's stack icon asks for that orbit slot on arrival.
    std::optional<std::uint8_t> destination_slot;
    std::optional<AbilityIntent> ability{};
};

// One queued execution per published capability, scoped to its session lifetime
// so a replacement/load can publish the same numeric preparation again.
struct PendingAbilityDrop {
    std::weak_ptr<const void> owner{};
    std::optional<sim::galactic::PreparedAbility> prepared{};
    [[nodiscard]] bool matches(const std::weak_ptr<const void>& current_owner,
        const sim::galactic::PreparedAbility& capability) const {
        return prepared && *prepared == capability && !owner.owner_before(current_owner)
            && !current_owner.owner_before(owner);
    }
    void arm(const std::weak_ptr<const void>& current_owner, const sim::galactic::PreparedAbility& capability) {
        owner = current_owner; prepared = capability;
    }
    void observe(const sim::galactic::PreparedAbility* capability) {
        if (prepared && (!capability || *prepared != *capability)) { prepared.reset(); owner.reset(); }
    }
};

// WGUI-172: bind a published actor/container capability to the fleet being dragged.
// Follow current observer identities; missing, dead, foreign or cyclic links fail closed.
[[nodiscard]] inline bool ability_source_in_fleet(const sim::galactic::GalacticSnapshot& snapshot,
    PlayerId local, ObjectId source, ObjectId fleet_id) {
    const auto fleets = snapshot.fleets();
    const auto fleet = std::lower_bound(fleets.begin(), fleets.end(), fleet_id,
        [](const auto& row, auto id) { return row.id < id; });
    if (fleet == fleets.end() || fleet->id != fleet_id || fleet->owner != local || fleet->motion.transit) return false;
    const auto actors = snapshot.actor_records();
    for (std::size_t remaining = actors.size(); source && remaining; --remaining) {
        const auto actor = std::lower_bound(actors.begin(), actors.end(), source,
            [](const auto& row, auto id) { return row.id < id; });
        if (actor == actors.end() || actor->id != source || actor->owner != local || actor->dead || actor->delete_pending)
            return false;
        // Setup/allocation share one object-ID namespace: a transport cannot alias
        // a fleet root, and a direct fleet parent is represented by that fleet's ID.
        if (actor->container == fleet_id) return actor->planet == fleet->orbit;
        source = actor->container;
    }
    return false;
}

// WGCH-151..155 / WGUI-172: the hero ability one dragged local fleet offers at the planet it is parked over.
// Every action shares the planet's special-action slot; the selector tries destruction, theft, sabotage, then
// neutralization (WGCH-152; the black-market, spy, siphon, corruption and removal actions have their own entries).
// Among several actors the first in stored order whose type authors an action wins; the original-type ranking
// between several qualified heroes is unverified (WGCH-152 rank tie-break) and kept as the least visible choice.
// Availability is the authored profile plus the target rule that needs no hidden state: a planet the local
// player does not own. The host revalidates eligibility, ownership, price and preparation identity.
struct AbilityDrop {
    ObjectId actor{}, planet{};
    sim::galactic::AbilityKind kind{};
    friend bool operator==(const AbilityDrop&, const AbilityDrop&) = default;
};
[[nodiscard]] inline std::optional<sim::galactic::AbilityKind> authored_drop_kind(
    const sim::galactic::GalacticAbilityProfile& profile) {
    using sim::galactic::AbilityKind;
    if (profile.lifecycle_only) return std::nullopt;
    if (profile.destruction) return AbilityKind::planet_destruction;
    if (profile.theft) return AbilityKind::technology_theft;
    if (profile.sabotage) return AbilityKind::structure_sabotage;
    if (profile.neutralize) return AbilityKind::hero_neutralization;
    return std::nullopt;
}
[[nodiscard]] inline std::optional<AbilityDrop> ability_drop_candidate(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local, ObjectId fleet_id,
    const sim::galactic::GalacticAbilitySetup& host) {
    const auto fleets = snapshot.fleets();
    const auto fleet = std::lower_bound(fleets.begin(), fleets.end(), fleet_id,
        [](const auto& row, auto id) { return row.id < id; });
    if (fleet == fleets.end() || fleet->id != fleet_id || fleet->owner != local || fleet->motion.transit) return std::nullopt;
    const auto planets = snapshot.planets();
    const auto target = std::find_if(planets.begin(), planets.end(), [&](const auto& row) { return row.id == fleet->orbit; });
    if (target == planets.end() || target->owner == local) return std::nullopt;
    for (const auto& actor : snapshot.actor_records()) {
        if (actor.owner != local || actor.dead || actor.delete_pending) continue;
        const auto profile = std::find_if(host.profiles.begin(), host.profiles.end(),
            [&](const auto& row) { return row.type == actor.current_type; });
        if (profile == host.profiles.end()) continue;
        const auto kind = authored_drop_kind(*profile);
        if (kind && ability_source_in_fleet(snapshot, local, actor.id, fleet_id))
            return AbilityDrop{actor.id, fleet->orbit, *kind};
    }
    return std::nullopt;
}
// The typed order a release on the slot submits: preparation for the menu abilities, whose published menu
// then opens; destruction queues its unpriced execution directly (the host holds no menu for it, WGCH-156).
[[nodiscard]] inline AbilityIntent ability_drop_order(const AbilityDrop& drop) {
    using sim::galactic::AbilityKind;
    if (drop.kind == AbilityKind::planet_destruction) {
        sim::galactic::PreparedAbility queued;
        queued.actor = drop.actor; queued.planet = drop.planet; queued.kind = drop.kind;
        return sim::galactic::ExecutePreparedAbility{queued};
    }
    return sim::galactic::PrepareGalacticAbility{drop.actor, drop.planet, drop.kind};
}

// WGUI-49/50: resolve the live identity's parents, rather than accepting a
// widget's cached planet. Fleet shells need not have census rows; their orbit is authoritative.
[[nodiscard]] inline ObjectId actor_containing_planet(const sim::galactic::GalacticSnapshot& snapshot,
    ObjectId actor_id) {
    const auto actors = snapshot.actor_records();
    const auto planets = snapshot.planets();
    const auto fleets = snapshot.fleets();
    for (std::size_t remaining = actors.size() + 1; actor_id && remaining; --remaining) {
        const auto fleet = std::lower_bound(fleets.begin(), fleets.end(), actor_id,
            [](const auto& row, auto id) { return row.id < id; });
        if (fleet != fleets.end() && fleet->id == actor_id) {
            const auto record = std::lower_bound(actors.begin(), actors.end(), actor_id,
                [](const auto& row, auto id) { return row.id < id; });
            if (record != actors.end() && record->id == actor_id && (record->dead || record->delete_pending)) return 0;
            if (fleet->motion.transit) return 0;
            return std::any_of(planets.begin(), planets.end(), [&](const auto& row) { return row.id == fleet->orbit; })
                ? fleet->orbit : 0;
        }
        const auto actor = std::lower_bound(actors.begin(), actors.end(), actor_id,
            [](const auto& row, auto id) { return row.id < id; });
        if (actor == actors.end() || actor->id != actor_id || actor->dead || actor->delete_pending)
            return 0;
        if (std::any_of(planets.begin(), planets.end(), [&](const auto& row) { return row.id == actor->container; }))
            return actor->container;
        actor_id = actor->container;
    }
    return 0;
}
[[nodiscard]] inline ObjectId first_hero_planet(const sim::galactic::GalacticSnapshot& snapshot,
    sim::galactic::TypeId type) {
    if (!type) return 0;
    for (const auto& actor : snapshot.actor_records())
        if (actor.current_type == type && !actor.dead && !actor.delete_pending)
            return actor_containing_planet(snapshot, actor.id);
    return 0;
}

// #3072: the local fleets one dragged card or stack icon stands for: every parked
// (not travelling) local fleet at `planet` in one of `slots`, in fleet-id order,
// so the card's first fleet leads (fleet_intelligence.hpp's local slot card).
[[nodiscard]] inline std::vector<ObjectId> stack_fleets(const sim::galactic::GalacticSnapshot& snapshot,
    PlayerId local, ObjectId planet, std::span<const std::uint8_t> slots) {
    std::vector<ObjectId> result;
    for (const auto& fleet : snapshot.fleets())
        if (fleet.owner == local && fleet.orbit == planet && !fleet.motion.transit
            && std::find(slots.begin(), slots.end(), fleet.orbit_slot) != slots.end()) result.push_back(fleet.id);
    return result;
}

// #3072: a dragged stack moves as one. Every other fleet of `stack` first merges
// into the order's fleet (the merge target for a merge drop), then `order` follows.
// One seat submits them in this order, so the merges execute before it.
[[nodiscard]] inline std::vector<sim::galactic::CommandPayload> stack_commands(const DropIntent& intent,
    std::span<const ObjectId> stack, sim::galactic::CommandPayload order) {
    const auto into = intent.kind == DropKind::merge ? intent.target : intent.source;
    std::vector<sim::galactic::CommandPayload> result;
    for (const auto fleet : stack)
        if (fleet && fleet != intent.source && fleet != into) result.emplace_back(sim::galactic::MergeFleets{fleet, into});
    result.push_back(std::move(order));
    return result;
}

// Project preview binding: a selected local fleet and a map destination become
// an ordinary typed drop. Reuse WGM-16..22 search and the published movement
// access table; never draw a direct line through an intermediate conflict.
[[nodiscard]] inline std::optional<std::vector<sim::galactic::PlanetId>> destination_path(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local, ObjectId source,
    ObjectId destination, std::span<const InputPlanet> planets) {
    using namespace sim::galactic;
    const auto target = std::find_if(planets.begin(), planets.end(), [&](const auto& row) { return row.id == destination; });
    if (target == planets.end() || !target->discovered) return std::nullopt;
    if (destination == source) return std::nullopt;
    const auto* movement = snapshot.movement();
    if (!movement || !snapshot.maximum_movement_distance()) return std::nullopt;
    auto graph = PlanetGraph::build(snapshot.planets(), snapshot.routes(), *snapshot.maximum_movement_distance());
    if (!graph) return std::nullopt;
    std::vector<PathPlanet> access;
    for (const auto& node : graph.value().nodes()) {
        const auto input = std::find_if(planets.begin(), planets.end(), [&](const auto& row) { return row.id == node.id; });
        const auto permitted = std::find_if(movement->access.begin(), movement->access.end(),
            [&](const auto& row) { return row.player == local && row.planet == node.id; });
        PathPlanet row; row.id = node.id;
        row.revealed = input != planets.end() && input->discovered
            && permitted != movement->access.end() && permitted->accessible;
        row.fleet_presence_visible = true;
        row.nonallied_fleet = permitted == movement->access.end() || permitted->potential_conflict;
        access.push_back(row);
    }
    PathOptions options; options.allow_bad_final = true;
    // WGM-18: multiplayer omits the local-reveal gate; the saved session flag decides, never the viewer.
    options.multiplayer = snapshot.victory() && snapshot.victory()->context.multiplayer;
    auto path = find_planet_path(graph.value(), source, destination, access, options);
    if (!path || path.value().planets.size() < 2) return std::nullopt;
    return std::move(path).value().planets;
}

[[nodiscard]] inline std::optional<DropIntent> fleet_destination_intent(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local, ObjectId selected_fleet,
    ObjectId destination, std::span<const InputPlanet> planets) {
    const auto fleets = snapshot.fleets();
    const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == selected_fleet; });
    if (fleet == fleets.end() || fleet->owner != local || fleet->motion.transit) return std::nullopt;
    const auto target = std::find_if(planets.begin(), planets.end(), [&](const auto& row) { return row.id == destination; });
    if (target == planets.end() || !target->discovered) return std::nullopt;
    if (destination == fleet->orbit) return DropIntent{DropKind::land, selected_fleet, 0, destination, {}, 0, {}, std::nullopt};
    auto path = destination_path(snapshot, local, fleet->orbit, destination, planets);
    if (!path) return std::nullopt;
    return DropIntent{DropKind::move, selected_fleet, 0, destination, std::move(*path), 0, {}, std::nullopt};
}

class MapInput final {
public:
    InputGates gates;
    ObjectId selected{}, deferred{}, home{};
    unsigned delay{};
    bool held_scroll{}, held_rotate{}, hyperspace{}, double_release{}, tooltip{};
    std::optional<PointerPoint> box_start, box_end;
    ObjectId dragged{};
    std::vector<ObjectId> multiselection;
    // WGUI-52: these describe the rectangle only, never widget drag activation.
    static constexpr float rectangle_outline = 3.0F, rectangle_alpha = 0.2F;

    [[nodiscard]] static const InputPlanet* planet(std::span<const InputPlanet> planets, ObjectId id) {
        const auto it = std::find_if(planets.begin(), planets.end(), [id](const auto& p) { return p.id == id; });
        return it == planets.end() ? nullptr : &*it;
    }
    // WGUI-47/48: caller locks are deliberately absent from the core setter.
    SelectionEffects select(std::span<const InputPlanet> planets, ObjectId id,
        CameraState camera, bool delayed = false) {
        SelectionEffects effect;
        effect.target = id;
        if (!gates.galactic || gates.action_menu) return effect;
        effect.tutorial = true;
        if (id == selected) { effect.accepted = true; return effect; }
        const auto* target = planet(planets, id);
        if (id && !target) return effect;
        if (id && delayed && camera != CameraState::focused) {
            deferred = id; delay = 2;
            effect.accepted = effect.deferred = effect.position = true;
            return effect;
        }
        if (target && !target->discovered) return effect;
        selected = id;
        effect.accepted = effect.changed = true;
        effect.refocus = id && (camera == CameraState::focused || camera == CameraState::inward);
        return effect;
    }
    // The HUD service owns when this is called (WGUI-U01). Revalidate on delivery.
    SelectionEffects service_deferred(std::span<const InputPlanet> planets, CameraState camera) {
        if (!delay || --delay) return {};
        const auto target = deferred;
        deferred = 0;
        return select(planets, target, camera);
    }
    SelectionEffects world_select(std::span<const InputPlanet> planets, ObjectId id,
        CameraState camera, bool twice = false) {
        if (!gates.world_allowed() || gates.selection_locked) return {};
        const bool same = id && id == selected;
        auto effect = select(planets, id, camera);
        effect.zoom = effect.accepted && same && twice;
        effect.toggle_zoom = effect.zoom;
        double_release = twice && effect.accepted;
        return effect;
    }
    SelectionEffects cancel_world(std::span<const InputPlanet> planets, CameraState camera) {
        if (!gates.world_allowed()) return {};
        multiselection.clear();
        if (gates.galactic && !gates.selection_locked && camera == CameraState::overview)
            return select(planets, 0, camera);
        return {};
    }
    // WGUI-49/75: own and other intentionally differ in lock and return testing.
    SelectionEffects navigate(std::span<const InputPlanet> planets, PlayerId local,
        Navigation direction, CameraState camera) {
        if (planets.empty() || (direction == Navigation::next_own && gates.selection_locked)) return {};
        const auto current = std::find_if(planets.begin(), planets.end(), [&](const auto& p) { return p.id == selected; });
        const auto count = planets.size();
        const auto start = current == planets.end() ? (direction == Navigation::previous ? 0 : count - 1)
            : static_cast<std::size_t>(current - planets.begin());
        for (std::size_t offset = 1; offset <= count; ++offset) {
            const auto index = direction == Navigation::previous ? (start + count - offset) % count
                : (start + offset) % count;
            const auto& candidate = planets[index];
            if (direction == Navigation::next_own && candidate.allegiance != local) continue;
            if (direction == Navigation::next_other && candidate.allegiance == local) continue;
            if ((direction == Navigation::next || direction == Navigation::previous) && !candidate.discovered) continue;
            auto effect = select(planets, candidate.id, camera);
            if (effect.accepted || direction == Navigation::next_own) {
                effect.position = effect.tutorial = true;
                return effect;
            }
        }
        SelectionEffects effect; effect.tutorial = true; return effect;
    }
    SelectionEffects display_index(std::span<const InputPlanet> planets, std::size_t index, CameraState camera) {
        if (index >= planets.size() || !planets[index].discovered) return {};
        return select(planets, planets[index].id, camera);
    }
    // The snapshot producer resolves first-match leader/hero containment (WGUI-49/50).
    SelectionEffects go_home(std::span<const InputPlanet> planets, ObjectId first_leader_planet, CameraState camera) {
        if (!home && planet(planets, first_leader_planet)) home = first_leader_planet;
        if (!planet(planets, home)) return {};
        auto effect = select(planets, home, camera); effect.position = true; return effect;
    }
    SelectionEffects hero(std::span<const InputPlanet> planets, bool nonnull_type, bool inactive,
        ObjectId containing_planet, CameraState camera, bool twice) {
        if (gates.selection_locked || !nonnull_type || inactive || !planet(planets, containing_planet)) return {};
        auto effect = select(planets, containing_planet, camera);
        effect.position = true; effect.zoom = twice && effect.accepted; return effect;
    }
    void cancel_pointer() {
        held_scroll = held_rotate = hyperspace = double_release = tooltip = false;
        box_start.reset(); box_end.reset(); dragged = 0;
    }
    bool hud_event() {
        if (!gates.galactic || !gates.hud_enabled) return false;
        if (gates.tutorial_locked) { cancel_pointer(); return false; }
        return true;
    }
    bool begin_box(PointerPoint point, bool allied, bool valid_index) {
        if (!hud_event()) return false;
        if (dragged || box_start) return true;
        if (!allied || !valid_index) return false;
        box_start = point; return true;
    }
    void move_box(PointerPoint point) {
        if (!box_start) return;
        const float x = point.x - box_start->x, y = point.y - box_start->y;
        if (box_end || x * x + y * y > 144.0F) box_end = point;
    }
    bool release_box(std::span<const InputWidget> widgets) {
        if (!box_start || !box_end) { box_start.reset(); return false; }
        multiselection.clear();
        const auto a = *box_start, b = *box_end;
        for (const auto& w : widgets) {
            if (w.ground && w.drag_select && w.visible && w.enabled && w.object
                && w.x >= std::min(a.x, b.x) && w.x <= std::max(a.x, b.x)
                && w.y >= std::min(a.y, b.y) && w.y <= std::max(a.y, b.y)) multiselection.push_back(w.id);
        }
        box_start.reset(); box_end.reset(); return true;
    }
    bool begin_widget(const InputWidget& widget, bool control, bool focused_fleet,
        const std::function<void()>& merge_home) {
        if (!hud_event() || dragged || !widget.visible || !widget.enabled || !widget.object) return false;
        if (control && !widget.stacked_drag) return false;
        if (control && focused_fleet) {
            if (!merge_home) return false; // #2263: require the focused-fleet producer.
            merge_home(); // WGUI-54: precedes activation.
        }
        dragged = widget.id; return true;
    }
    bool release_widget(const std::function<void(ObjectId)>& drop) {
        if (!dragged) {
            if (double_release) { double_release = false; return true; }
            return false;
        }
        drop(dragged);
        for (const auto id : multiselection) if (id != dragged) drop(id);
        dragged = 0; double_release = false; return true;
    }
    [[nodiscard]] bool wheel_zoom() const { return gates.world_allowed() && !held_scroll && !held_rotate; }
    [[nodiscard]] std::optional<Vec3> radar(PointerPoint point, float width, float height) const {
        if (!gates.tutorial_scroll || !gates.world_allowed()) return std::nullopt;
        return radar_to_world({point.x, point.y, 0}, width, height);
    }
    // WGUI-55 / WGM-23: typed intents enter the ordinary seat-issued command sink.
    // Path search/acceptance stays in movement; the UI rechecks discovery at dispatch.
    [[nodiscard]] std::optional<sim::galactic::CommandPayload> command(
        const DropIntent& intent, std::span<const InputPlanet> planets) const {
        if (!gates.galactic || !gates.world_allowed() || gates.tutorial_locked || !intent.source) return std::nullopt;
        using namespace sim::galactic;
        switch (intent.kind) {
        case DropKind::move:
            if (intent.path.size() < 2 || intent.path.back() != intent.planet) return std::nullopt;
            for (const auto id : intent.path) {
                const auto* target = planet(planets, id);
                if (!target || !target->discovered) return std::nullopt;
            }
            if (intent.destination_slot && *intent.destination_slot > 2) return std::nullopt;
            return MoveFleet{intent.source, intent.path, intent.destination_slot};
        case DropKind::merge:
            if (!intent.target || intent.source == intent.target) return std::nullopt;
            return MergeFleets{intent.source, intent.target};
        case DropKind::land: {
            const auto* target = planet(planets, intent.planet);
            if (!target || !target->discovered) return std::nullopt;
            return LandFleet{intent.source, intent.planet};
        }
        case DropKind::orbit_slot: return SetFleetOrbitSlot{intent.source, intent.slot};
        case DropKind::launch: {
            // WGM-13/WGUI-55 (#2880): surface units onto a same-planet orbit fleet or orbit slot.
            // The simulation owns the transport, ownership, blockade and capacity admission.
            const auto* origin = planet(planets, intent.planet);
            if (!origin || !origin->discovered || intent.slot > 2 || (intent.target && intent.slot)) return std::nullopt;
            LaunchFromSurface order;
            order.target = intent.target; order.slot = intent.slot;
            order.units = intent.members.empty() ? std::vector<ObjectId>{intent.source} : intent.members;
            if (order.units.front() != intent.source) return std::nullopt;
            for (std::size_t i = 0; i < order.units.size(); ++i)
                if (!order.units[i] || std::find(order.units.begin(), order.units.begin() + static_cast<std::ptrdiff_t>(i),
                    order.units[i]) != order.units.begin() + static_cast<std::ptrdiff_t>(i)) return std::nullopt;
            if (!intent.path.empty()) {
                if (intent.target || intent.path.size() < 2 || intent.path.front() != intent.planet
                    || intent.path.back() == intent.planet || (intent.destination_slot && *intent.destination_slot > 2)) return std::nullopt;
                for (const auto id : intent.path) {
                    const auto* node = planet(planets, id);
                    if (!node || !node->discovered) return std::nullopt;
                }
                return LaunchFleet{std::move(order.units), intent.path, intent.destination_slot};
            }
            return order;
        }
        case DropKind::ability: {
            // WGUI-172: preserve the producer's typed capability. The sim revalidates
            // eligibility, ownership, price and preparation identity at execution.
            if (!gates.hud_enabled || gates.action_menu || gates.selection_locked || !intent.ability) return std::nullopt;
            return std::visit([&](const auto& order) -> std::optional<CommandPayload> {
                using Order = std::decay_t<decltype(order)>;
                if constexpr (std::is_same_v<Order, CancelGalacticAbility>) {
                    if (order.actor != intent.source || !order.preparation) return std::nullopt;
                } else {
                    const auto actor = [&] {
                        if constexpr (std::is_same_v<Order, PrepareGalacticAbility>) return order.actor;
                        else return order.prepared.actor;
                    }();
                    const auto destination = [&] {
                        if constexpr (std::is_same_v<Order, PrepareGalacticAbility>) return order.planet;
                        else return order.prepared.planet;
                    }();
                    const auto* target = planet(planets, intent.planet);
                    if (actor != intent.source || destination != intent.planet || !target || !target->discovered)
                        return std::nullopt;
                }
                return CommandPayload{order};
            }, *intent.ability);
        }
        }
        return std::nullopt;
    }
};

} // namespace eawr::viewer::galactic

#pragma once

#include "galaxy_view.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/graph.hpp"

#include <algorithm>
#include <functional>
#include <span>
#include <utility>

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
};

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
[[nodiscard]] inline std::optional<DropIntent> fleet_destination_intent(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local, ObjectId selected_fleet,
    ObjectId destination, std::span<const InputPlanet> planets) {
    using namespace sim::galactic;
    const auto fleets = snapshot.fleets();
    const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == selected_fleet; });
    if (fleet == fleets.end() || fleet->owner != local || fleet->motion.transit) return std::nullopt;
    const auto target = std::find_if(planets.begin(), planets.end(), [&](const auto& row) { return row.id == destination; });
    if (target == planets.end() || !target->discovered) return std::nullopt;
    if (destination == fleet->orbit) return DropIntent{DropKind::land, selected_fleet, 0, destination, {}, 0, {}, std::nullopt};
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
    auto path = find_planet_path(graph.value(), fleet->orbit, destination, access, options);
    if (!path || path.value().planets.size() < 2) return std::nullopt;
    return DropIntent{DropKind::move, selected_fleet, 0, destination, std::move(path).value().planets, 0, {}, std::nullopt};
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
    // Containment resolution belongs to the snapshot/widget producer (#2263). First-match
    // leader/hero order is preserved by accepting its resolved containing planet.
    SelectionEffects go_home(std::span<const InputPlanet> planets, ObjectId first_leader_planet, CameraState camera) {
        if (!home) home = first_leader_planet;
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
            return order;
        }
        case DropKind::ability: return std::nullopt; // #2263: hero/ability eligibility and command producer.
        }
        return std::nullopt;
    }
};

} // namespace eawr::viewer::galactic

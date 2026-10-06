#pragma once

#include "eawr/sim/commands.hpp"

#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

// #535: how a unit fades as the local player's space fog reveals or hides it
// (docs/behaviour/space-fog-presentation.md FW-16 to FW-18). This is the engine-free part: a
// per-entity ease toward full or zero opacity, driven by the live session's visibility, on FoC's
// logical-frame clock (fog_field.hpp's FW-05 clock, one tick a frame, tactical::logical_frames_
// per_second). The viewer applies the result as the unit's own opacity (live_session_view.cpp).
// Presentation only: nothing reaches the simulation, and it never changes what
// TacticalSnapshot::visible_entities says a player sees.
namespace eawr::presentation::space {

// FW-16, FW-18: the ease's characteristic time and the opacity below which a fading-out unit
// stops being drawn, FoC's defaults.
struct UnitFadeLooks {
    double smooth_seconds{0.25};
    double hide_threshold{0.025};
};

// WPJ-38/39: one projectile's observer hide service. The query belongs to the
// visibility owner; endpoint visibility never participates. Delayed shots stay
// in limbo and do not start their visibility clock until appearance.
class ProjectileHide final {
public:
    using Query = std::function<bool()>;
    void advance(double tick, bool limbo, bool immediate, const Query& visible);
    [[nodiscard]] float opacity() const noexcept { return static_cast<float>(value_); }
    [[nodiscard]] static constexpr bool model_visible(double opacity) noexcept { return !(opacity < 0.025); }
    [[nodiscard]] bool drawn() const noexcept { return started_ && model_visible(value_); }
    [[nodiscard]] bool revealed() const noexcept { return target_; }
private:
    bool started_{};
    bool target_{};
    double value_{};
    double velocity_{};
    double last_tick_{};
    double checked_tick_{};
};

// FW-16 to FW-18: a unit first tracked while visible shows at once. One first tracked while
// fogged eases in when later revealed; any visible unit eases toward zero when hidden,
// and is forgotten once it eases below
// `hide_threshold` while hidden or leaves `alive` (destroyed). Every ease shares one
// characteristic time; none restarts at either end when the target flips back before it settles.
class UnitFade final {
public:
    explicit UnitFade(UnitFadeLooks looks = {});

    // One rendered frame. `visible` is the entities the local player's fog shows this tick,
    // ascending (TacticalSnapshot::visible_entities); `alive` is every entity the session still
    // holds, ascending (TacticalSnapshot::instances' IDs), a superset of `visible`. `frames` is
    // the logical frames the battle advanced since the last call (0 while paused; fast forward
    // and fractions are kept, as fog_field.hpp's FW-05 clock). Catch-up uses constant work
    // per tracked unit; non-finite or negative elapsed frames leave the ease unchanged.
    void advance(std::span<const sim::EntityId> visible, std::span<const sim::EntityId> alive, double frames,
                 std::span<const sim::EntityId> immediate = {});

    // The opacity of a drawn entity (0 hidden, 1 fully shown); nullopt for one this call does not
    // draw at all (never yet seen, or faded fully out).
    [[nodiscard]] std::optional<float> opacity(sim::EntityId entity) const noexcept;
    // Entities the last advance() draws: fully shown ones and those still easing out (FW-18),
    // ascending.
    [[nodiscard]] std::vector<sim::EntityId> drawn() const;

private:
    struct State {
        double value{1.0};
        double velocity{0.0};
        bool target_visible{true};
    };

    UnitFadeLooks looks_;
    std::map<sim::EntityId, State> fading_;
    std::vector<sim::EntityId> ever_seen_; // ascending; every entity `alive` has ever held
};

// FW-25..28: one observer's memory, independent of simulation lifetime and
// sensor contacts. The caller owns immutable render copies, keyed by entity.
class FogGhosts final {
public:
    struct Observation {
        sim::EntityId entity{};
        std::array<double, 3> position{};
        bool visible{};
        bool initially_known{};
    };
    struct State {
        std::array<double, 3> position{};
        bool known{};
        bool ghost{};
    };
    using RevealedPoint = std::function<bool(const std::array<double, 3>&)>;
    void advance(std::span<const Observation> observations, const RevealedPoint& revealed);
    [[nodiscard]] const std::map<sim::EntityId, State>& states() const noexcept { return states_; }
private:
    std::map<sim::EntityId, State> states_;
};

// WHZ-71: serviced before the contact target changes, independently of fog visibility.
class NebulaBlend final {
public:
    void service(bool contact);
    [[nodiscard]] float value() const noexcept { return static_cast<float>(value_); }
private:
    double value_{};
    double velocity_{};
    bool contact_{};
};

} // namespace eawr::presentation::space

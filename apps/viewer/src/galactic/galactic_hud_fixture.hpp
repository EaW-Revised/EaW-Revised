#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "galactic_mode.hpp"
#include "galaxy_view.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "battle_dialog_model.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/presentation/ui/layout.hpp"

namespace eawr::presentation::godot_backend {
// Optional capture/test inputs. Normal play has no provider. Implementations live
// beside their capture fixtures and never supply authored campaign facts.
class GalacticHudFixture {
public:
    enum class Feature { fleet_visibility, ground_placement, intelligence,
        unaligned_planets, driven_transit, battle_dialog, corruption_overview };
    struct RotationInput { double elapsed{}; bool paused{}; };
    virtual std::optional<presentation::ui::PixelRect> ground_stack_rect(GalacticMode::State&,
        std::string_view, std::size_t, const presentation::ui::ShellPlacement&) const { return {}; }
    virtual ~GalacticHudFixture() = default;
    [[nodiscard]] virtual bool enabled(Feature) const { return false; }
    [[nodiscard]] virtual std::optional<bool> score_space() const { return {}; }
    [[nodiscard]] virtual std::optional<RotationInput> rotation_input(std::uint64_t) const { return {}; }
    virtual bool configure_scene(GalacticMode::State&, viewer::galactic::GalaxySnapshotStub&) { return true; }
    virtual bool configure_setup(GalacticMode::State&) { return true; }
    virtual bool start_session(GalacticMode::State&) { return true; }
    virtual bool prepare_ability(GalacticMode::State&) { return true; }
    virtual bool black_market(GalacticMode::State&) { return true; }
    virtual bool destruction(GalacticMode::State&) { return true; }
    virtual bool hero_action(GalacticMode::State&) { return true; }
    virtual bool weapon_action(GalacticMode::State&) { return true; }
    virtual bool corruption(GalacticMode::State&) { return true; }
    virtual bool orbit(GalacticMode::State&) { return true; }
    virtual bool configure_hud(GalacticMode::State&) { return true; }
    virtual void update_hud(GalacticMode::State&) {}
    virtual void observation(GalacticMode::State&, sim::galactic::ObservedPlanet&,
        sim::galactic::ObserverPlanet&, sim::galactic::PlanetProduction&,
        std::optional<double>&, std::optional<double>&, viewer::galactic::ObjectId) {}
    virtual std::optional<bool> choose_battle(GalacticMode::State&, std::uint64_t, viewer::galactic::BattleChoice) { return std::nullopt; }
    virtual bool battle_dialog(GalacticMode::State&) { return true; }
    virtual void publish_battle(GalacticMode::State&) {}
    virtual void corruption_frame(GalacticMode::State&, double) {}
    [[nodiscard]] virtual std::optional<std::uint64_t> corruption_display_frame() const { return {}; }
    virtual bool configure_score(GalacticMode::State&, platform::LiveGameSession::Options&) { return true; }
    virtual bool run_score_ledger(GalacticMode::State&) { return true; }
};

// Command-line capture adapter; returns no provider when no fixture was requested.
[[nodiscard]] std::shared_ptr<GalacticHudFixture> make_capture_hud_fixture(const GalacticMode::Options&);
} // namespace eawr::presentation::godot_backend

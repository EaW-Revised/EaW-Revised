#pragma once
#include "galactic_mode.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/story_summary.hpp"
#include "eawr/sim/galactic/story_goals.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace godot { class Button; class Control; class Label; class PanelContainer; }

namespace eawr::presentation::godot_backend {
class EawrOverlayButton;

// WST-58: the story summary the campaign opens at the intro goal's chapter. It consumes only the session's unhashed
// story presentation feed (goal rows and summary requests) and the dialog file's text; it owns no campaign state.
// Placeholder chrome until the authored story dialog skin is wired (no sound, no speech, no movie, no teletype).
class StorySummaryWidget final {
public:
    void service(GalacticMode::State&);
    [[nodiscard]] const presentation::ui::StorySummary& model() const noexcept { return model_; }

private:
    void feed(GalacticMode::State&);
    void build(GalacticMode::State&);
    void redraw(GalacticMode::State&);
    presentation::ui::StorySummary model_;
    std::map<std::string, sim::galactic::story::Goal> goals_; // by tag, from the feed's goal rows
    std::uint64_t cursor_{}, drawn_{};
    std::shared_ptr<data::ui::TextDatabase> text_;
    godot::Control* root_{};
    godot::PanelContainer* panel_{};
    godot::Label* title_{};
    godot::Label* body_{};
    godot::Button* close_{};
    EawrOverlayButton* relay_{};
};
void service_story_summary(GalacticMode::State&);
} // namespace eawr::presentation::godot_backend

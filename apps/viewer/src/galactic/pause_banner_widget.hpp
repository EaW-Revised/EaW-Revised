#pragma once
#include "galactic_mode.hpp"

#include "ui/battle_overlay.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/battle_messages.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace godot { class Control; }

namespace eawr::presentation::godot_backend {
class EawrBattleOverlay;

// The "Game Paused" banner of the galactic map (#6759): while the galactic clock is paused the pause shell's frame and
// text hang at the top centre of the screen with a Resume Game button under them, drawn like the tactical pause banner
// (TM-09: the same shell, texts and colour) and acting like the clock panel's pause button. The banner is presentation
// only: it reads the host's paused flag and asks the host to resume through the same request path as the button.
// Nothing here is saved, hashed or replayed.
class PauseBannerWidget final {
public:
    void service(GalacticMode::State&);
    [[nodiscard]] std::string report_json() const;

private:
    void build(GalacticMode::State&);
    // WGEP-173: the stacked production countdown lines of the strategic map.
    struct CountdownType {
        bool enabled{};
        bool coloured{}; // the type authors a readable Build_Countdown_Text_RGBA; without one no line is drawn
        godot::String label;
        godot::Color colour;
    };
    [[nodiscard]] const CountdownType& countdown_type(GalacticMode::State&, const std::string& type);
    [[nodiscard]] std::vector<EawrBattleOverlay::CountdownLine> countdown_lines(GalacticMode::State&);
    EawrBattleOverlay* overlay_{};
    bool tried_{};
    std::shared_ptr<data::ui::TextDatabase> text_;
    std::map<std::string, CountdownType> countdown_types_;
    presentation::ui::BattleMessageLooks looks_; // GameConstants looks, read once in build()
};
void service_pause_banner(GalacticMode::State&);
} // namespace eawr::presentation::godot_backend

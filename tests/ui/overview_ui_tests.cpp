// #848: the battle UI in the tactical overview levels (docs/behaviour/foc-battle-selection.md V-5a to
// V-5g): what hides at x1 and x2, and the nine-frame cross-fade of every level change. The viewer
// side (the HUD, the brackets and the held image in a live battle) is test_battle_input.py.

#include "eawr/presentation/ui/overview_ui.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "ui_test_support.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

void test_visibility() {
    const ui::OverviewUi off = ui::overview_ui(false);
    expect(off.tactical_shell && off.pause_banner && off.radar_contents && off.unit_brackets && off.world_markers
               && off.distance_fog,
           "the tactical camera draws everything");
    const ui::OverviewUi on = ui::overview_ui(true);
    expect(!on.tactical_shell, "V-5b: the tactical shell hides in the overview");
    expect(!on.pause_banner, "V-5b: the pause banner hides in the overview");
    expect(!on.radar_contents, "V-5c: the radar contents stop in the overview");
    expect(!on.unit_brackets, "V-5d: the unit brackets hide in the overview");
    expect(on.world_markers, "V-5e: squadron icons, reticles, circles, the drag box and the pointer stay");
    expect(!on.distance_fog, "V-5f: the distance fog is off in the overview");
}

void test_reinforcement_during_movie() {
    for (const bool initially_open : {false, true}) {
        bool pane_open = initially_open;
        bool dragging = true;
        const auto update = [&](bool movie) {
            const auto shell = ui::overview_ui(movie).tactical_shell;
            ui::apply_reinforcement_availability(ui::reinforcement_available(true, shell, movie, false),
                [&] { pane_open = false; }, [&] { dragging = false; });
            return shell && pane_open;
        };
        expect(update(false) == initially_open, "initial pane state is shown");
        expect(!update(true), "movie hides the pane");
        expect(update(false) == initially_open, "movie exit preserves the prior pane state");
        ui::apply_reinforcement_availability(false, [&] { pane_open = false; }, [&] { dragging = false; });
        expect(!pane_open && !dragging, "lost eligibility closes the pane and cancels the drag");
    }
    expect(!ui::reinforcement_available(true, false, false, false), "overview still closes the pane");
    expect(!ui::reinforcement_available(false, false, true, false), "movie does not override game restrictions");
    expect(!ui::reinforcement_available(true, false, true, true), "movie does not enable land reinforcement");
}

void test_fade() {
    ui::OverviewFade fade;
    expect(!fade.next_frame() && fade.drawn_frames() == 0, "no fade before a level change");
    fade.request();
    const float expected[] = {0.9F, 0.8F, 0.7F, 0.6F, 0.5F, 0.4F, 0.3F, 0.2F, 0.1F};
    for (std::uint32_t frame = 0; frame < ui::overview_fade_frames; ++frame) {
        const auto opacity = fade.next_frame();
        expect(opacity && std::abs(*opacity - expected[frame]) < 1.0e-6F,
               "V-5g: fade frame " + std::to_string(frame + 1) + " lays the held image at its fixed step");
    }
    expect(!fade.next_frame() && !fade.running(), "V-5g: the held image is gone on the tenth frame");
    expect(fade.drawn_frames() == 9 && fade.requests() == 1, "one fade draws nine frames");

    fade.request();
    static_cast<void>(fade.next_frame());
    static_cast<void>(fade.next_frame());
    fade.request();
    const auto restarted = fade.next_frame();
    expect(restarted && std::abs(*restarted - 0.9F) < 1.0e-6F, "a level change during a fade restarts it at 0.9");
    std::uint32_t rest = 0;
    while (fade.next_frame()) ++rest;
    expect(rest == 8 && fade.requests() == 3, "the restarted fade runs its nine frames");
}

} // namespace

int main() {
    test_visibility();
    test_reinforcement_during_movie();
    test_fade();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " overview UI check(s) failed\n";
        return 1;
    }
    std::cout << "ui overview passed\n";
    return 0;
}

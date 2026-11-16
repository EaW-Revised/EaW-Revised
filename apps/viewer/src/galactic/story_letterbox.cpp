#include "galactic_mode_internal.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/movie_letterbox.hpp"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace eawr::presentation::godot_backend {
using namespace godot;

// legacy #6634 (HCB-11c, BCAM-04): the cinematic bands a story script asked for with Letter_Box_In / Letter_Box_Out. The
// bands are the same two opaque black bands the battle camera draws, each `Camera_FX_Manager_Letterbox_Height` of the
// screen height (authored, validated by movie_letterbox_height), sliding over the seconds the script gave. While the
// letterbox is on the game GUI is hidden (HCB-11c). Presentation only: it reads the unsaved StoryHudState the story
// presentation feed drives and never reaches the simulation.
void GalacticMode::State::service_story_letterbox() {
    if (!hud || !galaxy_container) return;
    const bool visible = story_hud.letterbox_visible();
    if (!visible && !story_bands) return; // never asked for: nothing built, nothing to undo
    if (visible && !story_bands && !story_band_height_failed) {
        auto height = filesystem ? presentation::ui::movie_letterbox_height(*filesystem)
                                 : core::Result<float>::failure(core::Diagnostic{});
        if (!height) {
            story_band_height_failed = true; // one diagnostic, not one per frame
            UtilityFunctions::push_warning(String::utf8(("[eawr-story] no letterbox bands: "
                + (filesystem ? core::format_diagnostic(height.error()) : std::string("no game data is mounted"))).c_str()));
        } else {
            story_band_height = height.value();
            story_bands = memnew(Control);
            story_bands->set_name("GalacticStoryLetterbox");
            story_bands->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            story_bands->set_z_index(2);
            galaxy_container->add_child(story_bands);
            for (const bool bottom : {false, true}) {
                auto* band = memnew(ColorRect);
                band->set_color(Color(0, 0, 0, 1));
                band->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                story_bands->add_child(band);
                band->set_anchor(SIDE_RIGHT, 1.0F, true);
                band->set_anchor(SIDE_TOP, bottom ? 1.0F : 0.0F, true);
                band->set_anchor(SIDE_BOTTOM, bottom ? 1.0F : 0.0F, true);
            }
            story_bands->hide();
        }
    }
    // The GUI follows the target at once (as the battle camera's does); the bands follow the slide.
    // Only this code hides `hud` today, so showing it again when no letterbox is asked for cannot override another hide.
    const bool gui_hidden = visible && story_hud.letterbox_on() && story_bands != nullptr;
    if (hud->is_visible() == gui_hidden) hud->set_visible(!gui_hidden);
    if (!story_bands) return;
    const float height = presentation::ui::movie_letterbox_band_fraction(story_band_height.value_or(0.0F), story_hud.letterbox_progress());
    if (story_bands->is_visible() != visible) story_bands->set_visible(visible);
    if (!visible) return;
    // The parent is a bare control in a container: keep it at the viewport's size, including after a resize.
    const Vector2 size = story_bands->get_viewport()->get_visible_rect().size;
    if (story_bands->get_size() != size) { story_bands->set_position(Vector2()); story_bands->set_size(size); }
    if (auto* top = Object::cast_to<Control>(story_bands->get_child(0))) top->set_anchor(SIDE_BOTTOM, height, true);
    if (auto* bottom = Object::cast_to<Control>(story_bands->get_child(1))) bottom->set_anchor(SIDE_TOP, 1.0F - height, true);
}

} // namespace eawr::presentation::godot_backend

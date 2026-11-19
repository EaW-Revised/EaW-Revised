#pragma once
#include "galactic_mode.hpp"
#include "story_text_state.hpp"
#include "text_reveal.hpp"
#include "eawr/data/ui/text_database.hpp"
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace godot { class Control; class Label; class VBoxContainer; }

namespace eawr::presentation::godot_backend {

// #236 PR B (WST-47, WST-48): the story's objective list and SCREEN_TEXT lines. It consumes only the session's unhashed
// story presentation feed and the text database; it owns no campaign state. Placeholder chrome: plain labels in fixed
// corners, text ids resolved through the English text database (a missing id shows as itself), no speech,
// no sound, and the authored colour and style words are read by the model but not drawn (their look is unverified).
class StoryTextWidget final {
public:
    void service(GalacticMode::State&);
    // A new or loaded session: forget the old session's list and lines but keep the one overlay (the widget builds its
    // controls once per mode, so a load never stacks a second overlay on the first).
    void reset();

private:
    void build(GalacticMode::State&);
    void redraw(GalacticMode::State&);
    viewer::galactic::StoryTextState model_;
    std::uint64_t cursor_{}, ticks_{};
    std::string drawn_;
    std::shared_ptr<data::ui::TextDatabase> text_;
    bool text_tried_{}; // one attempt at the text database: a missing file is not retried on every frame
    std::optional<float> reveal_rate_;
    struct RevealedText final {
        std::uint64_t identity{};
        viewer::galactic::TextReveal reveal;
        std::vector<godot::String> text;
        std::vector<godot::Label*> labels;
    };
    std::vector<RevealedText> revealed_;
    godot::Control* root_{};
    godot::VBoxContainer* objectives_{};
    godot::VBoxContainer* lines_{};
};
void service_story_text(GalacticMode::State&);
} // namespace eawr::presentation::godot_backend

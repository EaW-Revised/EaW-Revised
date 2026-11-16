#pragma once
#include "galactic_mode.hpp"
#include "ui/dialog_builder.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/story_summary.hpp"
#include "eawr/sim/galactic/story_goals.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <godot_cpp/variant/vector2.hpp>

namespace godot { class Control; }

namespace eawr::presentation::godot_backend {
class EawrOverlayButton;

// The story summary / "Active Story Objectives" window behind the story-arc button (WST-58/68/70, WGUI-36, H088),
// built on the authored story summary dialog: the Active and Completed tabs, the goal list, the chapter's "incoming
// transmission" header and recap, the hint and the Close button. It consumes only the session's unhashed story
// presentation feed (goal rows and summary requests) and the dialog file's text; it owns no campaign state. Which goals
// are viewed is local state (not saved): HCB-71. No sound, speech, movie or teletype.
class StorySummaryWidget final {
public:
    void service(GalacticMode::State&);
    void open(GalacticMode::State&);
    // WGUI-36: the story button has something to open.
    [[nodiscard]] bool has_goals() const noexcept { return model_.has_goals(); }
    [[nodiscard]] bool visible() const noexcept { return model_.visible(); } // the window is open (a modal layer)
    [[nodiscard]] const presentation::ui::StorySummary& model() const noexcept { return model_; }
    [[nodiscard]] std::string report_json() const;
    // A new or loaded session: forget the goals, the viewed marks and the feed position; the window itself stays built.
    void reset();

private:
    struct Recap { // what the dialog shows for the selected goal: resolved text, ready to put in the lists
        std::string header;
        std::vector<std::pair<std::string, std::optional<std::array<std::uint8_t, 4>>>> lines; // unwrapped, one per TEXT/NEWLINE
    };
    void feed(GalacticMode::State&);
    void rebuild(GalacticMode::State&);
    void refresh(GalacticMode::State&);
    void close();
    void choose(std::int64_t index);
    [[nodiscard]] std::string resolve(GalacticMode::State&, const std::string& key);
    [[nodiscard]] const std::string& dialog_text(GalacticMode::State&, const std::string& name);
    [[nodiscard]] std::optional<presentation::ui::StoryChapterText> chapter_of(GalacticMode::State&, const presentation::ui::StoryGoalRow&);
    presentation::ui::StorySummary model_;
    std::uint64_t cursor_{}, drawn_{};
    bool dirty_{true};
    std::shared_ptr<data::ui::TextDatabase> text_;
    bool text_tried_{};
    std::map<std::string, std::string> dialog_texts_; // by dialog name: the file's text, read once
    godot::Control* root_{};
    std::optional<BuiltDialog> dialog_;
    std::vector<EawrOverlayButton*> relays_;
    godot::Vector2 built_size_;
    godot::Vector2 failed_size_; // the size a dialog build failed at: not retried until the size changes
    std::size_t build_failures_{};
    std::vector<std::string> arial_families_; std::size_t system_family_count_{}; // report only: what the host offers
    std::vector<std::string> list_tags_; // the goal list's rows, in order, for selection
    Recap recap_;
    std::size_t rebuilds_{};
};
void service_story_summary(GalacticMode::State&);
} // namespace eawr::presentation::godot_backend

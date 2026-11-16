#include "galactic/story_hud_state.hpp"
#include "galactic/story_text_state.hpp"
#include "eawr/platform/live_story_feed.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <utility>
#include <vector>

// #236 (G9/G10; WST-75): the local HUD state a story plot drives, without the engine's widgets. Synthetic component
// names only: nothing in the class knows a stock or mod control.
using namespace eawr::viewer::galactic;
namespace st = eawr::sim::galactic::story;

namespace {
st::ConsumerRequest flash(std::string name) { return {st::Consumer::gui, "FLASH_GUI", {std::move(name)}}; }
st::ConsumerRequest control(std::string reward, std::string value) { return {st::Consumer::control, std::move(reward), {std::move(value)}}; }
} // namespace

int case_galactic_story_hud_state_tests() {
    {
        // A flash pulses for three 0.5 s periods, peaks half-way through each, then leaves and is reported finished once.
        StoryHudState hud;
        assert(hud.apply(flash("b_synthetic")));
        assert(hud.flashes().size() == 1 && hud.intensity(hud.flashes().front()) < 1e-9);
        hud.advance(0.25);
        assert(hud.intensity(hud.flashes().front()) > 0.999);
        hud.advance(0.25);
        assert(hud.intensity(hud.flashes().front()) < 1e-9);
        hud.advance(0.99);
        assert(hud.flashes().size() == 1 && hud.take_finished().empty());
        hud.advance(0.02);
        assert(hud.flashes().empty());
        assert((hud.take_finished() == std::vector<std::string>{"b_synthetic"}) && hud.take_finished().empty());
    }
    {
        // The same component (any case) restarts its pulse instead of stacking; another component runs beside it; a
        // negative step moves nothing.
        StoryHudState hud;
        hud.apply(flash("B_One"));
        hud.advance(1.0);
        hud.apply(flash("b_one"));
        hud.apply(flash("b_two"));
        assert(hud.flashes().size() == 2 && hud.flashes().front().elapsed == 0.0);
        hud.advance(-5.0);
        assert(hud.flashes().front().elapsed == 0.0);
    }
    {
        // The list is bounded: a plot cannot grow it without end.
        StoryHudState hud;
        for (int i = 0; i < 200; ++i) hud.apply(flash("b_" + std::to_string(i)));
        assert(hud.flashes().size() == StoryHudState::max_flashes);
    }
    {
        // LOCK_CONTROLS and SCROLL_LOCK: nonzero locks, zero unlocks, independently of each other.
        StoryHudState hud;
        assert(!hud.controls_locked() && !hud.scroll_locked());
        assert(hud.apply(control("LOCK_CONTROLS", "1")) && hud.controls_locked() && !hud.scroll_locked());
        assert(hud.apply(control("SCROLL_LOCK", "1")) && hud.scroll_locked());
        assert(hud.apply(control("LOCK_CONTROLS", "0")) && !hud.controls_locked() && hud.scroll_locked());
        assert(hud.apply(control("SCROLL_LOCK", "0")) && !hud.scroll_locked());
    }
    {
        // Requests that are not this class's are left alone, never an abort.
        StoryHudState hud;
        assert(!hud.apply({st::Consumer::gui, "FORCE_CLICK_GUI", {"b_x"}}));
        assert(!hud.apply({st::Consumer::gui, "FLASH_GUI", {}}));
        assert(!hud.apply({st::Consumer::control, "LOCK_CONTROLS", {}}));
        assert(!hud.apply({st::Consumer::camera, "LOCK_CONTROLS", {"1"}}));
        assert(!hud.apply({st::Consumer::control, "SOMETHING_ELSE", {"1"}}));
        assert(hud.flashes().empty() && !hud.controls_locked() && !hud.scroll_locked());
    }
    {
        // #236 PR B: the HUD's objective list mirrors the session's through the published events, for any sequence of rewards.
        st::Goals goals(1); st::FakePresentationSink sink;
        StoryTextState view;
        const auto run = [&](st::ObjectiveAction action, std::string identity = {}, std::string text = {}, std::int64_t index = -1, bool visible = true) {
            st::ObjectiveReward reward{action, std::move(identity), {std::move(text), false, 0, st::ObjectiveStatus::incomplete}, visible};
            reward.index = index;
            const auto before = sink.events.size();
            const bool applied = goals.apply(reward, sink);
            for (auto i = before; i < sink.events.size(); ++i) assert(view.apply(sink.events[i]));
            assert(applied || sink.events.size() == before);
            assert(view.objectives() == goals.state().objectives && view.objectives_visible() == goals.state().objectives_visible);
        };
        run(st::ObjectiveAction::add, {}, "a"); run(st::ObjectiveAction::add, {}, "b"); run(st::ObjectiveAction::add, {}, "a");
        run(st::ObjectiveAction::add, {}, "front", 0); run(st::ObjectiveAction::complete, "a"); run(st::ObjectiveAction::fail, "b");
        run(st::ObjectiveAction::replace, "a", "b"); run(st::ObjectiveAction::remove, "front"); run(st::ObjectiveAction::complete, "none");
        run(st::ObjectiveAction::add, {}, "mid", 1);
        run(st::ObjectiveAction::display, {}, {}, -1, false); run(st::ObjectiveAction::remove_all);
        assert(view.objectives().empty() && !view.objectives_visible());
    }
    {
        // #236 PR B review: a reader that falls behind the bounded feed resynchronises from the published objective state, not
        // from the events it lost. 300 objective events arrive in one batch; the feed keeps 256.
        st::Goals goals(1); st::FakePresentationSink sink;
        eawr::platform::LiveStoryFeed feed;
        StoryTextState view;
        std::uint64_t cursor{};
        const auto goals_of = [&](st::Player owner) { return owner == 1 ? &goals.state() : nullptr; };
        for (int i = 0; i < 300; ++i) assert(goals.apply(st::ObjectiveReward{st::ObjectiveAction::add, {}, {"o" + std::to_string(i), false, 0, st::ObjectiveStatus::incomplete}, true}, sink));
        auto batch = std::move(sink.events);
        sink.events.clear();
        feed.publish(batch, goals_of);
        assert(batch.empty() && feed.through() == 300);
        auto got = feed.after(cursor, 1);
        assert(got.incomplete && got.events.size() == eawr::platform::LiveStoryFeed::retention && got.objectives);
        assert(got.objectives->objectives == goals.state().objectives && got.through == 300);
        view.resync(*got.objectives); // the viewer ignores got.events and takes the state
        cursor = got.through;
        assert(view.objectives() == goals.state().objectives && view.objectives().size() == 300);
        // After the resync, ordinary events apply again.
        assert(goals.apply(st::ObjectiveReward{st::ObjectiveAction::complete, "o7", {}, true}, sink));
        batch = std::move(sink.events);
        sink.events.clear();
        feed.publish(batch, goals_of);
        got = feed.after(cursor, 1);
        assert(!got.incomplete && !got.objectives && got.events.size() == 1);
        for (const auto& row : got.events) assert(view.apply(row.event));
        assert(view.objectives() == goals.state().objectives);
        // Another owner sees nothing of this owner's events; a behind reader of that owner gets an empty list.
        assert(feed.after(300, 2).events.empty() && feed.after(0, 2).incomplete && feed.after(0, 2).objectives->objectives.empty());
        assert(feed.after(0, std::nullopt).events.empty());
        // A session change drops the old mirror: a fresh view is empty.
        view = StoryTextState{};
        assert(view.objectives().empty() && view.lines().empty());
    }
    {
        // legacy #6681: goal rows are kept for a reader that fell behind too, so the story summary rebuilds its goal list
        // from the owner's published state instead of from lost events. 300 goals arrive in one batch; the feed keeps 256.
        st::Goals goals(1); st::FakePresentationSink sink;
        eawr::platform::LiveStoryFeed feed;
        const auto goals_of = [&](st::Player owner) { return owner == 1 ? &goals.state() : nullptr; };
        for (int i = 0; i < 300; ++i)
            assert(goals.dialog(st::Dialog{"dialog_" + std::to_string(i), "", "tag_" + std::to_string(i), i, false, false, std::nullopt},
                st::LocalPresentation{}, sink));
        auto batch = std::move(sink.events);
        sink.events.clear();
        feed.publish(batch, goals_of);
        const auto got = feed.after(0, 1);
        assert(got.incomplete && got.objectives && got.objectives->goals == goals.state().goals && got.objectives->goals.size() == 300);
        assert(got.objectives->goals.front().tag == "tag_299"); // newest first, as the session keeps them
        // An owner with only objective-free, goal-free events has no snapshot to hand out.
        assert(feed.after(0, 2).objectives && feed.after(0, 2).objectives->goals.empty());
    }
    {
        // legacy #6634: Letter_Box_In / Letter_Box_Out. The seconds are the slide; 0 is at once; the target and the slid fraction
        // are separate (the GUI follows the target, the bands the slide); a request that is not a letterbox one, or whose
        // seconds are not a finite non-negative number, is ignored.
        using st::Consumer;
        StoryHudState view;
        const auto letterbox = [&](const char* name, const char* seconds) {
            return view.apply(st::ConsumerRequest{Consumer::camera, name, {seconds}});
        };
        assert(!view.letterbox_on() && !view.letterbox_visible() && view.letterbox_progress() == 0.0F);
        assert(letterbox("LETTER_BOX_IN", "2") && view.letterbox_on() && view.letterbox_visible() && view.letterbox_progress() == 0.0F);
        view.advance(1.0);
        assert(view.letterbox_progress() > 0.49F && view.letterbox_progress() < 0.51F); // half way after half the slide
        view.advance(5.0);
        assert(view.letterbox_progress() == 1.0F && view.letterbox_on());
        assert(letterbox("LETTER_BOX_OUT", "2") && !view.letterbox_on() && view.letterbox_visible()); // still sliding out
        view.advance(1.0);
        assert(view.letterbox_progress() > 0.49F && view.letterbox_progress() < 0.51F);
        view.advance(1.5);
        assert(view.letterbox_progress() == 0.0F && !view.letterbox_visible());
        assert(letterbox("LETTER_BOX_IN", "0") && view.letterbox_progress() == 1.0F); // at once, before any frame
        assert(letterbox("LETTER_BOX_OUT", "0") && view.letterbox_progress() == 0.0F && !view.letterbox_visible());
        assert(letterbox("LETTER_BOX_IN", "0.5"));
        view.advance(0.25);
        assert(view.letterbox_progress() > 0.49F && view.letterbox_progress() < 0.51F);
        assert(!letterbox("LETTER_BOX_IN", "-1") && !letterbox("LETTER_BOX_IN", "soon") && !letterbox("LETTER_BOX_OUT", "")
            && !letterbox("LETTER_BOX_OUT", "inf") && !letterbox("LETTER_BOX_OUT", "nan") && !letterbox("LETTER_BOX_IN", "2x"));
        assert(view.letterbox_on()); // a refused request changed nothing
        assert(!view.apply(st::ConsumerRequest{Consumer::camera, "OTHER_CAMERA", {"1"}}) && !view.apply(st::ConsumerRequest{Consumer::camera, "LETTER_BOX_IN", {}}));
        view = StoryHudState{}; // a new or loaded session
        assert(!view.letterbox_on() && !view.letterbox_visible());
    }
    {
        // SCREEN_TEXT: defaults, until-removed, the removal form, colour and style, and the bound.
        using st::Consumer;
        StoryTextState view;
        const auto text = [&](std::vector<std::string> parameters) {
            return view.apply({1, st::ConsumerRequest{Consumer::text, "SCREEN_TEXT", std::move(parameters)}});
        };
        assert(text({"TEXT_A", "", "", "", "", "", ""}) && view.lines().size() == 1 && view.lines()[0].seconds_left == 5.0 && view.lines()[0].teletype);
        assert(text({"TEXT_B", "-1", "", "", "0", "1 0.5 0", "Hint"}) && view.lines()[1].seconds_left < 0.0 && !view.lines()[1].teletype
            && view.lines()[1].colour && (*view.lines()[1].colour)[1] == 0.5F && view.lines()[1].style == StoryTextState::Style::hint);
        assert(text({"TEXT_C", "2", "", "", "", "1 2", "task"}) && !view.lines()[2].colour && view.lines()[2].style == StoryTextState::Style::task);
        view.advance(2.5);
        assert(view.lines().size() == 2 && view.lines()[0].text_id == "TEXT_A" && view.lines()[1].seconds_left < 0.0); // C ended, B stays
        view.advance(10.0);
        assert(view.lines().size() == 1 && view.lines()[0].text_id == "TEXT_B");
        assert(text({"TEXT_B", "", "", "remove", "", "", ""}) && view.lines().empty()); // the removal form shows nothing
        assert(text({"", "3"}) && view.lines().empty()); // no text id: nothing
        for (int i = 0; i < 50; ++i) text({"TEXT_" + std::to_string(i), "-1"});
        assert(view.lines().size() == StoryTextState::max_lines);
        assert(!view.apply({1, st::ConsumerRequest{Consumer::gui, "FLASH_GUI", {"b"}}}) && !view.apply({1, st::SoundRequest{"x"}}));
    }
    {
        // #6291 (GCT-07): the player's cinematic tour asks for the bands over half a second and for the GUI to be hidden;
        // it ends them again unless a story script still wants them.
        StoryHudState view;
        assert(!view.letterbox_on() && !view.tour_on());
        view.set_tour(true);
        assert(view.letterbox_on() && view.letterbox_visible() && view.tour_on() && view.letterbox_progress() == 0.0F);
        view.advance(0.25);
        assert(view.letterbox_progress() > 0.45F && view.letterbox_progress() < 0.55F);
        view.advance(0.25);
        assert(view.letterbox_progress() == 1.0F);
        view.set_tour(false);
        assert(!view.letterbox_on() && view.letterbox_visible()); // the bands are still sliding out
        view.advance(0.5);
        assert(!view.letterbox_visible());
        view.set_tour(true);
        view.apply(st::ConsumerRequest{st::Consumer::camera, "LETTER_BOX_IN", {"0"}});
        view.set_tour(false);
        assert(view.letterbox_on()); // a story script's request outlives the tour
    }
    return 0;
}

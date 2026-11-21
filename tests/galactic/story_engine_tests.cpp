#include <algorithm>
#include <array>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/setup_plan.hpp"
#include "eawr/campaign/story_plots.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/units/unit_tables_galactic.hpp"
#include "eawr/vfs/vfs.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

// #683 part 1: the live story engine (WST-01..08, 10..28, 30, 33, 60..62) over synthetic plots, the plot
// loader over loose files, and the engine inside a GameSession (hash, checkpoint, worker equality).
namespace {
namespace st = eawr::sim::galactic::story;
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using eawr::sim::math::Fixed;
using eawr::testing::take;
using galactic_test::expect;

Fixed sec(std::int64_t whole, std::int64_t twentieths = 0) {
    return Fixed::from_raw(whole * Fixed::scale + twentieths * (Fixed::scale / 20));
}
st::EventDef make(std::string name, std::string type, std::vector<std::string> p1 = {},
    std::vector<std::vector<std::string>> prereqs = {}) {
    st::EventDef e;
    e.name = std::move(name); e.type = std::move(type);
    e.params[0] = std::move(p1); e.prereqs = std::move(prereqs);
    return e;
}
st::EventDef trig(std::string name, std::vector<std::vector<std::string>> prereqs, std::string reward = {},
    std::vector<std::string> r1 = {}, std::vector<std::string> r2 = {}) {
    auto e = make(std::move(name), "STORY_TRIGGER", {}, std::move(prereqs));
    e.reward_type = std::move(reward); e.reward_params[0] = std::move(r1); e.reward_params[1] = std::move(r2);
    return e;
}
st::SubPlotDef sub(std::string name, st::Player owner, std::vector<st::EventDef> events, bool active = true) {
    st::SubPlotDef s;
    s.name = std::move(name); s.owner = owner; s.active = active; s.events = std::move(events);
    return s;
}
st::Engine engine(std::vector<st::SubPlotDef> subs, st::Config config = {}) {
    std::vector<st::PlotRecord> plots;
    for (const auto& s : subs) if (std::none_of(plots.begin(), plots.end(), [&](const auto& p) { return p.owner == s.owner; }))
        plots.push_back({s.owner, "plot.xml"});
    return take(st::Engine::create(config, std::move(plots), std::move(subs)));
}
std::vector<std::string> names(const std::vector<st::Triggered>& rows) {
    std::vector<std::string> out;
    for (const auto& row : rows) out.push_back(row.event);
    return out;
}
using Names = std::vector<std::string>;
st::EconomyNotice tech(std::int64_t level) {
    st::EconomyNotice n; n.kind = st::Economy::tech_level; n.value = level; return n;
}

void order_and_prerequisites() {
    // WST-06/20/21/22: AND inside a group, OR between groups, depth-first activation in document order
    // (the interim WST-U01 rule, #4344 C1).
    expect(st::interim_event_order == st::EventOrder::document, "WST-U01 interim rule is document order");
    auto e = engine({sub("S.xml", 1, {
        make("START", "STORY_ELAPSED", {"0"}),
        trig("A", {{"START"}}, "SPAWN_HERO", {"X"}),
        trig("B", {{"START"}}, "SPAWN_HERO", {"Y"}),
        trig("C", {{"A", "B"}}, "CREDITS", {"5"}),
        trig("D", {{"NEVER"}, {"A"}}, "CREDITS", {"6"}),
        make("NEVER", "STORY_TECH_LEVEL", {"9"}),
        trig("E", {{"NEVER"}}, "CREDITS", {"7"}),
        trig("F", {}, "CREDITS", {"8"}),
    })});
    expect(e.state().subplots[0].events[7].active && !e.state().subplots[0].events[1].active, "WST-06 no prerequisite group starts active, any group starts inactive");
    take(e.service(sec(0), false));
    expect(names(e.take_triggered()) == Names{"A", "D", "B", "C"}, "WST-20/21 depth-first activation: A, its dependant D, then B and the AND event C");
    expect(e.triggered("S.xml", "START") && !e.triggered("S.xml", "E") && !e.triggered("S.xml", "F"), "events without a satisfied group or a game event wait");
    take(e.service(sec(1), false));
    expect(e.take_triggered().empty(), "WST-14 a triggered event is not evaluated again");
    expect(e.state().sequence == 4, "records are numbered in trigger order");
}

void elapsed_time() {
    auto e = engine({sub("S.xml", 1, {make("T5", "STORY_ELAPSED", {"5"}), trig("AFTER", {{"T5"}}, "CREDITS", {"1"})})});
    take(e.service(sec(0), false)); take(e.service(sec(4), false));
    expect(!e.triggered("S.xml", "T5"), "WST-30 elapsed fires only once the accumulated time reaches the parameter");
    take(e.service(sec(10), true));
    expect(!e.triggered("S.xml", "T5"), "WST-12 paused time does not accumulate");
    take(e.service(sec(10, 19), false));
    expect(!e.triggered("S.xml", "T5"), "just short of the threshold");
    take(e.service(sec(11), false));
    expect(e.triggered("S.xml", "T5") && names(e.take_triggered()) == Names{"AFTER"}, "WST-12 resumes after the pause and fires at 5 unpaused seconds");
    auto back = engine({sub("S.xml", 1, {make("T2", "STORY_ELAPSED", {"2"})})});
    take(back.service(sec(5), false)); // 5 s from a clock that started at zero already counts
    expect(back.triggered("S.xml", "T2"), "elapsed counts from activation, which here is the story start");
    auto late = engine({sub("S.xml", 1, {make("START", "STORY_ELAPSED", {"0"}), make("LATER", "STORY_ELAPSED", {"3"}, {{"START"}})})});
    take(late.service(sec(10), false));
    expect(!late.triggered("S.xml", "LATER"), "WST-21 an activated elapsed event starts its clock at activation");
    take(late.service(sec(13), false));
    expect(late.triggered("S.xml", "LATER"), "and fires three seconds later");
    auto rewind = engine({sub("S.xml", 1, {make("T4", "STORY_ELAPSED", {"4"})})});
    take(rewind.service(sec(2), false)); take(rewind.service(sec(1), false)); take(rewind.service(sec(2), false));
    expect(!rewind.triggered("S.xml", "T4"), "WST-12 backwards time resynchronises without adding");
    take(rewind.service(sec(3), false));
    expect(rewind.triggered("S.xml", "T4"), "and counts again from the new time");
}

void flags() {
    auto flagged = make("WATCH", "STORY_FLAG", {"MyFlag", "other"});
    flagged.params[1] = {"2"}; flagged.params[2] = {"GREATER_THAN_EQUAL_TO"}; flagged.reward_type = "CREDITS"; flagged.reward_params[0] = {"1"};
    auto equal = make("EQ", "STORY_FLAG", {"myflag"}); equal.params[1] = {"3"}; equal.params[2] = {"EQUAL_TO"};
    auto not_equal = make("NE", "STORY_FLAG", {"myflag"}); not_equal.params[1] = {"1"}; not_equal.params[2] = {"NOT_EQUAL_TO"};
    auto less = make("LT", "STORY_FLAG", {"myflag"}); less.params[1] = {"4"}; less.params[2] = {"LESS_THAN"};
    auto absent = make("MISSING", "STORY_FLAG", {"nosuch"}); absent.params[1] = {"0"};
    auto bare = make("GT", "STORY_FLAG", {"myflag"}); bare.params[1] = {"2"}; // no comparison: greater-than
    auto e = engine({sub("S.xml", 1, {
        make("START", "STORY_ELAPSED", {"0"}),
        trig("SET", {{"START"}}, "SET_FLAG", {"myFLAG"}, {"3"}),
        flagged, equal, not_equal, less, absent, bare,
        trig("BAD", {{"START"}}, "INCREMENT_FLAG", {"undefined"}, {"1"}),
        trig("INC", {{"SET"}}, "INCREMENT_FLAG", {"myflag"}, {"2"}),
    })});
    take(e.service(sec(0), false));
    expect(e.flag("MYFLAG") == 5 && e.flag("myflag") == 5, "WST-27 SET_FLAG then INCREMENT_FLAG on the upper-cased name");
    expect(!e.flag("undefined") && !e.diagnostics().empty(), "WST-27 incrementing an undefined flag is an error and creates nothing");
    expect(e.triggered("S.xml", "WATCH") && e.triggered("S.xml", "GT"), "flag events are polled in the same service (WST-15), unknown comparison reads greater-than");
    expect(!e.triggered("S.xml", "EQ") && !e.triggered("S.xml", "LT"), "the flag is 5 by the poll: equal-to 3 and less-than 4 are false");
    expect(!e.triggered("S.xml", "NE") && !e.triggered("S.xml", "MISSING"), "WST-27 NOT_EQUAL_TO never fires and undefined flags are skipped");
    const auto records = e.take_triggered();
    expect(!records.empty() && records[0].event == "SET" && records[0].engine_applied && records[0].reward_type == "SET_FLAG",
        "engine-run rewards are recorded as applied");
    bool credits{};
    for (const auto& row : records) if (row.event == "WATCH") credits = row.reward_type == "CREDITS" && !row.engine_applied && row.reward_params[0] == std::vector<std::string>{"1"};
    expect(credits, "other rewards are left to the owning service with their parameters");
}

void tech_levels() {
    auto up = engine({
        sub("P1.xml", 1, {make("T2", "STORY_TECH_LEVEL", {"2"}), trig("G1", {{"T2"}}, "SPAWN_HERO", {"H1"}),
            make("NEXT", "STORY_TECH_LEVEL", {"-1"}), trig("GN", {{"NEXT"}}, "SPAWN_HERO", {"HN"})}),
        sub("P2.xml", 2, {make("T2", "STORY_TECH_LEVEL", {"2"}), trig("G2", {{"T2"}}, "SPAWN_HERO", {"H2"})}),
    });
    take(up.notify(1, tech(1)));
    expect(up.take_triggered().empty(), "WST-33 below the level nothing fires; -1 only records its first value");
    take(up.notify(1, tech(1)));
    expect(up.take_triggered().empty() && !up.triggered("P1.xml", "NEXT"), "a same-level notice is not a change");
    take(up.notify(1, tech(3)));
    expect(names(up.take_triggered()) == Names{"G1", "GN"}, "WST-33 level >= parameter, and -1 fires on the first change after the first value");
    expect(!up.triggered("P2.xml", "T2"), "WST-14 a dispatch naming a player reaches only that player's sub-plots");
    take(up.notify(2, tech(2)));
    expect(names(up.take_triggered()) == Names{"G2"}, "the other player's plot answers its own tech");
    take(up.notify(std::nullopt, tech(5)));
    expect(up.take_triggered().empty(), "triggered events stay triggered");
}

void perpetual_and_branches() {
    auto src = make("SRC", "STORY_TECH_LEVEL", {"1"}); src.perpetual = true;
    auto perpetual = trig("EVERY", {{"SRC"}}, "CREDITS", {"1"}); perpetual.perpetual = true;
    auto e = engine({sub("S.xml", 1, {src, perpetual, trig("ONCE", {{"SRC"}}, "CREDITS", {"2"})})});
    take(e.notify(1, tech(1))); take(e.notify(1, tech(2)));
    expect(names(e.take_triggered()) == Names{"EVERY", "ONCE", "EVERY"}, "WST-22 a perpetual trigger fires once per activation, a plain one only once");

    auto first = trig("B1", {{"S0"}}, "CREDITS", {"1"}); first.branch = "BR";
    auto second = trig("B2", {{"B1"}}, "CREDITS", {"2"}); second.branch = "br";
    auto reset = make("RESET", "STORY_TECH_LEVEL", {"1"}); reset.reward_type = "RESET_BRANCH"; reset.reward_params[0] = {"Br"};
    auto branches = engine({sub("S.xml", 1, {make("S0", "STORY_ELAPSED", {"0"}), first, second, reset})});
    take(branches.service(sec(0), false));
    expect(names(branches.take_triggered()) == Names{"B1", "B2"}, "branch events run once");
    take(branches.notify(1, tech(1)));
    expect(names(branches.take_triggered()) == Names{"RESET", "B1", "B2"}, "WST-24 RESET_BRANCH clears the branch and re-checks its prerequisites");

    auto disable = make("OFF", "STORY_TECH_LEVEL", {"1"}); disable.reward_type = "DISABLE_BRANCH"; disable.reward_params[0] = {"BR"}; disable.reward_params[1] = {"1"};
    auto hit = trig("HIT", {{"S0"}}, "CREDITS", {"1"}); hit.branch = "BR";
    auto lone = trig("LONE", {{"S0"}}, "CREDITS", {"1"});
    auto off_one = make("OFFONE", "STORY_TECH_LEVEL", {"1"}); off_one.reward_type = "DISABLE_STORY_EVENT"; off_one.reward_params[0] = {"lone"}; off_one.reward_params[1] = {"1"};
    auto cut = engine({sub("S.xml", 1, {make("S0", "STORY_ELAPSED", {"0"}), hit, lone, disable, off_one})});
    take(cut.notify(1, tech(1))); take(cut.service(sec(0), false));
    expect(names(cut.take_triggered()) == Names{"OFF", "OFFONE"}, "WST-24/25 disabled branch and event never trigger");

    // WST-24: nesting of resets is limited to three levels.
    std::vector<st::EventDef> chain;
    for (int i = 1; i <= 5; ++i) {
        auto link = make("E" + std::to_string(i), "STORY_TECH_LEVEL", {i == 1 ? "1" : "99"});
        link.reward_type = "RESET_BRANCH"; link.reward_params[0] = {"NOSUCH" + std::to_string(i)};
        if (i < 5) link.reward_params[1] = {"E" + std::to_string(i + 1)};
        chain.push_back(std::move(link));
    }
    auto nested = engine({sub("S.xml", 1, std::move(chain))});
    take(nested.notify(1, tech(1)));
    expect(names(nested.take_triggered()) == Names{"E1", "E2", "E3", "E4"} && !nested.triggered("S.xml", "E5"),
        "WST-24 the fourth nested reset is refused");
    expect(std::any_of(nested.diagnostics().begin(), nested.diagnostics().end(), [](const auto& d) { return d.reason.find("nesting") != std::string::npos; }),
        "the refusal is reported");
}

void sub_plots_and_elements() {
    auto open = trig("OPEN", {{"S0"}}, "STORY_ELEMENT", {"Act2"});
    auto act1 = sub("Act1.xml", 1, {make("S0", "STORY_ELAPSED", {"0"}), open});
    auto act2 = sub("Act2.xml", 1, {make("LATE", "STORY_ELAPSED", {"0"}), trig("GO", {{"LATE"}}, "CREDITS", {"9"})}, false);
    auto e = engine({act1, act2});
    take(e.service(sec(0), false));
    expect(names(e.take_triggered()) == Names{"OPEN", "GO"}, "WST-26 STORY_ELEMENT activates the named .xml sub-plot, which is evaluated in the same pass");
    auto closed = engine({sub("Act1.xml", 1, {make("S0", "STORY_ELAPSED", {"0"})}), act2});
    take(closed.service(sec(5), false));
    expect(closed.take_triggered().empty() && !closed.triggered("Act2.xml", "LATE"), "WST-16 a suspended sub-plot's events receive nothing");
}

st::TextNotice click(std::string name) { return {st::Text::click_gui, std::move(name), std::nullopt}; }

void click_gui() {
    // WST-37: a clicked component fires the events whose Event_Param1 list names it, case-insensitively, once.
    auto e = engine({
        sub("P1.xml", 1, {make("C1", "STORY_CLICK_GUI", {"B_Filter_Empire", "other"}), trig("G1", {{"C1"}}, "CREDITS", {"1"}),
            make("C2", "STORY_CLICK_GUI", {"b_filter_unit"}), make("GEN", "STORY_GENERIC", {"b_filter_empire"})}),
        sub("P2.xml", 2, {make("C1", "STORY_CLICK_GUI", {"b_filter_empire"})}),
    });
    for (const auto& row : e.unavailable()) expect(row.what != "STORY_CLICK_GUI", "WST-37 STORY_CLICK_GUI has an evaluator");
    take(e.notify(1, click("B_FILTER_EMPIRE")));
    expect(names(e.take_triggered()) == Names{"G1"} && e.triggered("P1.xml", "C1"), "the named component fires its event and dependants");
    expect(!e.triggered("P1.xml", "C2") && !e.triggered("P1.xml", "GEN") && !e.triggered("P2.xml", "C1"),
        "other components, other text events and other players' plots stay quiet (WST-14)");
    take(e.notify(1, click("b_filter_empire")));
    expect(e.take_triggered().empty(), "a spent one-shot event does not fire again");
    take(e.notify(2, click("b_filter_empire")));
    expect(e.triggered("P2.xml", "C1"), "the clicking player's own plot answers");
    take(e.notify(1, click("b_filter_unit")));
    expect(e.triggered("P1.xml", "C2"), "a second component in the same plot");
}

void open_corruption() {
    auto e = engine({
        sub("P1.xml", 1, {make("OPEN", "STORY_OPEN_CORRUPTION", {"PLANET_A", "PLANET_B"}),
            trig("GO", {{"OPEN"}}, "CREDITS", {"1"}), make("OTHER", "STORY_OPEN_CORRUPTION", {"PLANET_C"})}),
        sub("P2.xml", 2, {make("OPEN", "STORY_OPEN_CORRUPTION", {"PLANET_A"})}),
        sub("OFF.xml", 1, {make("OPEN", "STORY_OPEN_CORRUPTION", {"PLANET_A"})}, false)});
    take(e.notify(1, st::TextNotice{st::Text::open_corruption, "planet_a", std::nullopt}));
    expect(names(e.take_triggered()) == Names{"GO"} && e.triggered("P1.xml", "OPEN"), "dialog opening matches a planet type and triggers dependants");
    expect(!e.triggered("P1.xml", "OTHER") && !e.triggered("P2.xml", "OPEN") && !e.triggered("OFF.xml", "OPEN"),
        "dialog opening respects the planet list, owner and active subplot");
    take(e.notify(1, st::TextNotice{st::Text::open_corruption, "PLANET_A", std::nullopt}));
    expect(e.take_triggered().empty(), "one-shot open events stay spent");
    take(e.notify(2, st::TextNotice{st::Text::open_corruption, "PLANET_A", std::nullopt}));
    expect(e.triggered("P2.xml", "OPEN"), "each player's own opening reaches its plot");
}

void click_listeners() {
    // #5857: the HUD raises a click only for a component some STORY_CLICK_GUI event names. The list is definition-wide
    // (a suspended plot or a spent event still names it), upper-cased and sorted, and an Event_Param edit moves its revision.
    auto e = engine({
        sub("P1.xml", 1, {make("C1", "STORY_CLICK_GUI", {"b_zed", "Other_Name"}), make("GEN", "STORY_GENERIC", {"not_a_click"})}),
        sub("P2.xml", 2, {make("C2", "STORY_CLICK_GUI", {"b_alpha"}), make("C3", "STORY_CLICK_GUI", {"B_ZED"})}, false),
    });
    expect(e.click_listeners() == (Names{"B_ALPHA", "B_ZED", "OTHER_NAME"}), "listeners: every named component once, upper-case, sorted");
    const auto revision = e.click_listener_revision();
    take(e.notify(1, click("b_zed")));
    expect(e.click_listener_revision() == revision && e.click_listeners().size() == 3, "a click changes neither the list nor its revision");
    st::PlotCommand edit; edit.op = st::PlotCommand::Op::event_param; edit.subplot = "P1.xml"; edit.event = "c1"; edit.index = 0; edit.values = {"b_new"};
    take(e.apply(edit));
    expect(e.click_listener_revision() != revision && e.click_listeners() == (Names{"B_ALPHA", "B_NEW", "B_ZED"}), "an Event_Param1 edit moves the revision and the list");
    auto none = engine({sub("P.xml", 1, {make("GEN", "STORY_GENERIC", {"x"})})});
    expect(none.click_listeners().empty(), "a story without click events listens to nothing");
}

void click_command_codec() {
    // WST-37: the click command's bytes are a length byte and the printable-ASCII name; anything else is malformed.
    const gc::CommandPayload command = gc::GuiClick{"b_Filter_Empire"};
    expect(gc::layer_of(command) == gc::CommandLayer::session && gc::opcode_of(command) == gc::session_opcodes::gui_click,
        "GuiClick is a session command");
    const auto bytes = gc::encode_payload(command);
    expect(bytes.size() == 16 && bytes[0] == 15, "one length byte and the name");
    expect(gc::decode_payload(gc::CommandLayer::session, gc::session_opcodes::gui_click, bytes) == std::optional(command), "round trip");
    const auto encode = [](std::string name) { return gc::encode_payload(gc::GuiClick{std::move(name)}); };
    expect(encode("").empty() && encode(std::string(gc::max_gui_component_name + 1, 'a')).empty(), "encode rejects empty and oversized names");
    expect(encode("a b").empty() && encode(std::string(1, '\x7f')).empty() && encode(std::string(1, '\x80')).empty(),
        "encode rejects spaces and non-printable names");
    const auto decode = [](std::vector<std::uint8_t> raw) { return gc::decode_payload(gc::CommandLayer::session, gc::session_opcodes::gui_click, raw); };
    expect(!decode({}) && !decode({0}), "empty names are malformed");
    expect(!decode({3, 'a', 'b'}) && !decode({1, 'a', 'b'}), "a length that differs from the body is malformed");
    expect(!decode({3, 'a', ' ', 'b'}) && !decode({2, 'a', 0x7F}) && !decode({2, 'a', 0x80}), "spaces and non-ASCII bytes are malformed");
    std::vector<std::uint8_t> long_name{static_cast<std::uint8_t>(gc::max_gui_component_name + 1)};
    long_name.insert(long_name.end(), gc::max_gui_component_name + 1, 'a');
    expect(!decode(long_name), "a name longer than the limit is malformed");
}

// WST-56/26 (#5495): Lua Activate / Suspend / Reset and the event edits, as plain ordered commands.
void lua_plot_commands() {
    using Op = st::PlotCommand::Op;
    const auto command = [](Op op, std::string event = {}, std::uint32_t index = 0, std::vector<std::string> values = {}) {
        st::PlotCommand c; c.op = op; c.subplot = "Intervention.xml"; c.event = std::move(event); c.index = index; c.values = std::move(values);
        return c;
    };
    const auto build = [] {
        return engine({sub("Intervention.xml", 1, {make("START", "STORY_ELAPSED", {"0"}), trig("PAY", {{"START"}}, "CREDITS", {"1"})}, false)});
    };
    auto e = build();
    const auto pristine = e.digest();
    take(e.service(sec(5), false));
    expect(e.take_triggered().empty(), "a suspended intervention plot does nothing");
    take(e.apply(command(Op::event_param, "start", 0, {"2"})));
    take(e.apply(command(Op::reward_param, "pay", 0, {"750"})));
    take(e.apply(command(Op::add_dialog_text, "pay", 0, {"text:TEXT_X", "int:3"})));
    expect(e.digest() != pristine, "an edited plot changes the digest");
    take(e.apply(command(Op::activate)));
    expect(e.has_subplot("Intervention.xml") && e.has_event("Intervention.xml", "Pay") && !e.has_event("Intervention.xml", "NOPE"), "lookups");
    take(e.service(sec(6), false)); take(e.service(sec(8), false));
    auto rows = e.take_triggered();
    expect(names(rows) == Names{"PAY"} && rows[0].reward_params[0] == std::vector<std::string>{"750"}
        && rows[0].dialog_text == std::vector<std::string>{"text:TEXT_X", "int:3"}, "the edited reward and dialog text are used");
    // Checkpoint: the state with edits restores into a fresh engine to the same digest and behaviour.
    const auto saved = e.state();
    auto again = build();
    take(again.restore(saved));
    expect(again.digest() == e.digest() && again.state().edits == saved.edits, "edits round-trip through the saved state");
    take(again.apply(command(Op::reset)));
    take(again.apply(command(Op::suspend)));
    expect(!again.active("Intervention.xml", "PAY") && again.state().subplots[0].active == false, "suspend");
    // Restoring an unedited state removes the edits from a used engine.
    auto clean = build();
    take(e.restore(clean.state()));
    expect(e.digest() == pristine && e.state().edits.empty(), "restoring an unedited state reverts the edits; the digest equals the old bytes");
    take(e.apply(command(Op::activate)));
    take(e.service(sec(0), false)); take(e.service(sec(2), false));
    auto reverted = e.take_triggered();
    expect(names(reverted) == Names{"PAY"} && reverted[0].reward_type == "CREDITS" && reverted[0].reward_params[0] == std::vector<std::string>{"1"}
        && reverted[0].dialog_text.empty(), "a reverted event fires its original reward (CREDITS 1) after the restore");
    // A restore that fails half way leaves the definitions and the state untouched (transactional).
    auto guarded = build();
    take(guarded.apply(command(Op::reward_param, "pay", 0, {"750"})));
    const auto edited_digest = guarded.digest();
    auto broken = build().state();
    st::EventEdit ghost; ghost.subplot = "Intervention.xml"; ghost.event = "GHOST";
    broken.edits.push_back(ghost);
    expect(!guarded.restore(broken), "a saved edit naming an event that is not loaded is refused");
    expect(guarded.digest() == edited_digest && guarded.state().edits.size() == 1, "a refused restore leaves the state as it was");
    take(guarded.apply(command(Op::activate)));
    take(guarded.service(sec(0), false)); take(guarded.service(sec(2), false));
    auto kept = guarded.take_triggered();
    expect(names(kept) == Names{"PAY"} && kept[0].reward_params[0] == std::vector<std::string>{"750"}, "a refused restore keeps the earlier edits live");
    expect(!e.apply(command(Op::event_param, "start", 7, {"1"})) && !e.apply(command(Op::reward_param, "nope", 0, {"1"})),
        "an index outside the slots and an unknown event are refused");
    st::PlotCommand unknown = command(Op::reset); unknown.subplot = "Missing.xml";
    expect(!e.apply(unknown), "an unknown sub-plot is refused");
}

void land_battle_seam() {
    // Tactical and land events and rewards are parsed but refused until land battles exist.
    expect(st::land_battle_refusal == "not available until land battles", "the refusal text");
    expect(st::known_event_type("STORY_LAND_TACTICAL") && !st::known_event_type("STORY_POLITICAL_CONTROL")
        && !st::known_event_type("STORY_BASE_DESTROYED") && !st::known_event_type("Story_Trigger"), "WST-04/08 name table is exact and omits the dropped names");
    auto link = make("LINK", "STORY_TECH_LEVEL", {"1"}); link.reward_type = "LINK_TACTICAL"; link.reward_params[0] = {"Naboo"};
    auto e = engine({sub("S.xml", 1, {make("LAND", "STORY_LAND_TACTICAL", {"x.xml"}), make("ENTER", "STORY_SELECT_PLANET", {"Naboo"}), link,
        make("MYSTERY", "STORY_TRIGGER")})});
    int refusals{}, pending{};
    for (const auto& row : e.unavailable()) {
        if (row.reason == st::land_battle_refusal) ++refusals; else if (row.reason.find("no evaluator") != std::string::npos) ++pending;
    }
    expect(refusals == 2 && pending == 1, "the land event and the LINK_TACTICAL reward are refused, STORY_SELECT_PLANET waits for its evaluator");
    take(e.notify(1, tech(1)));
    const auto records = e.take_triggered();
    expect(records.size() == 1 && records[0].reward_type == "LINK_TACTICAL" && records[0].refusal == st::land_battle_refusal,
        "a triggered LINK_TACTICAL carries the refusal and must not run");
    st::TacticalNotice tactical; tactical.kind = st::Tactical::land_started;
    take(e.notify(1, tactical));
    expect(!e.triggered("S.xml", "LAND"), "land events never fire");
}

void cycles_and_tactical_triggers() {
    // A TRIGGER_EVENT cycle in mod data stops at the recursion limit with a named refusal, never at a stack overflow.
    auto self = make("SELF", "STORY_TECH_LEVEL", {"1"}); self.reward_type = "TRIGGER_EVENT"; self.reward_params[0] = {"self"};
    auto ping = make("PING", "STORY_TECH_LEVEL", {"1"}); ping.reward_type = "TRIGGER_EVENT"; ping.reward_params[0] = {"pong"};
    auto pong = make("PONG", "STORY_TECH_LEVEL", {"99"}); pong.reward_type = "TRIGGER_EVENT"; pong.reward_params[0] = {"ping"};
    auto loop = engine({sub("S.xml", 1, {self, ping, pong})});
    take(loop.notify(1, tech(1)));
    const auto records = loop.take_triggered();
    expect(!records.empty() && records.size() < 600, "the cycle's records are bounded");
    expect(std::count_if(loop.diagnostics().begin(), loop.diagnostics().end(), [](const auto& d) { return d.reason.find("recursion limit") != std::string::npos; }) >= 2,
        "each cycle stops at the recursion limit with a named refusal");
    // TRIGGER_EVENT and branch resets cannot reach a tactical event either (WST-35 events never fire here).
    auto go = make("GO", "STORY_TECH_LEVEL", {"1"}); go.reward_type = "TRIGGER_EVENT"; go.reward_params[0] = {"land"};
    auto land = make("LAND", "STORY_LAND_TACTICAL", {"x.xml"}); land.reward_type = "CREDITS"; land.reward_params[0] = {"5"};
    land.branch = "BR";
    auto reset = make("RESET", "STORY_TECH_LEVEL", {"1"}); reset.reward_type = "RESET_BRANCH"; reset.reward_params[0] = {"BR"}; reset.reward_params[1] = {"land"};
    auto tactical = engine({sub("S.xml", 1, {go, land, reset})});
    take(tactical.notify(1, tech(1)));
    expect(names(tactical.take_triggered()) == Names{"GO", "RESET"} && !tactical.triggered("S.xml", "LAND"),
        "a tactical event named by TRIGGER_EVENT or RESET_BRANCH never fires and queues no reward");
    expect(std::any_of(tactical.diagnostics().begin(), tactical.diagnostics().end(), [](const auto& d) { return d.reason == st::land_battle_refusal; }),
        "and is reported with the land-battle refusal");
}

void digest_covers_clocks() {
    const auto plots = [] { return std::vector<st::SubPlotDef>{sub("S.xml", 1, {make("T9", "STORY_ELAPSED", {"9"})})}; };
    auto a = engine(plots()), b = engine(plots()), c = engine(plots());
    take(a.service(sec(2), false)); take(b.service(sec(2), false)); take(c.service(sec(3), false));
    expect(a.digest() == b.digest(), "equal clocks, equal digests");
    expect(a.digest() != c.digest(), "diverging event clocks change the digest (desync detection)");
}

void multiplayer_gate() {
    auto mp = trig("MP", {}, "CREDITS", {"1"}); mp.multiplayer = true;
    mp.type = "STORY_ELAPSED"; mp.params[0] = {"0"};
    auto plain = make("PLAIN", "STORY_ELAPSED", {"0"});
    auto solo = engine({sub("S.xml", 1, {mp, plain})});
    take(solo.service(sec(0), false));
    expect(solo.triggered("S.xml", "MP") && solo.triggered("S.xml", "PLAIN"), "WST-14 solo games evaluate every event");
    st::Config online; online.solo = false;
    auto lan = engine({sub("S.xml", 1, {mp, plain})}, online);
    take(lan.service(sec(0), false));
    expect(lan.triggered("S.xml", "MP") && !lan.triggered("S.xml", "PLAIN"), "outside solo and skirmish only Multiplayer events are evaluated");
}

// #4588 probe (test only): the same level raise in a solo and in a multiplayer-flavour setup. The
// available-hero set differs exactly by the hero-unlock events that lack the Multiplayer flag
// (WST-14); an event authored with the flag fires identically in both flavours.
std::vector<std::string> hero_unlocks_after_raise(bool solo, std::int64_t level) {
    const auto flagged = [](st::EventDef e) { e.multiplayer = true; return e; };
    st::Config config; config.solo = solo;
    auto e = engine({sub("P.xml", 1, {
        flagged(make("MP_T2", "STORY_TECH_LEVEL", {"2"})), flagged(trig("MP_G2", {{"MP_T2"}}, "SPAWN_HERO", {"HERO_FLAGGED"})),
        make("DATA_T3", "STORY_TECH_LEVEL", {"3"}), trig("DATA_G3", {{"DATA_T3"}}, "SPAWN_HERO", {"HERO_UNFLAGGED"}),
        flagged(make("HALF_T2", "STORY_TECH_LEVEL", {"2"})), trig("HALF_G2", {{"HALF_T2"}}, "SPAWN_HERO", {"HERO_HALF"})})}, config);
    take(e.notify(1, tech(1)));
    take(e.notify(1, tech(level)));
    return names(e.take_triggered());
}
void hero_unlock_flavour_probe() {
    const Names sp3{"MP_G2", "DATA_G3", "HALF_G2"};
    const auto solo = hero_unlocks_after_raise(true, 3), online = hero_unlocks_after_raise(false, 3);
    expect(solo.size() == 3 && std::is_permutation(solo.begin(), solo.end(), sp3.begin()), "solo flavour fires every hero unlock at level 3");
    // A trigger reached by activation fires without its own eligibility test (activate -> trigger), so only the
    // tech event's own Multiplayer flag decides whether the hero unlock is reachable.
    expect(online.size() == 2 && std::is_permutation(online.begin(), online.end(), Names{"MP_G2", "HALF_G2"}.begin()),
        "multiplayer flavour fires every hero unlock whose STORY_TECH_LEVEL event carries the Multiplayer flag");
    Names missing;
    for (const auto& n : solo) if (std::find(online.begin(), online.end(), n) == online.end()) missing.push_back(n);
    std::sort(missing.begin(), missing.end());
    expect(missing == Names{"DATA_G3"}, "the only missing hero comes from a tech event without a Multiplayer flag: a data property, not an engine skip");
    const auto sp2 = hero_unlocks_after_raise(true, 2), mp2 = hero_unlocks_after_raise(false, 2);
    expect(mp2.size() == 2 && sp2.size() == 2, "a raise to level 2 behaves the same way");
}

void construction_refusals() {
    auto bad = st::Engine::create({}, {}, {sub("S.xml", 1, {make("A", "STORY_TRIGGER"), make("A", "STORY_TRIGGER")})});
    expect(!bad, "duplicate event names are refused (the loader replaces them first)");
    auto lower = st::Engine::create({}, {}, {sub("S.xml", 1, {make("lower", "STORY_TRIGGER")})});
    expect(!lower, "event names must be upper-case");
    auto skipped = engine({sub("S.xml", 1, {make("A", "STORY_ELAPSED", {"0"}), trig("B", {{"A", "GHOST"}}, "CREDITS", {"1"})})});
    expect(!skipped.diagnostics().empty(), "WST-06 an unknown prerequisite name is reported and skipped");
    take(skipped.service(sec(0), false));
    expect(skipped.triggered("S.xml", "B"), "the remaining prerequisites still decide");
}

std::vector<st::SubPlotDef> save_plots() {
    auto src = make("SRC", "STORY_TECH_LEVEL", {"1"}); src.perpetual = true;
    return {sub("S.xml", 1, {make("START", "STORY_ELAPSED", {"0"}), make("LATE", "STORY_ELAPSED", {"4"}),
        trig("SET", {{"START"}}, "SET_FLAG", {"seen"}, {"7"}), src, trig("EVERY", {{"SRC"}}, "CREDITS", {"1"}),
        trig("AFTER", {{"LATE"}}, "CREDITS", {"2"})})};
}
void save_and_restore() {
    // WST-60..62: the triggered state and the flag table round trip; definitions come from data.
    auto a = engine(save_plots());
    take(a.service(sec(0), false)); take(a.service(sec(2), false)); take(a.notify(1, tech(1)));
    static_cast<void>(a.take_triggered());
    auto b = engine(save_plots());
    take(b.restore(a.state()));
    expect(a.digest() == b.digest() && a.digest().size() == 64, "the restored engine has the same state digest");
    for (const std::int64_t whole : {3, 4, 5}) {
        take(a.service(sec(whole), false)); take(b.service(sec(whole), false));
        take(a.notify(1, tech(2))); take(b.notify(1, tech(2)));
    }
    expect(names(a.take_triggered()) == names(b.take_triggered()) && a.digest() == b.digest(), "both continue identically");
    expect(b.triggered("S.xml", "AFTER") && b.flag("seen") == 7, "triggered events and flags survive");
    auto other = engine({sub("S.xml", 1, {make("START", "STORY_ELAPSED", {"0"})})});
    expect(!other.restore(a.state()), "a save of other plots is refused");
    auto renamed = a.state(); renamed.subplots[0].events[0].name = "OTHER";
    auto c = engine(save_plots());
    expect(!c.restore(renamed), "a renamed event is refused by name (WST-61)");
}

// ---- the plot loader over loose files -------------------------------------------------------------

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}
void loader(const std::filesystem::path& root) {
    namespace cpn = eawr::campaign;
    const auto data = root / "loose";
    std::filesystem::remove_all(root);
    write(data / "xml" / "plots.xml", R"(<?xml version="1.0" ?>
<Story_Mode_Plots>
  <Lua_Script> Act_One </Lua_Script>
  <Active_Plot>sub_one.xml</Active_Plot>
  <Suspended_Plot>sub_two.xml</Suspended_Plot>
  <Lua_Script>Act_Three</Lua_Script>
  <Active_Plot></Active_Plot>
  <Active_Plot>sub_missing.xml</Active_Plot>
  <Active_Plot>sub_three.xml</Active_Plot>
  <Strange_Key>x</Strange_Key>
</Story_Mode_Plots>)");
    write(data / "xml" / "sub_one.xml", R"(<?xml version="1.0" ?>
<Story>
  <Event Name="Start_It">
    <Event_Type>STORY_ELAPSED</Event_Type>
    <Event_Param1> 1.5 </Event_Param1>
  </Event>
  <Event Name="Give">
    <Reward_Param2>Hoth, Dantooine
       Bespin</Reward_Param2>
    <Reward_Type>SPAWN_HERO</Reward_Type>
    <Event_Type>STORY_TRIGGER</Event_Type>
    <Reward_Param1>Han_Solo_Team</Reward_Param1>
    <Prereq>Start_It</Prereq>
    <Prereq>a, b</Prereq>
    <Branch>Br</Branch>
    <Perpetual>Yes</Perpetual>
    <Multiplayer>False</Multiplayer>
    <Story_Dialog>Dialog_X</Story_Dialog>
    <Story_Chapter>3</Story_Chapter>
    <Story_Tag>old_tag</Story_Tag>
    <Story_Incoming>new_tag</Story_Incoming>
    <Story_Dialog_Popup>0</Story_Dialog_Popup>
    <Inactive_Delay>12</Inactive_Delay>
    <Timeout>TBD</Timeout>
  </Event>
  <Event Name="Bogus"><Event_Type>STORY_NO_SUCH_TYPE</Event_Type></Event>
  <Event Name="Other"><Event_Type>Story_Elapsed</Event_Type></Event>
  <Event Name="start_it"><Event_Type>STORY_TECH_LEVEL</Event_Type><Event_Param1>2</Event_Param1></Event>
</Story>)");
    write(data / "xml" / "sub_two.xml", "<Story><Event Name=\"Only\"><Event_Type>STORY_TRIGGER</Event_Type></Event></Story>");
    write(data / "xml" / "sub_three.xml", "<Story><Event Name=\"Third\"><Event_Type>STORY_ELAPSED</Event_Type>"
        "<Event_Param1>0</Event_Param1><Story_Dialog>Incoming_Dialog</Story_Dialog><Story_Incoming>incoming_only</Story_Incoming></Event></Story>");
    eawr::vfs::MountSpec mount; mount.layer_id = "story-test"; mount.data_root = data;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    cpn::StoryPlots plots;
    take(cpn::load_story_plot(files, 7, "plots.xml", plots));
    expect(plots.plots.size() == 1 && plots.plots[0].owner == 7 && plots.plots[0].file == "plots.xml", "WST-01 the plot is recorded for its owning player");
    expect(plots.subplots.size() == 3, "WST-02 the missing sub-plot is skipped, the empty entry loads nothing");
    expect(plots.subplots[0].name == "sub_one.xml" && plots.subplots[0].active && plots.subplots[0].script == "Act_One",
        "WST-02/03 Active_Plot with the preceding script (trimmed)");
    expect(plots.subplots[1].name == "sub_two.xml" && !plots.subplots[1].active && plots.subplots[1].script.empty(),
        "WST-02 Suspended_Plot loads inactive; the script was consumed by the previous entry");
    expect(plots.subplots[2].name == "sub_three.xml" && plots.subplots[2].script.empty() && plots.subplots[2].owner == 7,
        "WST-02 a script before an empty entry is cleared by that entry");
    const auto& one = plots.subplots[0].events;
    expect(one.size() == 2, "unknown and mis-cased event types are dropped (WST-04/08), a repeated name replaces the earlier one");
    expect(one[0].name == "START_IT" && one[0].type == "STORY_TECH_LEVEL" && one[0].params[0] == std::vector<std::string>{"2"},
        "WST-04 names are upper-cased and the later duplicate replaces the earlier in its slot");
    const auto& give = one[1];
    expect(give.name == "GIVE" && give.type == "STORY_TRIGGER" && give.reward_type == "SPAWN_HERO", "WST-05 child order does not matter");
    expect(give.reward_params[1] == (std::vector<std::string>{"Hoth", "Dantooine", "Bespin"}) && give.reward_params[0].front() == "Han_Solo_Team",
        "WST-05 token lists split on spaces, commas and line breaks");
    expect(give.prereqs == (std::vector<std::vector<std::string>>{{"START_IT"}, {"A", "B"}}), "WST-06 each Prereq is one group of upper-cased names");
    expect(give.branch == "BR" && give.perpetual && !give.multiplayer && give.dialog == "Dialog_X" && give.chapter == 3
        && give.tag == "new_tag" && give.authored_tag_present == true && !give.popup && give.sound && give.inactive_delay == sec(12) && give.timeout == Fixed{},
        "WST-05/70 flags, dialog fields, Story_Incoming over Story_Tag, TBD ignored, defaults");
    const auto& incoming = plots.subplots[2].events.front();
    expect(incoming.tag == "incoming_only" && incoming.authored_tag_present == false, "WST-58/70 incoming is separate from authored tag presence");
    auto incoming_engine = engine({sub("incoming.xml", 7, {incoming})});
    take(incoming_engine.service(sec(0), false));
    const auto incoming_rows = incoming_engine.take_triggered();
    expect(incoming_rows.size() == 1 && incoming_rows.front().tag == "incoming_only" && incoming_rows.front().remove_after_viewed,
        "WST-58 the loader and engine retain removal-after-viewing for incoming-only dialogs");
    expect(std::any_of(plots.notes.begin(), plots.notes.end(), [](const auto& n) { return n.find("OTHER") != std::string::npos; })
        && plots.notes.size() >= 5, "dropped and skipped entries are reported");

    cpn::StoryPlots none;
    take(cpn::load_story_plot(files, 1, "  ", none));
    take(cpn::load_story_plot(files, 1, "absent.xml", none));
    expect(none.plots.empty() && none.subplots.empty(), "WST-01 an empty or missing plot name loads nothing and succeeds");
    expect(take(st::Engine::create({}, plots.plots, plots.subplots)).diagnostics().size() == 2, "loaded plots build an engine; the two unknown prerequisites are reported");
}

void campaign_story_routes(const std::filesystem::path& root) {
    namespace cpn = eawr::campaign;
    namespace dg = eawr::data::galactic;
    const auto directory = root / "campaign-routes";
    for (const auto* name : {"good.xml", "evil.xml", "corrupt.xml", "repeat.xml"})
        write(directory / "xml" / name, "<Story_Mode_Plots/>");
    write(directory / "xml" / "fourth.xml", "<Story_Mode_Plots><Active_Plot>fourth-sub.xml</Active_Plot></Story_Mode_Plots>");
    write(directory / "xml" / "fourth-sub.xml", "<Story><Event Name=\"Ready\"><Event_Type>STORY_ELAPSED</Event_Type>"
        "<Event_Param1>0</Event_Param1></Event></Story>");
    eawr::vfs::MountSpec mount; mount.layer_id = "campaign-routes"; mount.data_root = directory;
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    dg::GalacticData data;
    cpn::CampaignSetupOptions options;
    for (const auto* name : {"Copper", "Iron", "Amber", "Renamed_Fourth", "Dormant", "Neutral"}) {
        dg::Faction faction; faction.name = name; faction.is_neutral = faction.name == "Neutral";
        data.factions.push_back(faction);
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, name, id != 5});
    }
    options.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}};
    options.solo_reference_human = 1;
    dg::Planet planet; planet.name = "Home"; planet.behaviors = {"PLANET"};
    planet.galactic_position = dg::Position{}; data.planets.push_back(planet);
    data.constants = {{"MaximumFleetMovementDistance", "10", sec(10)}, {"Fiscal_Cycle_Time_In_Secs", "45", sec(45)},
        {"Good_Side_Name", "Copper", {}}, {"Evil_Side_Name", "Iron", {}}, {"Corrupt_Side_Name", "Amber", {}}};
    dg::Campaign campaign; campaign.name = "Renamed_Campaign"; campaign.starting_active_player = "Copper";
    campaign.locations = {"Home"}; campaign.planets = {"Home"};
    campaign.rebel_story_name = "good.xml"; campaign.empire_story_name = "evil.xml"; campaign.underworld_story_name = "corrupt.xml";
    campaign.story_names = {{"Renamed_Fourth", "fourth.xml"}, {"copper", "repeat.xml"},
        {"Renamed_Fourth", "missing.xml"}, {"Iron", "repeat.xml"}};
    data.campaigns.push_back(campaign);
    const auto plan = [&] { return take(cpn::plan_campaign_setup(data, {}, campaign.name, options)); };
    auto setup = plan();
    const auto plots = take(cpn::load_story_plots(files, data, setup));
    expect(plots.plots.size() == 6 && plots.plots[0].file == "good.xml" && plots.plots[1].file == "evil.xml"
        && plots.plots[2].file == "corrupt.xml" && plots.plots[3].owner == 4 && plots.plots[3].file == "fourth.xml"
        && plots.plots[4].owner == 1 && plots.plots[5].owner == 2,
        "WST-87 named routes precede additional renamed factions and repeated factions add loads");
    expect(plots.subplots.size() == 1 && plots.subplots[0].owner == 4, "the authored fourth faction owns its sub-plot");
    expect(std::any_of(plots.notes.begin(), plots.notes.end(), [](const auto& note) {
        return note.find("Renamed_Campaign") != std::string::npos && note.find("Renamed_Fourth") != std::string::npos
            && note.find("Data/XML/missing.xml") != std::string::npos;
    }), "WST-88 missing plots name the campaign/faction/path and later pairs still load");
    auto& routes = data.campaigns[0].story_names;
    const auto original = routes;
    for (const auto* faction : {"Unknown", "Dormant"}) {
        routes.insert(routes.begin() + 1, {faction, "repeat.xml"});
        const auto stopped = take(cpn::load_story_plots(files, data, plan()));
        expect(stopped.plots.size() == 4 && stopped.plots.back().owner == 4,
            "WST-88 unknown and inactive players stop additional pairs without undoing earlier or named routes");
        routes = original;
    }
    routes.push_back({"Iron", "fourth.xml"});
    const auto repeated = take(cpn::load_story_plots(files, data, plan()));
    expect(repeated.plots.size() == 7 && repeated.subplots.size() == 1 && repeated.subplots[0].owner == 2,
        "WST-89 repeating a plot retains both plot records and replaces the shared sub-plot owner");
    routes = original;
    std::string reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(cpn::GameSession::create(setup.galaxy()));
        take(game.install_story({}, plots.plots, plots.subplots));
        take(game.step(executor));
        const auto ids = cp::game_identities(setup.galaxy());
        cp::GameLoadOptions load;
        load.configure_session = [&](auto& fresh) { return fresh.install_story({}, plots.plots, plots.subplots); };
        const auto bytes = take(cp::save_game_checkpoint(game, ids));
        const auto restored = take(cp::load_game_checkpoint(bytes, ids, load, executor));
        expect(restored.session.story_engine()->state().subplots[0].owner == 4,
            "authored fourth-faction plot ownership survives a checkpoint");
        if (reference.empty()) reference = game.state_sha256();
        else expect(reference == game.state_sha256(), "authored plot routing gives equal session hashes on 1/2/4/8 workers");
        routes[0].name = "other.xml";
        const auto changed = plan();
        expect(changed.galaxy().content_identity != setup.galaxy().content_identity,
            "authored associations reach the setup content identity");
        expect(!cp::load_game_checkpoint(bytes, cp::game_identities(changed.galaxy()), load, executor),
            "a checkpoint refuses changed authored associations");
        auto replay = game.record();
        expect(replay.setup.content_identity != changed.galaxy().content_identity,
            "the replay records the campaign association identity");
        routes = original;
    }
}

// #5916 batch 2 part B: the type names that STORY_CONSTRUCT and STORY_DESTROY events author must resolve to the campaign's
// type bindings (the table the session names built and lost objects from). Report-only on which names do not resolve; the
// test fails when no authored name resolves at all. `mod_chain` empty = the installed game alone.
void installed_type_names(const std::filesystem::path& game_root, const std::optional<std::filesystem::path>& mod_chain, const char* label) {
    namespace cpn = eawr::campaign;
    auto roots = decltype(eawr::vfs::mod_chain_roots(std::filesystem::path{})){};
    if (mod_chain) roots = eawr::vfs::mod_chain_roots(*mod_chain);
    roots.emplace_back("expansion", game_root / "corruption" / "Data");
    roots.emplace_back("base", game_root / "GameData" / "Data");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (auto& layer : take(eawr::vfs::resolve_manifest_chain(roots))) mounts.push_back(std::move(layer.mount));
    const auto files = take(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = take(eawr::data::load_catalog(files, mod_chain ? eawr::data::Profile::remake : eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    cpn::CampaignSetupOptions options;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
    }
    std::vector<std::string> names;
    for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    auto routing_data = data;
    for (auto& row : routing_data.campaigns) row.starting_forces.clear();
    std::size_t campaigns{}, skipped{}, events{}, authored{}, resolved{}, hero_names{}, hero_flagged{};
    std::set<std::string> unresolved, not_heroes;
    for (const auto& campaign : data.campaigns) {
        // The stock campaigns reach their plots through the named side routes (WST-87), not `Story_Name` pairs.
        if (campaign.planets.empty() || campaigns >= 24) continue;
        const auto human = std::find_if(options.players.begin(), options.players.end(), [&](const auto& row) {
            return st::upper(row.faction) == st::upper(campaign.starting_active_player);
        });
        if (human == options.players.end()) continue;
        options.solo_reference_human = human->player.player_id;
        options.seats = {{{1}, human->player.player_id, gc::SeatKind::human, 0, true, 1}};
        try {
            const auto plan = take(cpn::plan_campaign_setup(routing_data, types, campaign.name, options));
            const auto plots = take(cpn::load_story_plots(files, data, plan));
            ++campaigns;
            for (const auto& sub : plots.subplots) for (const auto& event : sub.events) {
                const bool move = event.type == "STORY_MOVE" || event.type == "STORY_DEFEAT_HERO"; // parameter 1 lists heroes: each must be a named hero type
                if (event.type != "STORY_CONSTRUCT" && event.type != "STORY_DESTROY" && !move) continue;
                ++events;
                for (const auto& name : event.params[0]) {
                    ++authored;
                    const bool bound = std::any_of(plan.type_bindings().begin(), plan.type_bindings().end(), [&](const auto& binding) {
                        return st::upper(binding.original_type) == st::upper(name); });
                    if (bound) ++resolved; else unresolved.insert(name);
                    if (!move) continue;
                    ++hero_names;
                    const auto at = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(), [&](const auto& binding) {
                        return st::upper(binding.original_type) == st::upper(name); });
                    const auto* type = at == plan.type_bindings().end() ? nullptr : plan.original_type(at->id);
                    if (type && type->named_hero.value_or(false)) ++hero_flagged; else not_heroes.insert(name);
                }
            }
        } catch (const std::exception&) { ++skipped; }
    }
    std::cout << "installed type names (" << label << "): campaigns=" << campaigns << " skipped=" << skipped << " construct/destroy events=" << events
        << " authored type names=" << authored << " resolved=" << resolved << " unresolved distinct=" << unresolved.size()
        << " move hero names=" << hero_names << " flagged hero=" << hero_flagged << " not heroes distinct=" << not_heroes.size();
    std::size_t shown{};
    for (const auto& name : unresolved) { if (shown++ == 12) break; std::cout << (shown == 1 ? " [" : ", ") << name; }
    std::cout << (unresolved.empty() ? "" : "]") << '\n';
    expect(campaigns > 0, "installed chain has campaigns with story plots");
    expect(authored > 0, "the installed plots author construct or destroy type names");
    expect(resolved == authored && unresolved.empty(), "every authored construct, destroy and move type name resolves to the campaign's type bindings");
    expect(hero_flagged == hero_names && not_heroes.empty(), "every hero a move event names is a named hero type");
}

void installed_campaign_routes(const std::filesystem::path& game_root, const std::filesystem::path& mod_chain) {
    namespace cpn = eawr::campaign;
    auto roots = eawr::vfs::mod_chain_roots(mod_chain);
    roots.emplace_back("expansion", game_root / "corruption" / "Data");
    roots.emplace_back("base", game_root / "GameData" / "Data");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (auto& layer : take(eawr::vfs::resolve_manifest_chain(roots))) mounts.push_back(std::move(layer.mount));
    const auto files = take(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = take(eawr::data::load_catalog(files, eawr::data::Profile::remake));
    const auto data = take(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    cpn::CampaignSetupOptions options;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
    }
    std::vector<std::string> names;
    for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    // This opt-in case qualifies story routing independently of starting-force placement admission.
    // Keep every authored story association and the real faction/galaxy data unchanged.
    auto routing_data = data;
    for (auto& row : routing_data.campaigns) row.starting_forces.clear();
    std::size_t checked{};
    for (const auto& campaign : data.campaigns) {
        if (campaign.story_names.empty() || campaign.planets.empty()) continue;
        const auto human = std::find_if(options.players.begin(), options.players.end(), [&](const auto& row) {
            return eawr::sim::galactic::story::upper(row.faction) == eawr::sim::galactic::story::upper(campaign.starting_active_player);
        });
        if (human == options.players.end()) continue;
        options.solo_reference_human = human->player.player_id;
        options.seats = {{{1}, human->player.player_id, gc::SeatKind::human, 0, true, 1}};
        const auto plan = take(cpn::plan_campaign_setup(routing_data, types, campaign.name, options));
        const auto plots = take(cpn::load_story_plots(files, data, plan));
        for (const auto& route : campaign.story_names) {
            const auto owner = std::find_if(options.players.begin(), options.players.end(), [&](const auto& row) {
                return st::upper(row.faction) == st::upper(route.faction);
            });
            expect(owner != options.players.end(), "installed association names an existing faction");
            if (owner == options.players.end()) break;
            expect(std::any_of(plots.plots.begin(), plots.plots.end(), [&](const auto& plot) {
                return plot.owner == owner->player.player_id && plot.file == route.name;
            }), "installed authored association reaches its player's plot list");
        }
        expect(static_cast<bool>(st::Engine::create({}, plots.plots, plots.subplots)), "installed associations produce a valid story root");
        std::cout << "installed campaign=" << campaign.name << " associations=" << campaign.story_names.size()
            << " plots=" << plots.plots.size() << " subplots=" << plots.subplots.size() << '\n';
        ++checked;
        break; // One selected installed campaign qualifies this data path; it is not a whole-mod admission test.
    }
    expect(checked > 0, "installed chain contains a playable campaign with additional faction story associations");
}

// ---- the engine inside a GameSession --------------------------------------------------------------

gc::GalacticSetup session_setup() {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1;
    auto config = gc::synthetic_galaxy(options);
    config.rules.maximum_fleet_movement_distance = Fixed::from_raw(10000 * Fixed::scale);
    config.rules.fiscal_cycle_frames = 1000;
    config.fleet_control = gc::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
    config.player_economy = gc::PlayerEconomySetup{{{1, Fixed::from_raw(250 * Fixed::scale), Fixed::from_raw(10000 * Fixed::scale), 2, 5},
        {2, Fixed::from_raw(250 * Fixed::scale), Fixed::from_raw(10000 * Fixed::scale), 0, 5}}};
    config.fiscal = gc::FiscalSetup{Fixed::from_raw(2500 * Fixed::scale), {{1, true, Fixed::from_raw(Fixed::scale)}, {2, true, Fixed::from_raw(Fixed::scale)}}, {}};
    config.production = gc::ProductionSetup{5, {}, {}};
    for (const auto& player : config.players) {
        const bool human = std::any_of(config.seats.begin(), config.seats.end(), [&](const auto& seat) {
            return seat.player == player.player_id && seat.kind == gc::SeatKind::human;
        });
        config.production->players.push_back({player.player_id, human, !human, human, {}, {}, {}, {}, {}});
    }
    for (const auto& planet : config.planets) {
        config.fiscal->planets.push_back({planet.id, Fixed::from_raw(10 * Fixed::scale)});
        gc::PlanetProduction production; production.planet = planet.id; production.target_owner = planet.owner;
        config.production->planets.push_back(production);
    }
    return config;
}
std::vector<st::SubPlotDef> session_plots() {
    auto one_second = make("START", "STORY_ELAPSED", {"1"});
    return {sub("S.xml", 1, {one_second, trig("SPAWN", {{"START"}}, "SPAWN_HERO", {"Tarkin"}), make("ENTER", "STORY_SELECT_PLANET", {"Naboo"}),
                make("TECH3", "STORY_TECH_LEVEL", {"3"}), trig("GIFT", {{"TECH3"}}, "UNIQUE_UNIT", {"Accuser"}),
                make("INITIAL", "STORY_TECH_LEVEL", {"2"}), trig("BOBA", {{"INITIAL"}}, "SPAWN_HERO", {"Boba"})}),
        sub("P2.xml", 2, {make("TECH1", "STORY_TECH_LEVEL", {"1"}), trig("P2GIFT", {{"TECH1"}}, "SPAWN_HERO", {"Vader"})})};
}
std::vector<st::PlotRecord> session_records() { return {{1, "p1.xml"}, {2, "p2.xml"}}; }
void install(eawr::campaign::GameSession& game) { take(game.install_story({}, session_records(), session_plots())); }

void session() {
    const auto setup = session_setup();
    const auto ids = cp::game_identities(setup);
    std::vector<std::string> baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto plain = take(eawr::campaign::GameSession::create(setup));
        auto game = take(eawr::campaign::GameSession::create(setup));
        expect(game.story_engine() == nullptr, "no plots, no engine");
        install(game);
        expect(game.story_engine() && !take(eawr::campaign::GameSession::create(setup)).story_engine(), "the engine belongs to the session it was installed into");
        expect(!game.install_story({}, session_records(), session_plots()), "plots install once");
        std::vector<std::string> trail;
        std::string first_hash, plain_hash;
        std::uint64_t spawn_tick{};
        for (int i = 0; i < 40; ++i) {
            const auto tick = take(game.step(executor));
            static_cast<void>(take(plain.step(executor)));
            for (const auto& row : tick.story_triggers) {
                trail.push_back(row.event);
                if (row.event == "SPAWN") spawn_tick = tick.completed_tick;
            }
            if (i == 0) {
                expect(std::any_of(tick.diagnostics.begin(), tick.diagnostics.end(), [](const auto& d) {
                    return d.code == "EAWR-STORY-0001" && d.message.find("STORY_SELECT_PLANET") != std::string::npos; }),
                    "the engine's refusals are published once as diagnostics of the first step");
                expect(names(tick.story_triggers) == Names{"BOBA"}, "WGS-140 the tech set up before the first step reaches the plots at the first service");
                first_hash = game.state_sha256(); plain_hash = plain.state_sha256();
            }
        }
        expect(spawn_tick > 25 && spawn_tick < 40, "WST-12 the one-second elapsed event fires after about thirty galactic frames");
        expect(game.state_sha256() != plain.state_sha256() && first_hash != plain_hash, "the plot state joins the session digest");
        auto replay = take(eawr::campaign::GameSession::create(setup));
        install(replay);
        std::vector<std::string> hashes;
        for (int i = 0; i < 40; ++i) { static_cast<void>(take(replay.step(executor))); hashes.push_back(replay.state_sha256()); }
        expect(replay.state_sha256() == game.state_sha256(), "the same plots replay to the same digest");
        // WGEP-130/147: the assigned level reaches the story; the caller's own handler still runs.
        int caller{};
        game.set_technology_notice_handler([&](const auto&) { ++caller; return eawr::core::Result<void>::success(); });
        expect(take(game.set_technology({1, 3, true, true})) == 3 && caller >= 1, "the caller's technology handler still runs");
        const auto stepped = take(game.step(executor));
        expect(trail.size() == 2 && names(stepped.story_triggers) == Names{"GIFT"}, "a tech level assigned between steps fires its plot event in the next step");
        expect(take(game.set_technology({2, 1, true, true})) == 1, "the other player's tech");
        expect(names(take(game.step(executor)).story_triggers) == Names{"P2GIFT"}, "only the owner's sub-plot answers");

        // WST-60..62 through the game checkpoint.
        cp::GameLoadOptions options;
        options.configure_session = [](eawr::campaign::GameSession& fresh) { return fresh.install_story({}, session_records(), session_plots()); };
        const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto restored = take(cp::load_game_checkpoint(saved, ids, options, executor));
        expect(restored.session.story_engine() && restored.session.story_engine()->digest() == game.story_engine()->digest(),
            "the saved plot state is restored");
        expect(restored.session.story_engine()->triggered("S.xml", "SPAWN") && restored.session.story_engine()->triggered("P2.xml", "P2GIFT"),
            "triggered events survive a save and load");
        expect(take(cp::save_game_checkpoint(restored.session, ids)) == saved, "the restored session saves the same bytes");
        for (int i = 0; i < 5; ++i) {
            const auto a = take(game.step(executor)); const auto b = take(restored.session.step(executor));
            expect(a.story_triggers.empty() && b.story_triggers.empty() && game.state_sha256() == restored.session.state_sha256(),
                "events do not fire a second time after a load, and the digests agree");
        }
        auto without = cp::load_game_checkpoint(saved, ids, {}, executor);
        expect(!without, "a save with plots needs its plots installed on load");
        if (workers == 1) baseline = {first_hash, game.state_sha256()};
        else expect(baseline == (std::vector<std::string>{first_hash, game.state_sha256()}), "1/2/4/8 workers give equal plot-state digests");
    }
}

// ---- WST-32/34/35: the battle, conquest and loss evaluators (#5916 batch 1) ------------------------------------

st::EventDef counting(std::string name, std::string type, std::vector<std::vector<std::string>> params, bool perpetual = false) {
    st::EventDef e;
    e.name = std::move(name); e.type = std::move(type); e.perpetual = perpetual;
    for (std::size_t i = 0; i < params.size(); ++i) e.params[i] = std::move(params[i]);
    e.reward_type = "CREDITS"; e.reward_params[0] = {"1"};
    return e;
}
st::BattleNotice battle(st::Battle kind, std::string planet, st::BaseKind base) {
    st::BattleNotice n; n.kind = kind; n.planet = std::move(planet); n.base = base; return n;
}
st::ConquestNotice conquest(std::string planet, st::Player new_owner, st::Player old_owner) {
    return st::ConquestNotice{std::move(planet), new_owner, old_owner, 0};
}
st::LossNotice loss(st::Loss kind, std::string type, std::string planet, st::Player owner, st::BaseKind base = st::BaseKind::either) {
    st::LossNotice n; n.kind = kind; n.type = std::move(type); n.planet = std::move(planet); n.object_owner = owner; n.base = base; return n;
}

void battle_events() {
    // WST-35: kind (space/ground), count, then planet; wins and losses are separate events; the notice reaches only its player.
    auto e = engine({sub("S.xml", 1, {
        counting("WIN2", "STORY_WIN_BATTLES", {{"2"}, {"SPACE"}}),
        counting("LOSE1", "STORY_LOSE_BATTLES", {{"1"}, {"SPACE"}}),
        counting("GROUND", "STORY_WIN_BATTLES", {{"1"}, {"Land"}}),
        counting("AT", "STORY_WIN_BATTLES", {{"2"}, {"EITHER"}, {}, {"beta"}}),
        counting("BAD", "STORY_LOSE_BATTLES", {{"1"}, {"BASE_SPACE"}}),
    }), sub("P2.xml", 2, {counting("OTHER", "STORY_WIN_BATTLES", {{"1"}, {"SPACE"}})})});
    take(e.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    expect(!e.triggered("S.xml", "WIN2") && e.take_triggered().empty(), "WST-35 one win of two does not fire");
    expect(!e.triggered("P2.xml", "OTHER"), "WST-14 a notice for player 1 never reaches the plot of player 2");
    take(e.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::ground)));
    expect(e.triggered("S.xml", "GROUND") && !e.triggered("S.xml", "WIN2"), "a ground win counts for the ground event only; `Land` reads as ground (WST-08)");
    expect(!e.triggered("S.xml", "AT"), "the second battle is counted but Alpha is not on the list (count, then planet)");
    take(e.notify(1, battle(st::Battle::won, "Beta", st::BaseKind::space)));
    expect(e.triggered("S.xml", "WIN2") && e.triggered("S.xml", "AT"), "the second space win fires the space event, and the third count at Beta the planet one");
    expect(!e.triggered("S.xml", "LOSE1"), "wins never feed a lose event");
    take(e.notify(1, battle(st::Battle::lost, "Alpha", st::BaseKind::space)));
    expect(e.triggered("S.xml", "LOSE1") && !e.triggered("S.xml", "BAD"), "a space loss fires the space lose event; BASE_SPACE is not in the table so it reads as ground (WST-08)");
    take(e.notify(1, battle(st::Battle::lost, "Alpha", st::BaseKind::ground)));
    expect(e.triggered("S.xml", "BAD"), "and that unknown spelling matches a ground battle");
    take(e.notify(2, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    expect(e.triggered("P2.xml", "OTHER"), "the notice for player 2 reaches the plot of player 2");
    expect(names(e.take_triggered()) == (Names{"GROUND", "WIN2", "AT", "LOSE1", "BAD", "OTHER"}), "events fire in notice order, then document order");
    take(e.notify(std::nullopt, battle(st::Battle::retreat, "Alpha", st::BaseKind::space)));
    expect(e.take_triggered().empty() && e.state().counters.empty(), "retreat and victory notices wait for the next batch and count nothing");
}

void battle_perpetual_and_classes() {
    auto repeat = counting("EVERY", "STORY_WIN_BATTLES", {{"2"}, {"EITHER"}}, true);
    auto classes = counting("CLASSY", "STORY_WIN_BATTLES", {{"1"}, {"SPACE"}, {"FRIENDLY_CONTAINS", "CLASS_CAPITAL"}});
    auto e = engine({sub("S.xml", 1, {repeat, classes})});
    for (int i = 0; i < 4; ++i) take(e.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    expect(names(e.take_triggered()) == (Names{"EVERY", "EVERY"}), "WST-22 a perpetual counting event starts again from zero after each trigger");
    expect(!e.triggered("S.xml", "CLASSY") && e.diagnostics().size() == 1 && e.diagnostics()[0].event == "CLASSY",
        "the fleet-content filter is refused with one diagnostic, never a wrong match");
    // No planet list on a count event and an absent count: one battle is enough (default 1).
    auto bare = engine({sub("S.xml", 1, {counting("BARE", "STORY_LOSE_BATTLES", {})})});
    take(bare.notify(1, battle(st::Battle::lost, "Anywhere", st::BaseKind::ground)));
    expect(bare.triggered("S.xml", "BARE"), "defaults: count one, either kind, any planet");
}

void conquest_events() {
    st::Config config;
    config.relations[{1, 3}] = st::Config::Relation::friendly;
    config.relations[{1, 2}] = st::Config::Relation::enemy;
    auto e = engine({sub("S.xml", 1, {
        counting("FRIENDLY", "STORY_CONQUER", {{}, {}, {"FILTER_FRIENDLY_ONLY"}}),
        counting("ENEMY", "STORY_CONQUER", {{}, {}, {"FILTER_ENEMY_ONLY"}}),
        counting("NEUTRAL", "STORY_CONQUER", {{}, {}, {"FILTER_NEUTRAL_ONLY"}}),
        counting("ANY_NONE", "STORY_CONQUER", {{"Gamma"}, {}, {"FILTER_NONE"}}),
        counting("TWICE", "STORY_CONQUER", {{"A", "B"}, {"2"}}),
        counting("THREE", "STORY_CONQUER_COUNT", {{"3"}}),
    })}, config);
    take(e.notify(std::nullopt, conquest("A", 1, 9)));
    take(e.notify(std::nullopt, st::ConquestCountNotice{"A", 0}));
    expect(names(e.take_triggered()) == (Names{"FRIENDLY"}), "WST-32 the owner's own conquest passes the friendly filter only (the first of two counted for TWICE)");
    take(e.notify(std::nullopt, conquest("B", 2, 9)));
    take(e.notify(std::nullopt, st::ConquestCountNotice{"B", 0}));
    expect(names(e.take_triggered()) == (Names{"ENEMY", "TWICE"}), "an enemy conquest fires the enemy filter; TWICE counts down to zero over its two listed planets");
    take(e.notify(std::nullopt, conquest("Gamma", 7, 9)));
    take(e.notify(std::nullopt, st::ConquestCountNotice{"Gamma", 0}));
    expect(names(e.take_triggered()) == (Names{"NEUTRAL", "ANY_NONE", "THREE"}), "a stranger's conquest is neutral; FILTER_NONE with a listed planet; the third conquest fires the count");
    auto only = engine({sub("S.xml", 1, {counting("C", "STORY_CONQUER", {})}), sub("P2.xml", 2, {counting("C2", "STORY_CONQUER", {})})});
    take(only.notify(2, conquest("A", 2, 9)));
    expect(names(only.take_triggered()) == (Names{"C2"}), "a notice that names a player reaches only the plots of that player (WST-14)");
}

void loss_events() {
    st::Config config;
    config.relations[{1, 2}] = st::Config::Relation::enemy;
    auto e = engine({sub("S.xml", 1, {
        counting("TWO", "STORY_DESTROY", {{"Type_One", "Type_Two"}, {}, {"2", "1"}}),
        counting("HERE", "STORY_DESTROY", {{"Type_Two"}, {"Alpha"}, {}, {"FILTER_ENEMY_ONLY"}}),
        counting("BASE", "STORY_DESTROY_BASE", {{"Alpha"}, {"SPACE"}, {"FILTER_ENEMY_ONLY"}}),
        counting("EMPTYLIST", "STORY_DESTROY", {{}, {}, {"1"}}),
    })}, config);
    for (const auto& row : e.unavailable()) expect(row.event == "BASE" && row.reason.find("no starbase-loss notice") != std::string::npos,
        "only the starbase loss lacks an emitter: battle losses are announced");
    expect(e.unavailable().size() == 1, "the starbase-loss event is listed until the session emits the notice");
    take(e.notify(std::nullopt, loss(st::Loss::destroy, "type_one", "Anywhere", 2)));
    expect(!e.triggered("S.xml", "TWO") && e.state().counters.size() == 1 && e.state().counters[0].values == (std::vector<std::int64_t>{1, 1}),
        "WST-34 the type matches case-insensitively and its own count (2) runs down");
    take(e.notify(std::nullopt, loss(st::Loss::destroy, "Type_Two", "Anywhere", 2)));
    expect(e.triggered("S.xml", "TWO") && !e.triggered("S.xml", "HERE"), "the second type has its own count of one; a loss elsewhere never matches the planet event");
    take(e.notify(std::nullopt, loss(st::Loss::destroy, "Type_Two", "Alpha", 1)));
    expect(!e.triggered("S.xml", "HERE"), "the owner of the destroyed object must pass the filter when planets are listed");
    take(e.notify(std::nullopt, loss(st::Loss::destroy, "Type_Two", "Alpha", 2)));
    expect(e.triggered("S.xml", "HERE") && !e.triggered("S.xml", "EMPTYLIST"), "an enemy loss at the listed planet fires; an empty type list matches nothing");
    take(e.notify(std::nullopt, loss(st::Loss::destroy_base, "", "Alpha", 2, st::BaseKind::ground)));
    expect(!e.triggered("S.xml", "BASE"), "a ground base is not a space base");
    take(e.notify(std::nullopt, loss(st::Loss::destroy_base, "", "Alpha", 2, st::BaseKind::space)));
    expect(e.triggered("S.xml", "BASE") && e.state().counters.empty(), "WST-34 the enemy space base at Alpha fires, and no counter outlives a fired event");
}

void counters_save_and_digest() {
    const auto plots = [] {
        return std::vector<st::SubPlotDef>{sub("S.xml", 1, {counting("WIN3", "STORY_WIN_BATTLES", {{"3"}, {"SPACE"}}),
            counting("COUNT", "STORY_CONQUER_COUNT", {{"2"}}), counting("QUIET", "STORY_TECH_LEVEL", {{"9"}})})};
    };
    auto a = engine(plots());
    const auto untouched = a.digest();
    take(a.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::ground)));
    expect(a.digest() == untouched && a.state().counters.empty(), "a notice no event counts leaves the state, and so the digest, as it was");
    take(a.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    take(a.notify(std::nullopt, st::ConquestCountNotice{"A", 0}));
    expect(a.digest() != untouched && a.state().counters.size() == 2, "a counting event adds a sparse row and moves the digest");
    expect(a.state().counters[0].event == "COUNT" && a.state().counters[1].event == "WIN3", "rows are sorted by event name");
    auto b = engine(plots());
    take(b.restore(a.state()));
    expect(a.digest() == b.digest(), "a restored engine has the same digest with its counters");
    for (auto* each : {&a, &b}) {
        take(each->notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
        take(each->notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
        take(each->notify(std::nullopt, st::ConquestCountNotice{"B", 0}));
    }
    expect(names(a.take_triggered()) == names(b.take_triggered()) && a.triggered("S.xml", "WIN3") && b.triggered("S.xml", "COUNT")
        && a.digest() == b.digest() && a.state().counters.empty(), "both engines count on from the saved values and fire together");
    auto duplicate = a.state(); duplicate.counters = {{"S.xml", "COUNT", {1}}, {"S.xml", "COUNT", {2}}};
    auto d = engine(plots());
    expect(!d.restore(duplicate), "two counter rows for one event are refused (the key must be unique, not the whole row)");
    auto bad = a.state(); bad.counters = {{"S.xml", "QUIET", {1}}};
    auto c = engine(plots());
    expect(!c.restore(bad), "a counter row for an event that does not count is refused");
    bad.counters = {{"S.xml", "WIN3", {1}}, {"S.xml", "COUNT", {1}}};
    expect(!c.restore(bad), "unsorted counter rows are refused");
    bad.counters = {{"S.xml", "GHOST", {1}}};
    expect(!c.restore(bad), "a counter row for an unknown event is refused");
}

void counter_restart_after_lua_edit() {
    // WST-56: an Event_Param edit restarts the count of that event.
    auto e = engine({sub("S.xml", 1, {counting("WIN3", "STORY_WIN_BATTLES", {{"3"}, {"SPACE"}})})});
    take(e.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    take(e.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    expect(e.state().counters.size() == 1 && e.state().counters[0].values[0] == 2, "two of three counted");
    st::PlotCommand edit; edit.op = st::PlotCommand::Op::event_param; edit.subplot = "S.xml"; edit.event = "WIN3"; edit.index = 0; edit.values = {"2"};
    take(e.apply(edit));
    expect(e.state().counters.empty(), "the edit drops the running count");
    take(e.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    take(e.notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    expect(e.triggered("S.xml", "WIN3"), "and the event now fires at the edited count of two");
}

void counters_in_the_game_checkpoint() {
    // The counter tail follows the edit and flag tails: absent for a game that counts nothing (the old bytes), present
    // and restored for one that is part-way through a count. The engine is reached through the session's own pointer.
    const auto setup = session_setup();
    const auto ids = cp::game_identities(setup);
    const auto plots = [] {
        return std::vector<st::SubPlotDef>{sub("C.xml", 1, {counting("WIN3", "STORY_WIN_BATTLES", {{"3"}, {"SPACE"}})})};
    };
    const auto records = std::vector<st::PlotRecord>{{1, "p1.xml"}};
    eawr::platform::ThreadWorkerAdapter executor(1);
    cp::GameLoadOptions options;
    options.configure_session = [&](eawr::campaign::GameSession& fresh) { return fresh.install_story({}, records, plots()); };
    auto game = take(eawr::campaign::GameSession::create(setup));
    take(game.install_story({}, records, plots()));
    for (int i = 0; i < 3; ++i) static_cast<void>(take(game.step(executor)));
    const auto plain = take(cp::save_game_checkpoint(game, ids));
    auto* engine_of = const_cast<st::Engine*>(game.story_engine()); // test-only: the session has no public notice entry
    take(engine_of->notify(1, battle(st::Battle::won, "Alpha", st::BaseKind::space)));
    const auto counted = take(cp::save_game_checkpoint(game, ids));
    expect(counted.size() > plain.size(), "a part-way count adds the counter tail to the checkpoint");
    auto restored = take(cp::load_game_checkpoint(counted, ids, options, executor));
    expect(restored.session.story_engine()->state().counters == game.story_engine()->state().counters && !game.story_engine()->state().counters.empty(),
        "the counter rows come back");
    expect(restored.session.story_engine()->digest() == game.story_engine()->digest(), "with the same plot digest");
    expect(take(cp::save_game_checkpoint(restored.session, ids)) == counted, "and the restored session saves the same bytes");
    auto earlier = take(cp::load_game_checkpoint(plain, ids, options, executor));
    expect(earlier.session.story_engine()->state().counters.empty(), "a save without the tail loads with no counters");
}

// ---- WST-31: arrivals and bounces (#5916 batch 2, part A) ------------------------------------------------------

st::TravelNotice arrival(st::Travel kind, std::string planet, st::Player owner, std::vector<std::string> types,
    std::size_t direct = 0, bool stealth = false) {
    st::TravelNotice n; n.kind = kind; n.planet = std::move(planet);
    n.fleet.owner = owner; n.fleet.types = std::move(types); n.fleet.direct_members = direct; n.fleet.stealth = stealth;
    return n;
}

void travel_events() {
    st::Config config;
    config.relations[{1, 2}] = st::Config::Relation::enemy;
    auto e = engine({sub("S.xml", 1, {
        counting("ANY", "STORY_ENTER", {}),
        counting("HERE", "STORY_ENTER", {{"alpha"}}),
        counting("MINE", "STORY_ENTER", {{}, {"FILTER_FRIENDLY_ONLY"}}),
        counting("FOES", "STORY_ENTER", {{}, {"FILTER_ENEMY_ONLY"}}),
        counting("NEEDS", "STORY_ENTER", {{"beta"}, {}, {"Type_One", "Type_Two"}}),
        counting("ONLY", "STORY_ENTER", {{"gamma"}, {}, {"Type_One"}, {}, {}, {"1"}}),
        counting("SNEAK", "STORY_ENTER", {{"delta"}, {}, {}, {}, {"1"}}),
        counting("NOSNEAK", "STORY_ENTER", {{"delta"}}),
        counting("ORBIT", "STORY_ENTER", {{"epsilon"}, {}, {}, {"Type_One"}}),
        counting("RAID", "STORY_ENTER", {{"epsilon"}, {}, {}, {}, {}, {}, {"1"}}),
        counting("BOUNCE", "STORY_FLEET_BOUNCED", {{"alpha"}}),
        counting("BOUNCE_SNEAK", "STORY_FLEET_BOUNCED", {{"alpha"}, {}, {}, {}, {"1"}}),
        counting("REFUSED", "STORY_INVASION_BOUNCED", {}),
    })}, config);
    for (const auto& row : e.unavailable()) expect(row.event == "REFUSED" && row.reason.find("no refused-invasion notice") != std::string::npos,
        "only the invasion bounce lacks an emitter");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Alpha", 1, {"Type_One"}, 1)));
    expect(names(e.take_triggered()) == (Names{"ANY", "HERE", "MINE"}), "WST-31 a planet list, case-insensitive, and the friendly filter relative to the plot's owner");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Zeta", 2, {"Type_One"}, 1)));
    expect(names(e.take_triggered()) == (Names{"FOES"}), "an enemy fleet fires only the enemy filter");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Beta", 1, {"Type_One"}, 1)));
    expect(!e.triggered("S.xml", "NEEDS"), "every required type must be inside the fleet");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Beta", 1, {"type_two", "TYPE_ONE", "Type_Three"}, 3)));
    expect(e.triggered("S.xml", "NEEDS"), "required types match case-insensitively among others");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Gamma", 1, {"Type_One", "Type_Two"}, 2)));
    expect(!e.triggered("S.xml", "ONLY"), "exclusive: the unflattened member count must equal the number of required types");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Gamma", 1, {"Type_One", "Type_Two"}, 1)));
    expect(e.triggered("S.xml", "ONLY"), "one direct member holding both flattened types passes the exclusive test for one required type");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Delta", 1, {"Type_One"}, 1, true)));
    expect(e.triggered("S.xml", "SNEAK") && !e.triggered("S.xml", "NOSNEAK"),
        "with no required types a stealth fleet passes only when stealth is allowed");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Delta", 1, {"Type_One"}, 1)));
    expect(e.triggered("S.xml", "NOSNEAK"), "an ordinary fleet passes");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Epsilon", 1, {"Type_One"}, 1)));
    expect(!e.triggered("S.xml", "ORBIT") && !e.triggered("S.xml", "RAID") && e.diagnostics().size() == 2,
        "the orbit-type list and the raid flag are refused with one diagnostic each, never a wrong match");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Epsilon", 1, {"Type_One"}, 1)));
    expect(e.diagnostics().size() == 2, "and not again");
    take(e.notify(std::nullopt, arrival(st::Travel::fleet_bounced, "Alpha", 1, {"Type_One"}, 1, true)));
    expect(!e.triggered("S.xml", "BOUNCE"), "a stealth fleet that bounces does not fire a bounce event that does not allow stealth");
    take(e.notify(std::nullopt, arrival(st::Travel::fleet_bounced, "Alpha", 1, {"Type_One"}, 1)));
    expect(e.triggered("S.xml", "BOUNCE") && !e.triggered("S.xml", "REFUSED"), "an ordinary bounce reaches only the bounce events");
    expect(names(e.take_triggered()) == (Names{"NEEDS", "ONLY", "SNEAK", "NOSNEAK", "BOUNCE_SNEAK", "BOUNCE"}), "every fired event left one record, in order");
    take(e.notify(std::nullopt, arrival(st::Travel::deploy_hero, "Alpha", 1, {"Type_One"}, 1)));
    expect(e.take_triggered().empty(), "hero deployment waits for a later batch");
}

// WST-35: STORY_DEFEAT_HERO counts the defeat of a listed hero type; the owner filter is relative to the plot's owner.
void hero_defeat_events() {
    st::Config config;
    config.relations[{1, 2}] = st::Config::Relation::enemy;
    auto e = engine({sub("S.xml", 1, {
        counting("ANYFOE", "STORY_DEFEAT_HERO", {{"Hero_One", "Hero_Two"}, {"2", "1"}, {"FILTER_ENEMY_ONLY"}}),
        counting("OURS", "STORY_DEFEAT_HERO", {{"Hero_One"}, {}, {"FILTER_FRIENDLY_ONLY"}}),
        counting("NOLIST", "STORY_DEFEAT_HERO", {}),
        counting("DESTROYED", "STORY_DESTROY", {{"Hero_One"}}),
    })}, config);
    expect(e.unavailable().empty(), "STORY_DEFEAT_HERO has an evaluator and an emitter");
    take(e.notify(std::nullopt, loss(st::Loss::hero_defeat, "hero_one", "Alpha", 1)));
    expect(names(e.take_triggered()) == (Names{"OURS"}), "a friendly hero fires the friendly filter only, case-insensitively");
    take(e.notify(std::nullopt, loss(st::Loss::hero_defeat, "Hero_One", "Alpha", 2)));
    expect(!e.triggered("S.xml", "ANYFOE") && e.state().counters.size() == 1 && e.state().counters[0].values == (std::vector<std::int64_t>{1, 1}),
        "an enemy hero counts down its own entry of the count list (2)");
    take(e.notify(std::nullopt, loss(st::Loss::hero_defeat, "Hero_Two", "Beta", 2)));
    expect(e.triggered("S.xml", "ANYFOE") && !e.triggered("S.xml", "NOLIST"),
        "the second type has its own count of one and fires the event; an empty type list matches nothing");
    take(e.notify(std::nullopt, loss(st::Loss::destroy, "Hero_One", "Alpha", 2)));
    expect(e.triggered("S.xml", "DESTROYED"), "an ordinary destroy notice stays a destroy event");
}

// WST-31: STORY_MOVE is sent once per named hero in an arriving fleet; hero list and planet list both match.
st::TravelNotice hero_move(std::string hero, std::string planet, st::Player owner = 1) {
    auto n = arrival(st::Travel::move_hero, std::move(planet), owner, {}, 0);
    n.hero = std::move(hero);
    return n;
}

void move_events() {
    st::Config config;
    config.relations[{1, 2}] = st::Config::Relation::enemy;
    auto e = engine({sub("S.xml", 1, {
        counting("ANY", "STORY_MOVE", {}),
        counting("HERO_ONLY", "STORY_MOVE", {{"Hero_One", "Hero_Two"}}),
        counting("HERE_ONLY", "STORY_MOVE", {{}, {"alpha"}}),
        counting("BOTH", "STORY_MOVE", {{"hero_one"}, {"Alpha", "Beta"}}),
        counting("ENTERS", "STORY_ENTER", {}),
    })}, config);
    expect(e.unavailable().empty(), "STORY_MOVE has an evaluator and an emitter");
    take(e.notify(std::nullopt, hero_move("Hero_Three", "Gamma")));
    expect(names(e.take_triggered()) == (Names{"ANY"}), "an empty hero and planet list match any hero anywhere");
    take(e.notify(std::nullopt, hero_move("HERO_TWO", "Gamma", 2)));
    expect(names(e.take_triggered()) == (Names{"HERO_ONLY"}), "a hero list matches case-insensitively, and an enemy hero counts: there is no owner filter");
    take(e.notify(std::nullopt, hero_move("Hero_One", "Alpha")));
    expect(names(e.take_triggered()) == (Names{"HERE_ONLY", "BOTH"}), "hero and planet must both match");
    take(e.notify(std::nullopt, arrival(st::Travel::enter, "Alpha", 1, {"Hero_One"}, 1)));
    expect(names(e.take_triggered()) == (Names{"ENTERS"}), "a plain arrival does not fire a move event by itself");
}

// WST-31: the campaign layer expands one arrival into one move notice per hero (hero_moves), in the order given; two heroes
// arriving together fire two events in that order, and an arrival without heroes sends none.
void hero_moves_per_arrival() {
    auto e = engine({sub("S.xml", 1, {
        counting("FIRST", "STORY_MOVE", {{"Hero_One"}}),
        counting("SECOND", "STORY_MOVE", {{"Hero_Two"}}),
        counting("WRONG_PLACE", "STORY_MOVE", {{"Hero_One"}, {"Beta"}}),
    })});
    const auto together = arrival(st::Travel::enter, "Alpha", 1, {"Hero_One", "Hero_Two", "Type_One"}, 2);
    const std::vector<std::string> heroes{"Hero_One", "Hero_Two"};
    const auto moves = st::hero_moves(together, heroes);
    expect(moves.size() == 2 && moves[0].hero == "Hero_One" && moves[1].hero == "Hero_Two" && moves[0].kind == st::Travel::move_hero
        && moves[0].planet == "Alpha" && moves[1].fleet.owner == 1, "one move notice per hero, in the order given, carrying the arrival's planet and fleet");
    for (const auto& move : moves) take(e.notify(std::nullopt, move));
    expect(names(e.take_triggered()) == (Names{"FIRST", "SECOND"}), "two heroes in one arrival fire two events in a fixed order");
    expect(st::hero_moves(together, {}).empty(), "an arrival without heroes sends no move notice");
}

// ---- WST-33: builds and base levels (#5916 batch 2, part B) ----------------------------------------------------

st::EconomyNotice built(std::string type, st::Player owner, std::string planet = "alpha") {
    st::EconomyNotice n; n.kind = st::Economy::construct; n.type = std::move(type); n.planet = std::move(planet); n.object_owner = owner;
    n.base = st::BaseKind::ground; n.value = 1; return n;
}
st::EconomyNotice level(std::int64_t value, st::BaseKind base, st::Player owner, std::string planet = "alpha") {
    st::EconomyNotice n; n.kind = st::Economy::construct_level; n.value = value; n.base = base; n.planet = std::move(planet); n.object_owner = owner;
    return n;
}

void construct_events() {
    st::Config config;
    config.relations[{1, 2}] = st::Config::Relation::enemy;
    auto e = engine({sub("S.xml", 1, {
        counting("ONE", "STORY_CONSTRUCT", {{"Type_One"}}),
        counting("THREE", "STORY_CONSTRUCT", {{"Type_Two", "Type_Three"}, {"3", "1"}}),
        counting("MINE", "STORY_CONSTRUCT", {{"Type_Four"}, {}, {"FILTER_FRIENDLY_ONLY"}}),
        counting("FOES", "STORY_CONSTRUCT", {{"Type_Four"}, {}, {"FILTER_ENEMY_ONLY"}}),
        counting("BASE2", "STORY_CONSTRUCT_LEVEL", {{"alpha"}, {"2"}, {"SPACE"}}),
        counting("ANYBASE", "STORY_CONSTRUCT_LEVEL", {}),
        counting("GROUNDBASE", "STORY_CONSTRUCT_LEVEL", {{}, {"1"}, {"GROUND"}}),
    })}, config);
    expect(e.unavailable().size() == 1 && e.unavailable()[0].event == "GROUNDBASE" && e.unavailable()[0].reason.find("ground base level") != std::string::npos,
        "a construct-level event that names a ground base is listed as cannot-fire");
    take(e.notify(std::nullopt, built("type_one", 2)));
    expect(names(e.take_triggered()) == (Names{"ONE"}), "WST-33 a built type matches case-insensitively, whoever built it");
    take(e.notify(std::nullopt, built("Type_Two", 1))); take(e.notify(std::nullopt, built("Type_Two", 1)));
    expect(!e.triggered("S.xml", "THREE") && e.state().counters.size() == 1 && e.state().counters[0].values == (std::vector<std::int64_t>{1, 1}),
        "the count of the matched type runs down from its own entry");
    take(e.notify(std::nullopt, built("Type_Three", 2)));
    expect(names(e.take_triggered()) == (Names{"THREE"}), "the second type has a count of one and fires the event on its own");
    take(e.notify(std::nullopt, built("Type_Four", 2)));
    expect(names(e.take_triggered()) == (Names{"FOES"}), "the owner filter is relative to the plot's owner: the enemy build fires only the enemy filter");
    take(e.notify(std::nullopt, built("Type_Four", 1)));
    expect(names(e.take_triggered()) == (Names{"MINE"}), "and the own build the friendly one");
    take(e.notify(1, level(1, st::BaseKind::space, 1)));
    expect(names(e.take_triggered()) == (Names{"ANYBASE"}), "a base level below the event's level does not fire it; an event with no limits fires at any level");
    take(e.notify(1, level(2, st::BaseKind::ground, 1)));
    expect(names(e.take_triggered()) == (Names{"GROUNDBASE"}), "a ground base is not a space base: only the event that names a ground base fires (the session never sends one yet)");
    take(e.notify(1, level(3, st::BaseKind::space, 1, "beta")));
    expect(e.take_triggered().empty(), "the planet list is read");
    take(e.notify(2, level(2, st::BaseKind::space, 2)));
    expect(e.take_triggered().empty(), "a level notice that names a player never reaches another player's plots");
    take(e.notify(1, level(2, st::BaseKind::space, 1)));
    expect(names(e.take_triggered()) == (Names{"BASE2"}), "a level at or above the event's level at a listed planet fires it");
}

// #2044 / WST-58: the story dialog of a triggered event becomes a holocron goal of the plot's owner, once. The summary
// request is the unhashed presentation output; the goal rows ride the checkpoint but not the state digest.
std::vector<st::SubPlotDef> holocron_plots() {
    auto intro = make("INTRO", "STORY_ELAPSED", {"1"});
    intro.dialog = "Dialog_Intro"; intro.chapter = 4; intro.tag = "Intro_Tag"; intro.variable = "Var";
    auto hint = make("HINT", "STORY_ELAPSED", {"2"}); // no tag, quiet: the button-flash path
    hint.dialog = "Dialog_Hint"; hint.chapter = 1; hint.popup = false;
    hint.tag = "Incoming_Hint"; hint.authored_tag_present = false; // incoming override, no authored Story_Tag
    return {sub("H.xml", 1, {intro, hint})};
}
void holocron_goals() {
    const auto setup = session_setup();
    const auto ids = cp::game_identities(setup);
    const auto install_holocron = [](eawr::campaign::GameSession& game) { return game.install_story({}, {{1, "h.xml"}}, holocron_plots()); };
    std::string baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(eawr::campaign::GameSession::create(setup));
        take(install_holocron(game));
        std::string signature;
        std::size_t opens{}, intro_rows{}, flashes{};
        std::uint64_t opened_at{};
        for (int i = 0; i < 100; ++i) {
            const auto tick = take(game.step(executor));
            for (const auto& event : tick.story_presentation) {
                expect(event.owner == 1, "WST-58 the goal belongs to the plot's owner");
                if (const auto* goal = std::get_if<st::GoalChanged>(&event.payload)) {
                    signature += "G:" + goal->tag + ";";
                    if (goal->tag == "Intro_Tag") ++intro_rows;
                } else if (const auto* summary = std::get_if<st::SummaryRequest>(&event.payload)) {
                    signature += "S:" + std::to_string(static_cast<int>(summary->action)) + ":" + summary->tag + ":" + std::to_string(summary->chapter) + ";";
                    if (summary->action == st::SummaryAction::open) { ++opens; opened_at = tick.completed_tick; }
                    if (summary->action == st::SummaryAction::refresh_or_flash) ++flashes;
                }
            }
        }
        expect(opens == 1 && intro_rows == 1, "WST-58 the intro goal appears once and opens its summary once (popup is the default)");
        expect(flashes == 1, "WST-70 a quiet plot defers refresh-or-flash to the actual local summary state");
        // WST-70 advisor/history and mission-sound admission is an unwired goals_policy_seam, not an absence contract.
        expect(opened_at > 25 && opened_at < 40, "the one-second elapsed event opens the summary after about thirty galactic frames");
        const auto* goals = game.story_goals(1);
        expect(goals && goals->goals.size() == 2 && goals->goals[1].tag == "Intro_Tag" && goals->goals[1].chapter == 4
            && !goals->goals[1].completed && !goals->goals[1].remove_after_viewed,
            "WST-58 the tagged intro goal is kept");
        expect(goals && goals->goals.size() == 2 && goals->goals[0].tag == "Incoming_Hint" && goals->goals[0].remove_after_viewed,
            "WST-58 the new incoming-only goal is first and goes once viewed");
        expect(!game.story_goals(2), "the other owner has no goals");
        auto copy = take(eawr::campaign::GameSession::create(setup));
        take(install_holocron(copy));
        for (int i = 0; i < 100; ++i) static_cast<void>(take(copy.step(executor)));
        expect(copy.state_sha256() == game.state_sha256() && copy.story_goals(1) && copy.story_goals(1)->goals == goals->goals,
            "the same plots and ticks give the same digest and goals");

        // WST-60..62 through the game checkpoint: the goals ride the save and a load shows nothing again.
        cp::GameLoadOptions options;
        options.configure_session = [&](eawr::campaign::GameSession& fresh) { return install_holocron(fresh); };
        const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto restored = take(cp::load_game_checkpoint(saved, ids, options, executor));
        const auto* back = restored.session.story_goals(1);
        expect(back && goals && back->goals == goals->goals && back->objectives_visible == goals->objectives_visible,
            "the goals are restored with the checkpoint");
        expect(take(cp::save_game_checkpoint(restored.session, ids)) == saved, "the restored session saves the same bytes");
        std::size_t again{};
        std::vector<std::string> republished;
        for (int i = 0; i < 40; ++i)
            for (const auto& row : take(restored.session.step(executor)).story_presentation) {
                if (std::holds_alternative<st::SummaryRequest>(row.payload)) ++again;
                else if (const auto* goal = std::get_if<st::GoalChanged>(&row.payload); goal && goal->value) republished.push_back(goal->tag);
            }
        expect(again == 0, "WST-58 a loaded game does not show the intro again");
        expect(republished == (std::vector<std::string>{"Intro_Tag", "Incoming_Hint"}),
            "a loaded game republishes its goal rows to the HUD, oldest first so the head insertion restores the saved order (legacy #6681)");
        if (workers == 1) baseline = signature;
        else expect(baseline == signature, "1/2/4/8 workers publish the same presentation requests in the same order");
    }
}

// #236 PR B (WST-47/65/66/67): the objective rewards change the plot owner's list, an unreadable one is refused by name,
// the numbering counter and the list ride the checkpoint (owner format 7), and SCREEN_TEXT reaches the HUD as text.
std::vector<st::SubPlotDef> objective_plots() {
    const auto note = [](const char* name, const char* trigger, const char* reward, std::vector<std::string> first, std::vector<std::string> second = {}) {
        auto e = make(name, "STORY_CLICK_GUI", {trigger}); e.perpetual = true;
        e.reward_type = reward; e.reward_params[0] = std::move(first); e.reward_params[1] = std::move(second);
        return e;
    };
    return {sub("O.xml", 1, {note("ADD1", "b_1", "ADD_OBJECTIVE", {"TEXT_ONE"}), note("ADD2", "b_2", "ADD_OBJECTIVE", {" TEXT_TWO "}),
        note("ADD1B", "b_3", "ADD_OBJECTIVE", {"TEXT_ONE"}), note("DONE", "b_4", "OBJECTIVE_COMPLETE", {"TEXT_ONE"}),
        note("FAIL", "b_5", "OBJECTIVE_FAILED", {"TEXT_TWO"}), note("UPD", "b_6", "UPDATE_OBJECTIVE", {"TEXT_TWO"}, {"TEXT_THREE"}),
        note("REM", "b_7", "REMOVE_OBJECTIVE", {"TEXT_ONE"}), note("HIDE", "b_8", "ENABLE_OBJECTIVE_DISPLAY", {"0"}),
        note("SHOW", "b_9", "ENABLE_OBJECTIVE_DISPLAY", {}), note("ADD_BAD", "b_a", "ADD_OBJECTIVE", {}),
        note("UPD_BAD", "b_b", "UPDATE_OBJECTIVE", {"TEXT_ONE"}), note("DISPLAY_BAD", "b_c", "ENABLE_OBJECTIVE_DISPLAY", {"x"}),
        note("MISS", "b_d", "OBJECTIVE_COMPLETE", {"TEXT_NONE"}), note("ALL", "b_e", "REMOVE_ALL_OBJECTIVES", {}),
        note("TEXT", "b_f", "SCREEN_TEXT", {"TEXT_HINT"}, {"7"}), note("TEXT_BAD", "b_g", "SCREEN_TEXT", {})})};
}
void story_objectives() {
    const auto setup = session_setup();
    const auto ids = cp::game_identities(setup);
    const auto install = [](eawr::campaign::GameSession& game) { return game.install_story({}, {{1, "o.xml"}}, objective_plots()); };
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(eawr::campaign::GameSession::create(setup));
        take(install(game));
        std::uint64_t sequence{};
        std::map<std::string, std::string> outcome;
        std::vector<std::string> text;
        const auto click = [&](const char* control) {
            take(game.submit(gc::GameCommand{{game.completed_tick(), {1}, ++sequence}, gc::GuiClick{control}}));
            const auto tick = take(game.step(executor));
            for (const auto& row : tick.story_triggers) outcome[row.event] = row.outcome;
            for (const auto& row : tick.story_presentation) {
                const auto* request = std::get_if<st::ConsumerRequest>(&row.payload);
                if (request && request->consumer == st::Consumer::text && row.owner == 1) {
                    std::string line = request->reward_name;
                    for (const auto& p : request->parameters) line += ":" + p;
                    text.push_back(std::move(line));
                }
            }
        };
        const auto ids_now = [&] {
            std::string out;
            if (const auto* goals = game.story_goals(1)) for (const auto& o : goals->objectives)
                out += o.text_id + "#" + std::to_string(o.number) + (o.status == st::ObjectiveStatus::complete ? "+" : o.status == st::ObjectiveStatus::failed ? "-" : "") + ";";
            return out;
        };
        click("b_1"); click("b_2"); click("b_3");
        expect(ids_now() == "TEXT_ONE#1;TEXT_TWO#2;TEXT_ONE#3;", "ADD_OBJECTIVE appends in order with the counter's numbers, duplicates admitted, the name trimmed");
        click("b_4"); click("b_5");
        expect(ids_now() == "TEXT_ONE#1+;TEXT_TWO#2-;TEXT_ONE#3;", "complete and fail hit the first match only");
        click("b_6");
        expect(ids_now() == "TEXT_ONE#1+;TEXT_THREE#2;TEXT_ONE#3;", "UPDATE_OBJECTIVE renames in place and resets the status");
        click("b_7");
        expect(ids_now() == "TEXT_THREE#2;TEXT_ONE#3;", "REMOVE_OBJECTIVE removes the first match");
        click("b_8");
        expect(game.story_goals(1) && !game.story_goals(1)->objectives_visible, "ENABLE_OBJECTIVE_DISPLAY 0 hides the list");
        click("b_9");
        expect(game.story_goals(1)->objectives_visible, "an empty parameter shows it again");
        click("b_a"); click("b_b"); click("b_c"); click("b_d");
        expect(outcome["ADD_BAD"].starts_with("refused: ADD_OBJECTIVE needs a text identity")
            && outcome["UPD_BAD"].starts_with("refused: UPDATE_OBJECTIVE needs the new identity")
            && outcome["DISPLAY_BAD"].starts_with("refused: ENABLE_OBJECTIVE_DISPLAY needs an integer") && outcome["MISS"] == "applied: no objective matched",
            "an unreadable objective reward is refused by name; a missing match changes nothing");
        expect(ids_now() == "TEXT_THREE#2;TEXT_ONE#3;", "the refused rewards changed nothing");
        click("b_f"); click("b_g");
        expect(text == std::vector<std::string>{"SCREEN_TEXT:TEXT_HINT:7:::::"} && outcome["TEXT_BAD"].starts_with("refused: SCREEN_TEXT needs a text id"),
            "SCREEN_TEXT reaches the seat with its seven positional parameters; one without a text id is refused");
        // The list, the counter and the display flag ride the checkpoint (owner format 7), and a load shows nothing again.
        cp::GameLoadOptions options;
        options.configure_session = [&](eawr::campaign::GameSession& fresh) { return install(fresh); };
        const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto restored = take(cp::load_game_checkpoint(saved, ids, options, executor));
        const auto* back = restored.session.story_goals(1);
        expect(back && back->objectives == game.story_goals(1)->objectives && back->next_number == 4 && back->objectives_visible,
            "the objective list and the numbering counter are restored with the checkpoint");
        expect(take(cp::save_game_checkpoint(restored.session, ids)) == saved, "the restored session saves the same bytes");
        {
            const auto first = take(restored.session.step(executor));
            std::vector<std::string> republished;
            for (const auto& row : first.story_presentation)
                if (const auto* changed = std::get_if<st::ObjectiveChanged>(&row.payload); changed && row.owner == 1 && changed->value)
                    republished.push_back(changed->value->text_id + "#" + std::to_string(changed->value->number));
            expect(republished == (std::vector<std::string>{"TEXT_THREE#2", "TEXT_ONE#3"}), "a loaded game republishes the restored objective list to the HUD");
        }
        click("b_e");
        expect(ids_now().empty() && game.story_goals(1)->next_number == 4, "REMOVE_ALL_OBJECTIVES clears the list and keeps the counter");
        baseline.push_back(game.state_sha256());
        expect(baseline.back() == baseline.front(), "1/2/4/8 workers keep equal digests across the objective rewards");
    }
}

// #683 part 2: the apply loop of the session. Rewards run once per step after the service, in trigger order; a reward
// that changes the tech level triggers a further event that runs in the same step; refusals are published once.
void published_story_fault() {
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool throws : {false, true}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(eawr::campaign::GameSession::create(session_setup()));
        take(game.install_story({}, {{1, "fault.xml"}}, {sub("fault.xml", 1, {
            make("A", "STORY_ELAPSED", {"0"}), trig("FIRST", {{"A"}}, "SPAWN_HERO", {"First"}),
            trig("SECOND", {{"A"}}, "SPAWN_HERO", {"Second"})})}));
        Names order;
        game.set_story_rewards([&](gc::GalacticSession&, const st::Triggered& record, const eawr::sim::PartitionExecutor&)
            -> eawr::core::Result<eawr::campaign::GameSession::StoryRewardOutcome> {
            order.push_back(record.event);
            if (throws) throw std::runtime_error("injected published story reward exception");
            eawr::core::Diagnostic error; error.code = "INJECTED-PUBLISHED-STORY";
            return eawr::core::Result<eawr::campaign::GameSession::StoryRewardOutcome>::failure(std::move(error));
        });
        bool refused{};
        for (unsigned frame = 0; frame < 40 && !refused; ++frame) {
            try { refused = !game.step(executor); } catch (const std::runtime_error&) { refused = true; }
        }
        expect(refused && order == Names{"FIRST"} && game.galaxy().native_post_publication_faults() == 1,
            "published story reward fault preserves first-callback order and refusal without rollback/retry or a following reward");
    }
}

// #7826: a story's FORCE_CLICK_GUI on the pause control is the session's own clock change. Two peers fed the same click
// toggle at the same tick (paused and running, so both directions), end in the expected pause state and keep equal digests
// on 1/2/4/8 workers; the HUD is handed no press for it; a save taken right after the toggle loads back with the same
// pause depth (the load prologue carries the saved depth, a story-caused pause included).
void story_pause_toggle() {
    const auto plots = [] {
        auto a = make("PAUSE_TOGGLE", "STORY_CLICK_GUI", {"b_go"}); a.perpetual = true;
        a.reward_type = "FORCE_CLICK_GUI"; a.reward_params[0] = {"B_Play_Pause"}; // the control's name matches in any case
        return std::vector<st::SubPlotDef>{sub("P.xml", 1, {a})};
    };
    const auto depth = [](eawr::campaign::GameSession& game) { return game.galaxy().clock().pause_depth; };
    // Both transports: the multiplayer flag takes the shared human hold path (protected system pause), the other the nested counter.
    for (const bool multiplayer : {false, true}) {
    const auto setup = [multiplayer] { auto s = session_setup(); s.networked_transport = multiplayer; return s; };
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::array<eawr::campaign::GameSession, 2> peers{take(eawr::campaign::GameSession::create(setup())),
                                                          take(eawr::campaign::GameSession::create(setup()))};
        for (auto& peer : peers) take(peer.install_story({}, {{1, "p.xml"}}, plots()));
        std::uint64_t sequence{};
        std::vector<std::string> digests;
        const auto click_both = [&](const std::string_view label, const std::uint32_t expected) {
            ++sequence;
            for (auto& peer : peers) take(peer.submit(gc::GameCommand{{peer.completed_tick(), {1}, sequence}, gc::GuiClick{"b_go"}}));
            std::array<eawr::campaign::GameTick, 2> ticks{take(peers[0].step(executor)), take(peers[1].step(executor))};
            for (std::size_t i = 0; i < 2; ++i) {
                expect(depth(peers[i]) == expected, std::string(label) + ": peer " + std::to_string(i) + " pause depth");
                const bool hud_press = std::any_of(ticks[i].story_presentation.begin(), ticks[i].story_presentation.end(), [](const auto& row) {
                    const auto* request = std::get_if<st::ConsumerRequest>(&row.payload);
                    return request && request->reward_name == "FORCE_CLICK_GUI";
                });
                expect(!hud_press, std::string(label) + ": the HUD is handed no press for the pause control");
                expect(ticks[i].story_triggers.size() == 1 && ticks[i].story_triggers.front().outcome == "applied", std::string(label) + ": an applied reward");
            }
            expect(peers[0].completed_tick() == peers[1].completed_tick() && peers[0].state_sha256() == peers[1].state_sha256(),
                std::string(label) + ": both peers at the same tick with equal digests");
            digests.push_back(peers[0].state_sha256());
        };
        click_both("running -> paused", 1);
        // The pause holds across plain ticks (no story, no command).
        for (auto& peer : peers) take(peer.step(executor));
        expect(depth(peers[0]) == 1 && depth(peers[1]) == 1, "paused stays paused");
        // A save right after the toggle (paused by the story) loads back paused.
        const auto saved = take(peers[0].save_checkpoint_state());
        auto loaded = take(eawr::campaign::GameSession::create(setup()));
        take(loaded.install_story({}, {{1, "p.xml"}}, plots())); // a checkpoint loads into a campaign with the same plots
        take(loaded.load_checkpoint_state(saved));
        expect(depth(loaded) == 1, "a save taken while a story's pause holds loads back with the same pause depth");
        click_both("paused -> running", 0);
        const auto saved_running = take(peers[0].save_checkpoint_state());
        auto loaded_running = take(eawr::campaign::GameSession::create(setup()));
        take(loaded_running.install_story({}, {{1, "p.xml"}}, plots())); // a checkpoint loads into a campaign with the same plots
        take(loaded_running.load_checkpoint_state(saved_running));
        expect(depth(loaded_running) == 0, "a save taken after the story's resume loads back running");
        if (baseline.empty()) baseline = digests;
        expect(digests == baseline, "the toggle digests are equal on " + std::to_string(workers) + " workers (multiplayer " + std::to_string(multiplayer) + ")");
    }
    }
}

// #5857: FORCE_CLICK_GUI becomes a presentation request for the plot owner's seat, and the per-tick bound drops the
// clicks a self-forcing loop would pile up, deterministically and without ending the match.
void forced_clicks() {
    const auto plots = [] {
        auto a = make("A", "STORY_CLICK_GUI", {"b_first"}); a.perpetual = true;
        a.reward_type = "FORCE_CLICK_GUI"; a.reward_params[0] = {"b_second"};
        auto b = make("B", "STORY_CLICK_GUI", {"b_second"}); b.perpetual = true;
        b.reward_type = "FORCE_CLICK_GUI"; b.reward_params[0] = {"b_first", "ignored"};
        auto bad = make("BAD", "STORY_CLICK_GUI", {"b_bad"}); bad.perpetual = true;
        bad.reward_type = "FORCE_CLICK_GUI";
        return std::vector<st::SubPlotDef>{sub("F.xml", 1, {a, b, bad})};
    };
    const auto forced_names = [](const eawr::campaign::GameTick& tick) {
        Names out;
        for (const auto& row : tick.story_presentation) {
            const auto* request = std::get_if<st::ConsumerRequest>(&row.payload);
            if (request && request->consumer == st::Consumer::gui && request->reward_name == "FORCE_CLICK_GUI" && row.owner == 1)
                out.push_back(request->parameters.front());
        }
        return out;
    };
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(eawr::campaign::GameSession::create(session_setup()));
        take(game.install_story({}, {{1, "f.xml"}}, plots()));
        std::uint64_t sequence{};
        const auto click_at = [&](std::uint64_t tick, const char* name) {
            take(game.submit(gc::GameCommand{{tick, {1}, ++sequence}, gc::GuiClick{name}}));
        };
        // Printed only when a check below fails, so a red run says what the step published.
        const auto trace = [&](const char* label, const eawr::campaign::GameTick& t, const bool failed) {
            if (!failed) return;
            std::cerr << "forced_clicks " << label << ": workers=" << workers << " tick=" << t.completed_tick << " triggers=" << t.story_triggers.size()
                << " presentation=" << t.story_presentation.size() << " notices=" << t.story_notices.size() << " diagnostics=" << t.diagnostics.size() << std::endl;
            for (const auto& d : t.diagnostics) std::cerr << "  diag " << d.code << ": " << d.message << std::endl;
            for (const auto& r : t.story_triggers) std::cerr << "  trigger " << r.event << " -> " << r.outcome << std::endl;
        };
        // One click, one forced request, outcome "applied", nothing hashed or recorded for the presentation.
        click_at(game.completed_tick(), "B_FIRST");
        auto tick = take(game.step(executor));
        trace("first", tick, forced_names(tick) != Names{"b_second"} || tick.story_triggers.size() != 1);
        expect(forced_names(tick) == Names{"b_second"}, "a click on a listened component forces the component its reward names");
        expect(tick.story_triggers.size() == 1 && tick.story_triggers.front().outcome == "applied", "the forced click is an applied reward");
        // A reward without a component is refused by name; the match goes on.
        click_at(game.completed_tick(), "b_bad");
        tick = take(game.step(executor));
        trace("bad", tick, !forced_names(tick).empty() || tick.story_triggers.size() != 1);
        expect(forced_names(tick).empty() && tick.story_triggers.size() == 1
            && tick.story_triggers.front().outcome.starts_with("refused: FORCE_CLICK_GUI needs one component name"), "a reward without a component is refused");
        // The bound: twelve clicks in one tick, eight reach the story, four are dropped with a diagnostic.
        const auto at = game.completed_tick();
        for (int i = 0; i < 12; ++i) click_at(at, "b_first");
        tick = take(game.step(executor));
        const auto dropped = std::count_if(tick.diagnostics.begin(), tick.diagnostics.end(),
            [](const auto& d) { return d.message.find("GUI click dropped") != std::string::npos; });
        expect(forced_names(tick).size() == static_cast<std::size_t>(gc::max_gui_clicks_per_tick) && dropped == static_cast<std::ptrdiff_t>(12 - gc::max_gui_clicks_per_tick),
            "the per-tick bound lets eight clicks through and drops the rest with a diagnostic");
        const auto after = take(game.step(executor));
        expect(forced_names(after).empty(), "the bound is per tick: the next tick starts clean");
        // The following tick admits clicks again (the counter restarts).
        click_at(game.completed_tick(), "b_second");
        expect(forced_names(take(game.step(executor))) == Names{"b_first"}, "a later tick admits clicks again");
        baseline.push_back(game.state_sha256());
        expect(baseline.back() == baseline.front(), "1/2/4/8 workers keep equal digests across the click loop");
    }
}

// #236 (WST-49/75): FLASH_GUI, LOCK_CONTROLS and SCROLL_LOCK become presentation requests for the plot owner's seat, a
// reward that cannot be read is refused by name without ending the match, and the digest is equal at 1/2/4/8 workers.
void story_hud_requests() {
    const auto plots = [](bool with_rewards) {
        // Each positional reward parameter is its own token list: parameter 1 in reward_params[0], parameter 2 in [1].
        const auto note = [&](const char* name, const char* trigger, const char* reward, std::vector<std::string> params, std::vector<std::string> second = {}) {
            auto e = make(name, "STORY_CLICK_GUI", {trigger}); e.perpetual = true;
            if (with_rewards) { e.reward_type = reward; e.reward_params[0] = std::move(params); e.reward_params[1] = std::move(second); }
            return e;
        };
        return std::vector<st::SubPlotDef>{sub("H.xml", 1, {note("FLASH", "b_a", "FLASH_GUI", {" B_Target "}, {"aux"}), note("FLASH_BAD", "b_b", "FLASH_GUI", {}),
            note("LOCK", "b_c", "LOCK_CONTROLS", {"7"}), note("UNLOCK", "b_d", "LOCK_CONTROLS", {"0"}), note("LOCK_BAD", "b_e", "LOCK_CONTROLS", {"x"}),
            note("SCROLL", "b_f", "SCROLL_LOCK", {"1"}), note("SCROLL_BAD", "b_g", "SCROLL_LOCK", {})})};
    };
    const auto requests = [](const eawr::campaign::GameTick& tick) {
        std::vector<std::string> out;
        for (const auto& row : tick.story_presentation) {
            const auto* request = std::get_if<st::ConsumerRequest>(&row.payload);
            if (!request || row.owner != 1) continue;
            std::string text = request->reward_name + (request->consumer == st::Consumer::gui ? "/gui" : "/control");
            for (const auto& p : request->parameters) text += ":" + p;
            out.push_back(std::move(text));
        }
        return out;
    };
    std::vector<std::string> digests, control_digests;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        for (const bool with_rewards : {true, false}) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            auto game = take(eawr::campaign::GameSession::create(session_setup()));
            take(game.install_story({}, {{1, "h.xml"}}, plots(with_rewards)));
            std::uint64_t sequence{};
            std::vector<std::string> seen, outcomes;
            for (const char* control : {"b_a", "b_b", "b_c", "b_d", "b_e", "b_f", "b_g"}) {
                take(game.submit(gc::GameCommand{{game.completed_tick(), {1}, ++sequence}, gc::GuiClick{control}}));
                const auto tick = take(game.step(executor));
                for (auto& row : requests(tick)) seen.push_back(std::move(row));
                for (const auto& row : tick.story_triggers) outcomes.push_back(row.outcome);
            }
            if (with_rewards) {
                expect(seen == (std::vector<std::string>{"FLASH_GUI/gui:B_Target:aux", "LOCK_CONTROLS/control:1", "LOCK_CONTROLS/control:0", "SCROLL_LOCK/control:1"}),
                    "FLASH_GUI, LOCK_CONTROLS and SCROLL_LOCK reach the plot owner's seat with a trimmed name and a normalised lock flag");
                expect(outcomes.size() == 7 && outcomes[1].starts_with("refused: FLASH_GUI needs one component name")
                    && outcomes[4].starts_with("refused: LOCK_CONTROLS needs an integer") && outcomes[6].starts_with("refused: SCROLL_LOCK needs an integer"),
                    "an unreadable HUD reward is refused by name and the match goes on");
            } else {
                expect(seen.empty(), "the control run publishes no HUD request");
            }
            (with_rewards ? digests : control_digests).push_back(game.state_sha256());
        }
    }
    for (const auto& digest : digests) expect(digest == digests.front(), "1/2/4/8 workers keep equal digests across the HUD requests");
    for (const auto& digest : control_digests) expect(digest == control_digests.front(), "1/2/4/8 workers keep equal digests in the control run");
}

// #236 review: a lock never outlives its plot. The plot that set LOCK_CONTROLS and SCROLL_LOCK is suspended; the seat gets
// both releases once. An explicit unlock clears the claim, so a later plot end publishes nothing for it.
void story_lock_release() {
    const auto lock_plot = [] {
        const auto note = [](const char* name, const char* trigger, const char* reward, const char* value) {
            auto e = make(name, "STORY_CLICK_GUI", {trigger}); e.perpetual = true;
            e.reward_type = reward; e.reward_params[0] = {value};
            return e;
        };
        return std::vector<st::SubPlotDef>{sub("H.xml", 1, {note("LOCK", "b_a", "LOCK_CONTROLS", "1"), note("SCROLL", "b_b", "SCROLL_LOCK", "1"),
            note("UNLOCK", "b_c", "LOCK_CONTROLS", "0")})};
    };
    const auto releases = [](const eawr::campaign::GameTick& tick) {
        std::vector<std::string> out;
        for (const auto& row : tick.story_presentation) {
            const auto* request = std::get_if<st::ConsumerRequest>(&row.payload);
            if (request && request->consumer == st::Consumer::control && row.owner == 1 && request->parameters == Names{"0"}) out.push_back(request->reward_name);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    for (const bool unlock_first : {false, true}) {
        eawr::platform::ThreadWorkerAdapter executor(1);
        auto game = take(eawr::campaign::GameSession::create(session_setup()));
        take(game.install_story({}, {{1, "h.xml"}}, lock_plot()));
        std::uint64_t sequence{};
        const auto click_step = [&](const char* control) {
            take(game.submit(gc::GameCommand{{game.completed_tick(), {1}, ++sequence}, gc::GuiClick{control}}));
            return take(game.step(executor));
        };
        click_step("b_a"); click_step("b_b");
        if (unlock_first) expect(releases(click_step("b_c")) == Names{"LOCK_CONTROLS"}, "an explicit unlock publishes its release");
        st::PlotCommand suspend; suspend.op = st::PlotCommand::Op::suspend; suspend.subplot = "H.xml";
        take(game.apply_story_plot(suspend));
        const auto ended = take(game.step(executor));
        expect(releases(ended) == (unlock_first ? Names{"SCROLL_LOCK"} : Names{"LOCK_CONTROLS", "SCROLL_LOCK"}),
            "a suspended plot releases the locks it set, and only those still held");
        expect(releases(take(game.step(executor))).empty(), "the release is published once");
    }
}

// legacy #6681 / WST-47/58/68: STORY_GOAL_COMPLETED keeps a holocron goal and marks it completed, REMOVE_STORY_GOAL deletes it;
// a tag that matches nothing changes nothing and an empty tag is refused by name. Both ride the checkpoint (the goal rows are in the
// owner tail already) and a loaded game republishes the rows that are left, without a summary request.
std::vector<st::SubPlotDef> goal_reward_plots() {
    const auto note = [](const char* name, const char* trigger, const char* reward, std::vector<std::string> first) {
        auto e = make(name, "STORY_CLICK_GUI", {trigger}); e.perpetual = true;
        e.reward_type = reward; e.reward_params[0] = std::move(first);
        return e;
    };
    auto first = make("GOAL_A", "STORY_ELAPSED", {"1"});
    first.dialog = "Dialog_A"; first.chapter = 1; first.tag = "Tag_A"; first.popup = false;
    auto second = make("GOAL_B", "STORY_ELAPSED", {"1"});
    second.dialog = "Dialog_B"; second.chapter = 2; second.tag = "Tag_B"; second.popup = false;
    return {sub("G.xml", 1, {first, second, note("DONE_A", "b_done", "STORY_GOAL_COMPLETED", {" Tag_A "}),
        note("DROP_B", "b_drop", "REMOVE_STORY_GOAL", {"Tag_B"}), note("MISS", "b_miss", "STORY_GOAL_COMPLETED", {"Tag_None"}),
        note("MISS_DROP", "b_miss_drop", "REMOVE_STORY_GOAL", {"Tag_None"}), note("EMPTY", "b_empty", "STORY_GOAL_COMPLETED", {}),
        note("EMPTY_DROP", "b_empty_drop", "REMOVE_STORY_GOAL", {})})};
}
void story_goal_rewards() {
    const auto setup = session_setup();
    const auto ids = cp::game_identities(setup);
    const auto install = [](eawr::campaign::GameSession& game) { return game.install_story({}, {{1, "g.xml"}}, goal_reward_plots()); };
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(eawr::campaign::GameSession::create(setup));
        take(install(game));
        for (int i = 0; i < 60; ++i) static_cast<void>(take(game.step(executor)));
        const auto* goals = game.story_goals(1);
        expect(goals && goals->goals.size() == 2 && !goals->goals[0].completed && !goals->goals[1].completed, "both story goals were added, neither completed");
        std::uint64_t sequence{};
        std::map<std::string, std::string> outcome;
        std::vector<std::string> published; // "tag+" completed, "tag-" removed
        const auto click = [&](const char* control) {
            take(game.submit(gc::GameCommand{{game.completed_tick(), {1}, ++sequence}, gc::GuiClick{control}}));
            const auto tick = take(game.step(executor));
            for (const auto& row : tick.story_triggers) outcome[row.event] = row.outcome;
            for (const auto& row : tick.story_presentation)
                if (const auto* goal = std::get_if<st::GoalChanged>(&row.payload); goal && row.owner == 1)
                    published.push_back(goal->tag + (goal->value ? (goal->value->completed ? "+" : "") : "-"));
        };
        const auto state = [&] {
            std::string out;
            if (const auto* now = game.story_goals(1)) for (const auto& goal : now->goals) out += goal.tag + (goal.completed ? "+" : "") + ";";
            return out;
        };
        click("b_done");
        expect(state() == "Tag_B;Tag_A+;" || state() == "Tag_A+;Tag_B;", "STORY_GOAL_COMPLETED keeps the goal and marks it completed (the name is trimmed)");
        expect(published == std::vector<std::string>{"Tag_A+"} && outcome["DONE_A"] == "applied", "the completion reaches the seat as the goal's new row");
        click("b_miss"); click("b_miss_drop"); click("b_empty"); click("b_empty_drop");
        expect(outcome["MISS"] == "applied: no goal matched" && outcome["MISS_DROP"] == "applied: no goal matched"
            && outcome["EMPTY"].starts_with("refused: STORY_GOAL_COMPLETED needs a goal tag") && outcome["EMPTY_DROP"].starts_with("refused: REMOVE_STORY_GOAL needs a goal tag"),
            "a tag that matches nothing changes nothing; an empty tag is refused by name");
        expect(published.size() == 1, "and none of them published a goal row");
        // The completed goal and its place ride the checkpoint (owner format 7: the goal rows were already in the owner tail).
        cp::GameLoadOptions options;
        options.configure_session = [&](eawr::campaign::GameSession& fresh) { return install(fresh); };
        const auto saved = take(cp::save_game_checkpoint(game, ids));
        auto restored = take(cp::load_game_checkpoint(saved, ids, options, executor));
        const auto* back = restored.session.story_goals(1);
        expect(back && back->goals == game.story_goals(1)->goals, "the completed goal is restored with the checkpoint");
        expect(take(cp::save_game_checkpoint(restored.session, ids)) == saved, "the restored session saves the same bytes");
        {
            const auto first = take(restored.session.step(executor));
            std::vector<std::string> rows;
            std::size_t requests{};
            for (const auto& row : first.story_presentation) {
                if (std::holds_alternative<st::SummaryRequest>(row.payload)) ++requests;
                else if (const auto* goal = std::get_if<st::GoalChanged>(&row.payload); goal && goal->value && row.owner == 1)
                    rows.push_back(goal->tag + (goal->value->completed ? "+" : ""));
            }
            expect(rows.size() == 2 && requests == 0, "a loaded game republishes both goal rows, completed one included, and opens no summary");
        }
        click("b_drop");
        expect(state() == "Tag_A+;" && published.back() == "Tag_B-" && outcome["DROP_B"] == "applied", "REMOVE_STORY_GOAL deletes the goal and publishes its removal");
        click("b_drop");
        expect(outcome["DROP_B"] == "applied: no goal matched" && state() == "Tag_A+;", "removing it again changes nothing");
        baseline.push_back(game.state_sha256());
        expect(baseline.back() == baseline.front(), "1/2/4/8 workers keep equal digests across the goal rewards");
    }
}

// #5922: a click on any command-bar control reaches the plots that name it, including a production card such as BFC's
// b_Create_26: the listener list excludes no name and the click command triggers the event.
void click_every_control() {
    auto e = engine({sub("P.xml", 1, {make("BFC", "STORY_CLICK_GUI", {"b_Create_26"}), make("QUEUE", "STORY_CLICK_GUI", {"b_queue_space_03"}),
        make("HELP", "STORY_CLICK_GUI", {"b_droid_help"})})});
    expect(e.click_listeners() == (Names{"B_CREATE_26", "B_DROID_HELP", "B_QUEUE_SPACE_03"}), "a production card, a queue slot and the help button are all listened to");
    eawr::platform::ThreadWorkerAdapter executor(1);
    auto game = take(eawr::campaign::GameSession::create(session_setup()));
    // An event with no reward and no dialog leaves no record, so each carries one (refused here: no provider is bound).
    const auto rewarded = [](const char* name, const char* control) {
        auto event = make(name, "STORY_CLICK_GUI", {control});
        event.reward_type = "CREDITS"; event.reward_params[0] = {"1"};
        return event;
    };
    take(game.install_story({}, {{1, "p.xml"}}, {sub("P.xml", 1, {rewarded("BFC", "b_Create_26"), rewarded("QUEUE", "b_queue_space_03")})}));
    std::uint64_t sequence{};
    Names fired;
    for (const char* control : {"B_CREATE_26", "b_queue_space_03", "b_create_01"}) {
        take(game.submit(gc::GameCommand{{game.completed_tick(), {1}, ++sequence}, gc::GuiClick{control}}));
        for (const auto& row : take(game.step(executor)).story_triggers) fired.push_back(row.event);
    }
    expect(fired == (Names{"BFC", "QUEUE"}), "a BFC production-card click reaches the story engine as STORY_CLICK_GUI, whatever the case, and an unnamed control does nothing");
}

void session_rewards() {
    const auto setup = session_setup();
    const auto plots = [] {
        return std::vector<st::SubPlotDef>{sub("R.xml", 1, {make("A", "STORY_ELAPSED", {"1"}), trig("GRANT", {{"A"}}, "SPAWN_HERO", {"Raise"}),
            trig("CASH", {{"A"}}, "CREDITS", {"50"}), trig("CASH2", {{"A"}}, "CREDITS", {"60"}),
            make("T5", "STORY_TECH_LEVEL", {"5"}), trig("FINALE", {{"T5"}}, "SPAWN_HERO", {"Finale"})})};
    };
    const auto note_count = [](const std::vector<eawr::core::Diagnostic>& rows, std::string_view text) {
        return std::count_if(rows.begin(), rows.end(), [&](const auto& d) { return d.code == "EAWR-STORY-0001" && d.message.find(text) != std::string::npos; });
    };
    for (const bool bound : {false, true}) {
        eawr::platform::ThreadWorkerAdapter executor(1);
        auto game = take(eawr::campaign::GameSession::create(setup));
        take(game.install_story({}, {{1, "r.xml"}}, plots()));
        Names order;
        if (bound) game.set_story_rewards([&](gc::GalacticSession& world, const st::Triggered& record, const eawr::sim::PartitionExecutor&) {
            using Result = eawr::core::Result<eawr::campaign::GameSession::StoryRewardOutcome>;
            order.push_back(record.event);
            if (record.event == "GRANT") take(world.set_technology({1, 5, true, true})); // notifies the engine inside the apply
            if (record.reward_type == "CREDITS") return Result::success({false, "no wallet"});
            return Result::success({true, {}});
        });
        std::vector<eawr::campaign::GameTick> fired;
        for (int i = 0; i < 40; ++i) {
            auto tick = take(game.step(executor));
            if (!tick.story_triggers.empty()) fired.push_back(std::move(tick));
        }
        expect(fired.size() == 1, "the elapsed event fires once");
        const auto& tick = fired.front();
        std::vector<std::string> outcomes;
        for (const auto& row : tick.story_triggers) outcomes.push_back(row.outcome);
        if (bound) {
            expect(names(tick.story_triggers) == Names{"GRANT", "CASH", "CASH2", "FINALE"} && order == names(tick.story_triggers),
                "rewards apply in trigger order; the level a reward sets triggers the next event in the same step");
            expect(outcomes == (std::vector<std::string>{"applied", "refused: no wallet", "refused: no wallet", "applied"}), "each record carries its outcome");
            expect(note_count(tick.diagnostics, "CREDITS not applied: no wallet") == 1, "a refusal is published once per reward type and reason, not per event");
        } else {
            expect(names(tick.story_triggers) == Names{"GRANT", "CASH", "CASH2"}, "without a provider the records are still published");
            expect(std::all_of(outcomes.begin(), outcomes.end(), [](const auto& o) { return o == "refused: no reward provider is bound"; }), "and refused by name");
            expect(note_count(tick.diagnostics, "SPAWN_HERO not applied: no reward provider is bound") == 1
                && note_count(tick.diagnostics, "CREDITS not applied: no reward provider is bound") == 1, "one note per reward type");
        }
    }
}
} // namespace

int case_galactic_story_engine_tests(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed-type-names") {
            const auto game = eawr::testing::environment("EAWR_EAW_GAME_ROOT");
            if (!game || game->empty()) { std::cout << "SKIP: set EAWR_EAW_GAME_ROOT\n"; return 77; }
            installed_type_names(*game, std::nullopt, "stock");
            const auto mod = eawr::testing::environment("EAWR_REMAKE_MOD_ROOT");
            if (mod && !mod->empty()) installed_type_names(*game, std::filesystem::path(*mod), "mod");
            return galactic_test::failures ? 1 : 0;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--installed-associations") {
            installed_campaign_routes(argv[2], argv[3]);
            return galactic_test::failures ? 1 : 0;
        }
        order_and_prerequisites(); elapsed_time(); flags(); tech_levels(); click_gui(); open_corruption(); click_listeners(); click_command_codec(); perpetual_and_branches(); sub_plots_and_elements(); lua_plot_commands();
        land_battle_seam(); cycles_and_tactical_triggers(); digest_covers_clocks(); multiplayer_gate(); hero_unlock_flavour_probe(); construction_refusals(); save_and_restore();
        loader(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-engine"));
        campaign_story_routes(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-engine"));
        battle_events(); battle_perpetual_and_classes(); conquest_events(); loss_events(); counters_save_and_digest(); counter_restart_after_lua_edit(); travel_events(); move_events(); hero_moves_per_arrival(); hero_defeat_events(); construct_events();
        session(); counters_in_the_game_checkpoint(); holocron_goals(); forced_clicks(); story_pause_toggle(); story_hud_requests(); story_lock_release(); story_objectives(); story_goal_rewards(); click_every_control(); session_rewards(); published_story_fault();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

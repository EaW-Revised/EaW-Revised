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

void land_battle_seam() {
    // Tactical and land events and rewards are parsed but refused until land battles exist.
    expect(st::land_battle_refusal == "not available until land battles", "the refusal text");
    expect(st::known_event_type("STORY_LAND_TACTICAL") && !st::known_event_type("STORY_POLITICAL_CONTROL")
        && !st::known_event_type("STORY_BASE_DESTROYED") && !st::known_event_type("Story_Trigger"), "WST-04/08 name table is exact and omits the dropped names");
    auto link = make("LINK", "STORY_TECH_LEVEL", {"1"}); link.reward_type = "LINK_TACTICAL"; link.reward_params[0] = {"Naboo"};
    auto e = engine({sub("S.xml", 1, {make("LAND", "STORY_LAND_TACTICAL", {"x.xml"}), make("ENTER", "STORY_ENTER", {"Naboo"}), link,
        make("MYSTERY", "STORY_TRIGGER")})});
    int refusals{}, pending{};
    for (const auto& row : e.unavailable()) {
        if (row.reason == st::land_battle_refusal) ++refusals; else if (row.reason.find("no evaluator") != std::string::npos) ++pending;
    }
    expect(refusals == 2 && pending == 1, "the land event and the LINK_TACTICAL reward are refused, STORY_ENTER waits for its evaluator");
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
    return {sub("S.xml", 1, {one_second, trig("SPAWN", {{"START"}}, "SPAWN_HERO", {"Tarkin"}), make("ENTER", "STORY_ENTER", {"Naboo"}),
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
                    return d.code == "EAWR-STORY-0001" && d.message.find("STORY_ENTER") != std::string::npos; }),
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
        for (int i = 0; i < 40; ++i) again += take(restored.session.step(executor)).story_presentation.size();
        expect(again == 0, "WST-58 a loaded game does not show the intro again");
        if (workers == 1) baseline = signature;
        else expect(baseline == signature, "1/2/4/8 workers publish the same presentation requests in the same order");
    }
}

// #683 part 2: the apply loop of the session. Rewards run once per step after the service, in trigger order; a reward
// that changes the tech level triggers a further event that runs in the same step; refusals are published once.
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
        if (argc == 4 && std::string_view(argv[1]) == "--installed-associations") {
            installed_campaign_routes(argv[2], argv[3]);
            return galactic_test::failures ? 1 : 0;
        }
        order_and_prerequisites(); elapsed_time(); flags(); tech_levels(); perpetual_and_branches(); sub_plots_and_elements();
        land_battle_seam(); cycles_and_tactical_triggers(); digest_covers_clocks(); multiplayer_gate(); construction_refusals(); save_and_restore();
        loader(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-engine"));
        campaign_story_routes(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-engine"));
        session(); holocron_goals(); session_rewards();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

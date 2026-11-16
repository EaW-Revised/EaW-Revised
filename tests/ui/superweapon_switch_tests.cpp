// HCB-47 (docs/behaviour/hud-command-bar.md, space-tactical-superweapon.md TSW-13): the Death Star switch presenter,
// its click, and the click's round trip through the command sink and the simulation. No Godot: the presenter is a pure
// function of the snapshot rows and two local facts.
#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/superweapon_switch.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/tsw.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
namespace tactical = sim::tactical;
namespace math = sim::math;
using test::ui::expect;
using tactical::TswPhase;
using tactical::TswStage;

constexpr tactical::PlayerId local = 1;
constexpr tactical::PlayerId remote = 2;
constexpr sim::EntityId object = 5;

tactical::TswView row(const TswPhase phase, const tactical::PlayerId owner, const bool gate, const bool target = true,
    const std::uint64_t eta = tactical::TswView::no_eta, const sim::EntityId id = object) {
    tactical::TswView view;
    view.unit = id;
    view.owner = owner;
    view.phase = phase;
    view.serviced = true;
    view.has_target = target;
    view.gate_open = gate;
    view.eta_frames = eta;
    return view;
}

ui::SuperweaponSwitchView present(const std::vector<tactical::TswView>& rows, const ui::SuperweaponSwitchFired fired = {},
    const std::uint64_t tick = 1000, const bool shell_allows = true) {
    ui::SuperweaponSwitchInput input;
    input.rows = rows;
    input.local_player = local;
    input.completed_tick = tick;
    input.fired = fired;
    input.shell_allows = shell_allows;
    input.art = {5, 5};
    return ui::superweapon_switch_view(input);
}

// Every phase x local/remote owner x gate. The expectation is written per phase, not derived from the presenter.
void table() {
    struct Case {
        TswPhase phase;
        bool enabled_when_owned_and_open; // the press can reach the sink
        int icon;                         // -1 base, 0..4 alternates (fired phases rest on the last)
        ui::SuperweaponSwitchTooltip tooltip;
        bool countdown;
    };
    using Tip = ui::SuperweaponSwitchTooltip;
    const std::array<Case, 12> cases{{
        {TswPhase::idle, false, -1, Tip::too_far, false},
        {TswPhase::facing, false, -1, Tip::too_far, false},
        {TswPhase::approaching, false, -1, Tip::too_far, true},
        {TswPhase::ready, true, 0, Tip::ready, true},
        {TswPhase::powering_up, false, 4, Tip::ready, false},
        {TswPhase::charged, false, 4, Tip::ready, false},
        {TswPhase::attack_start, false, 4, Tip::ready, false},
        {TswPhase::attack_anim, false, 4, Tip::ready, false},
        {TswPhase::target_destruction, false, 4, Tip::ready, false},
        {TswPhase::debris_wait, false, 4, Tip::ready, false},
        {TswPhase::post_wait, false, 4, Tip::ready, false},
        {TswPhase::complete, false, 4, Tip::ready, false},
    }};
    for (const auto& item : cases) {
        for (const auto owner : {local, remote}) {
            for (const bool gate : {true, false}) {
                const auto view = present({row(item.phase, owner, gate, true, item.countdown ? 90 : tactical::TswView::no_eta)});
                const std::string context = "phase " + std::to_string(static_cast<int>(item.phase)) + (owner == local ? " local" : " remote")
                    + (gate ? " gate open" : " gate shut");
                // The first object is shown whoever owns it (TSW-W7).
                expect(view.visible && view.unit == object, ("HCB-47: shown for the first object, " + context).c_str());
                expect(view.owned == (owner == local), ("HCB-47: owned follows the local player, " + context).c_str());
                // The press needs the object to be ours and ready; the gate only decides the sound (below).
                expect(view.enabled == (item.enabled_when_owned_and_open && owner == local), ("HCB-47: enabled, " + context).c_str());
                expect(view.icon_frame == item.icon, ("HCB-47: icon, " + context).c_str());
                expect(view.tooltip == item.tooltip, ("HCB-47: tooltip key, " + context).c_str());
                expect(view.countdown_visible == item.countdown, ("HCB-47: readout visibility, " + context).c_str());
                const auto click = ui::superweapon_switch_click(view);
                const bool our_ready = item.phase == TswPhase::ready && owner == local;
                expect(click.send == (our_ready && gate), ("HCB-47: a command is sent only from an owned, ready, open switch, " + context).c_str());
                // Owned and ready: the press sound on accept, the negative cue on a refusal. Owned and not ready: the
                // negative cue (a pull of the lever that cannot start anything), silent once the lever is down. Not ours:
                // nothing.
                const auto want = owner != local ? ui::SuperweaponSwitchFeedback::none
                    : item.phase == TswPhase::ready ? (gate ? ui::SuperweaponSwitchFeedback::press : ui::SuperweaponSwitchFeedback::negative)
                    : view.fired ? ui::SuperweaponSwitchFeedback::none : ui::SuperweaponSwitchFeedback::negative;
                expect(click.feedback == want, ("HCB-47: press sound on accept, negative on a refusal or a not-ready pull, none for a no-op, " + context).c_str());
                expect(view.fired == (item.icon == 4), ("HCB-47: the lever-down flag follows the fired look, " + context).c_str());
            }
        }
    }
    // The readout colour and text (TSW-W8): green while approaching, red once ready; seconds and thousandths.
    const auto approaching = present({row(TswPhase::approaching, remote, true, true, 2933)});
    expect(approaching.countdown_text == " 097.766" && approaching.countdown_colour == ui::superweapon_countdown_green,
        "HCB-47: approaching readout is green, seconds.thousandths of the ETA");
    const auto ready = present({row(TswPhase::ready, local, true, true, 0)});
    expect(ready.countdown_text == " 000.000" && ready.countdown_colour == ui::superweapon_countdown_red,
        "HCB-47: ready readout is 000.000 in red");
    expect(ui::superweapon_countdown_text(60) == " 002.000" && ui::superweapon_countdown_text(1) == " 000.033"
            && ui::superweapon_countdown_text(30 * 1234) == " 1234.000",
        "HCB-47: the readout format is a space, three digits at least, a point, three digits");
    // Only while a target is set and an ETA exists.
    expect(!present({row(TswPhase::approaching, local, true, false, 100)}).countdown_visible, "HCB-47: no target, no readout");
    expect(!present({row(TswPhase::approaching, local, true, true)}).countdown_visible, "HCB-47: no ETA, no readout");
    // Hidden: no object, or the command bar's own conditions.
    expect(!present({}).visible, "HCB-47: no super weapon object hides the switch");
    expect(!present({row(TswPhase::ready, local, true)}, {}, 1000, false).visible, "HCB-47: the shell's conditions hide it");
    // The first object in the list is the one used (ascending entity id), whoever owns it.
    const auto two = present({row(TswPhase::approaching, remote, true, true, 100, 3), row(TswPhase::ready, local, true, true, 0, 4)});
    expect(two.unit == 3 && !two.owned && !two.enabled, "HCB-47: the first object is the one used");
    // An unowned player id never owns.
    expect(!present({row(TswPhase::ready, 0, true)}).owned, "HCB-47: player zero owns nothing");
}

// Fired: the first snapshot that shows the sequence running latches the lever, which animates the alternates at Anim_FPS
// (30 / 5 = 6 frames a step), then rests on the last. The click itself never latches (see sink_round_trip).
void fired_latch_and_animation() {
    ui::SuperweaponSwitchFired fired;
    ui::superweapon_switch_observe(fired, present({row(TswPhase::powering_up, local, true)}, fired, 1000), 1000);
    expect(fired.tick == 1000, "HCB-47: the first snapshot that shows the sequence latches the tick");
    ui::superweapon_switch_observe(fired, present({row(TswPhase::charged, local, true)}, fired, 2000), 2000);
    expect(fired.tick == 1000, "HCB-47: the latch is set once");
    const std::array<std::pair<std::uint64_t, int>, 7> steps{{{1000, 0}, {1005, 0}, {1006, 1}, {1012, 2}, {1018, 3}, {1024, 4}, {5000, 4}}};
    for (const auto& [tick, frame] : steps) {
        const auto view = present({row(TswPhase::ready, local, true)}, fired, tick);
        expect(view.icon_frame == frame && !view.enabled && view.tooltip == ui::SuperweaponSwitchTooltip::ready,
            ("HCB-47: click animation frame at tick " + std::to_string(tick)).c_str());
        expect(!view.countdown_visible, "HCB-47: the readout hides once the switch is fired");
    }
    // Retail never clears the flag: after the sequence the object is idle with no target, the lever stays down.
    const auto after = present({row(TswPhase::idle, local, false, false)}, fired, 9000);
    expect(after.icon_frame == 4 && !after.enabled && after.tooltip == ui::SuperweaponSwitchTooltip::ready,
        "HCB-47: the lever stays down after the sequence");
    // A sequence the local player did not click (a script, another seat) latches the same way.
    ui::SuperweaponSwitchFired other;
    const auto running = present({row(TswPhase::powering_up, remote, true)}, other, 3000);
    ui::superweapon_switch_observe(other, running, 3000);
    expect(other.tick == 3000, "HCB-47: a running sequence latches the lever");
    ui::SuperweaponSwitchFired idle;
    ui::superweapon_switch_observe(idle, present({row(TswPhase::ready, remote, true)}), 3000);
    expect(!idle.tick, "HCB-47: a ready switch does not latch");
    // No alternates: the icon never leaves the base art, ready or fired.
    ui::SuperweaponSwitchInput bare;
    const std::vector<tactical::TswView> bare_rows{row(TswPhase::ready, local, true)};
    bare.rows = bare_rows; bare.local_player = local; bare.completed_tick = 10; bare.art = {0, 5};
    expect(ui::superweapon_switch_view(bare).icon_frame == ui::superweapon_switch_base_icon,
        "HCB-47: a component without alternates keeps the base art");
    // The step length follows the component's Anim_FPS (a slower animation takes more frames per step).
    expect(ui::superweapon_switch_frames_per_step(5) == 6 && ui::superweapon_switch_frames_per_step(10) == 3
            && ui::superweapon_switch_frames_per_step(0) == 1 && ui::superweapon_switch_frames_per_step(60) == 1,
        "HCB-47: frames per animation step");
}

// ---------------------------------------------------------------------------------------------------------------
// Click -> command bytes, and the sink round trip with the simulation.

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId tsw_type = 10;
constexpr tactical::TypeId enemy_type = 11;
constexpr sim::EntityId tsw_id = 1;
constexpr sim::EntityId enemy_id = 2;

math::Fixed units(const std::int64_t value) { return math::Fixed::from_raw(value * one); }
math::Vec3 at(const std::int64_t x, const std::int64_t y = 0, const std::int64_t z = 0) { return {units(x), units(y), units(z)}; }

struct World {
    tactical::TacticalSetup setup;
    tactical::CombatTable table;
};

// Player 1 owns the Death Star; player 2's prop is the target at the origin. `start` is the Death Star's x; a defender
// (conflict defender = 1) starts ready at its own attack distance. The profile is a short flight (attack 500).
World world(const std::int64_t start, const tactical::PlayerId defender, const bool destroyable) {
    World result;
    auto& setup = result.setup;
    setup.seed = 7;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    tactical::UnitState death_star;
    death_star.entity_id = tsw_id; death_star.type_id = tsw_type; death_star.owner = 1; death_star.position = at(start);
    tactical::UnitState enemy;
    enemy.entity_id = enemy_id; enemy.type_id = enemy_type; enemy.owner = 2; enemy.position = at(0, 4000, 0);
    setup.units = {death_star, enemy};
    tactical::TswSetup tsw;
    tsw.targets = {{1, 2, 0, at(0), {}}};
    tsw.conflict = tactical::TswConflictSetup{defender, destroyable, false};
    setup.tsw = tsw;
    tactical::TswProfile profile;
    profile.attack_distance = units(500);
    profile.max_speed = math::Fixed::from_raw(one * 3 / 4);
    profile.max_thrust = math::Fixed::from_raw(one / 50);
    profile.power_up_frames = 270; profile.attack_anim_frames = 75; profile.debris_delay_frames = 33; profile.post_destruction_wait_frames = 350;
    tactical::CombatProfile weapon;
    weapon.type_id = tsw_type; weapon.category_bits = 1U; weapon.tactical_super_weapon = profile;
    result.table.profiles = {weapon, tactical::CombatProfile{enemy_type, 2U, std::nullopt, std::nullopt, {}, {}, {}}};
    return result;
}

tactical::TacticalSession make(const World& value) {
    const std::vector<tactical::SensorProfile> sensors{{tsw_type, units(2000)}, {enemy_type, units(2000)}};
    auto created = tactical::TacticalSession::create(value.setup, sensors, {}, {}, std::nullopt, value.table);
    expect(static_cast<bool>(created), "HCB-47: the super weapon session is created");
    return std::move(created).value();
}

ui::SuperweaponSwitchView view_of(const tactical::TacticalSession& session) {
    ui::SuperweaponSwitchInput input;
    const auto snapshot = session.snapshot();
    input.rows = snapshot->tsw_views();
    input.local_player = local;
    input.completed_tick = snapshot->completed_tick();
    input.art = {5, 5};
    return ui::superweapon_switch_view(input);
}

ui::TacticalIntent intent_for(const ui::SuperweaponSwitchClick& click) {
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::tactical_superweapon;
    intent.units = {click.unit};
    intent.tsw_stage = click.stage;
    intent.origin = ui::CommandOrigin::hud_button;
    return intent;
}

std::vector<tactical::Event> step_events(tactical::TacticalSession& session, const std::uint64_t ticks) {
    std::vector<tactical::Event> events;
    platform::ThreadWorkerAdapter executor(1);
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "HCB-47: the session steps");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->events())
            if (event.order == tactical::OrderKind::tactical_superweapon) events.push_back(event);
    }
    return events;
}

void click_to_command_bytes() {
    // The intent is the one verb with one unit and a stage; it becomes the sim's own payload with the player admission.
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::tactical_superweapon;
    intent.units = {tsw_id};
    intent.tsw_stage = TswStage::start_power_up;
    const auto payload = ui::command_payload(intent);
    expect(payload && payload.value() == tactical::CommandPayload{tactical::TacticalSuperweaponPayload{TswStage::start_power_up, tactical::TswAdmission::player}},
        "HCB-47: the click's payload is the switch command with the player admission");
    expect(tactical::human_wire_payload(payload.value()), "HCB-47: the click's payload may ride the human wire");
    intent.units = {};
    expect(!ui::command_payload(intent), "HCB-47: the switch command names its object");
    intent.units = {1, 2};
    expect(!ui::command_payload(intent), "HCB-47: the switch command names one object");

    // Through the scheduler: stamped for the open tick as the player's command, one unit, the stage.
    ui::CommandScheduler scheduler(local, 40, 7);
    scheduler.present_snapshot(6);
    intent.units = {tsw_id};
    expect(static_cast<bool>(scheduler.issue(intent)), "HCB-47: the click is queued");
    expect(!scheduler.issue(intent), "HCB-47: a second click before its tick is dropped as already pending");
    const auto commands = scheduler.take(7);
    expect(commands.size() == 1 && commands[0].key.tick == 7 && commands[0].key.player_id == local && commands[0].key.sequence == 40
            && commands[0].units == std::vector<sim::EntityId>{tsw_id}
            && tactical::order_kind(commands[0].payload) == tactical::OrderKind::tactical_superweapon,
        "HCB-47: the queued command is the player's, for the open tick, with the unit");

    // The replay bytes of a clicked session equal the bytes of the same command submitted directly.
    auto clicked = make(world(500, 1, true));
    auto direct = make(world(500, 1, true));
    ui::CommandScheduler sink(local, 1, 0);
    expect(static_cast<bool>(sink.issue(intent)), "HCB-47: the click is queued for the round trip");
    for (auto& command : sink.take(0)) expect(static_cast<bool>(clicked.submit(command)), "HCB-47: the clicked command is accepted");
    expect(static_cast<bool>(direct.submit(tactical::PlayerCommand{{0, local, 1}, {tsw_id},
        tactical::TacticalSuperweaponPayload{TswStage::start_power_up, tactical::TswAdmission::player}})), "HCB-47: the direct command is accepted");
    static_cast<void>(step_events(clicked, 4));
    static_cast<void>(step_events(direct, 4));
    const auto clicked_bytes = tactical::write_replay(clicked.record());
    const auto direct_bytes = tactical::write_replay(direct.record());
    expect(clicked_bytes && direct_bytes && clicked_bytes.value() == direct_bytes.value(),
        "HCB-47: a click and the same command give the same replay bytes");
}

// Ready accepted, approaching refused, a shut gate refused locally: the presenter and the simulation agree.
void sink_round_trip() {
    // A defender is ready: the view enables the switch, the click sends, the sim accepts and powers up.
    auto ready = make(world(500, 1, true));
    static_cast<void>(step_events(ready, 3));
    auto view = view_of(ready);
    expect(view.visible && view.enabled && view.owned && view.phase == TswPhase::ready && view.countdown_visible
            && view.countdown_text == " 000.000",
        "HCB-47: a ready, owned switch is enabled with a zero readout");
    ui::SuperweaponSwitchFired latch;
    ui::superweapon_switch_observe(latch, view, ready.completed_tick());
    expect(!latch.tick, "HCB-47: a ready switch does not latch");
    auto click = ui::superweapon_switch_click(view);
    expect(click.send && click.feedback == ui::SuperweaponSwitchFeedback::press && click.stage == TswStage::start_power_up,
        "HCB-47: the ready click sends a power-up request and earns the press sound");
    ui::CommandScheduler scheduler(local, 1, ready.completed_tick());
    expect(static_cast<bool>(scheduler.issue(intent_for(click))), "HCB-47: the request is queued");
    for (auto& command : scheduler.take(ready.completed_tick())) expect(static_cast<bool>(ready.submit(command)), "HCB-47: the request is accepted by the session");
    auto events = step_events(ready, 2);
    expect(events.size() == 1 && events[0].kind == tactical::EventKind::order_accepted && ready.tsw_state(tsw_id)->phase == TswPhase::powering_up,
        "HCB-47: the simulation accepts the ready request and powers up");
    view = view_of(ready);
    expect(!view.enabled && view.tooltip == ui::SuperweaponSwitchTooltip::ready && view.icon_frame == 4 && !view.countdown_visible,
        "HCB-47: the next snapshot shows the fired switch");

    // An attacker mid-flight: the switch is disabled and takes no click; a stale power-up request that does reach the
    // sink is refused by the simulation (the same answer the local admission gives).
    auto flying = make(world(700, 0, true));
    static_cast<void>(step_events(flying, 20));
    view = view_of(flying);
    expect(view.visible && !view.enabled && view.phase == TswPhase::approaching && view.countdown_visible
            && view.countdown_colour == ui::superweapon_countdown_green && view.tooltip == ui::SuperweaponSwitchTooltip::too_far
            && view.icon_frame == ui::superweapon_switch_base_icon,
        "HCB-47: an approaching switch is disabled, base art, too-far tooltip, green readout");
    {
        // Owner feedback: the pull of the lever while the object approaches earns the negative cue and sends nothing; the
        // look stays the not-ready one (base art with the red lamp, the too-far tooltip), which a ready switch differs from.
        const auto pull = ui::superweapon_switch_click(view);
        expect(!pull.send && pull.feedback == ui::SuperweaponSwitchFeedback::negative && !view.fired,
            "HCB-47: a not-ready pull of the lever answers with the negative cue and sends nothing");
        auto lit = view;
        lit.phase = TswPhase::ready;
        lit.enabled = true;
        lit.icon_frame = 0;
        expect(lit.icon_frame != view.icon_frame && lit.enabled != view.enabled,
            "HCB-47: the ready switch's lamp and lever art differ from the not-ready switch's");
        auto down = view;
        down.fired = true;
        expect(ui::superweapon_switch_click(down).feedback == ui::SuperweaponSwitchFeedback::none,
            "HCB-47: once the lever is down a pull is silent");
        auto hidden = view;
        hidden.visible = false;
        expect(ui::superweapon_switch_click(hidden).feedback == ui::SuperweaponSwitchFeedback::none, "HCB-47: a hidden switch takes no click");
    }
    ui::SuperweaponSwitchClick stale;
    stale.unit = tsw_id;
    stale.stage = TswStage::start_power_up;
    ui::CommandScheduler stale_sink(local, 1, flying.completed_tick());
    expect(static_cast<bool>(stale_sink.issue(intent_for(stale))), "HCB-47: the stale request is queued");
    for (auto& command : stale_sink.take(flying.completed_tick())) expect(static_cast<bool>(flying.submit(command)), "HCB-47: the stale request reaches the session");
    events = step_events(flying, 2);
    expect(events.size() == 1 && events[0].kind == tactical::EventKind::order_rejected
            && events[0].reason == tactical::RejectReason::ability_unavailable && flying.tsw_state(tsw_id)->phase == TswPhase::approaching,
        "HCB-47: the simulation refuses a power-up request while approaching and leaves the flight alone");

    // A ready switch on a planet that cannot be destroyed: enabled, but the click is refused locally with the negative
    // cue and nothing is sent (the gate the simulation would refuse with).
    auto shut = make(world(500, 1, false));
    static_cast<void>(step_events(shut, 3));
    view = view_of(shut);
    click = ui::superweapon_switch_click(view);
    expect(view.enabled && !view.gate_open && !click.send && click.feedback == ui::SuperweaponSwitchFeedback::negative,
        "HCB-47: a ready switch on an undestroyable planet is refused locally with the negative cue");
    // The same request reaching the simulation anyway is refused too.
    ui::CommandScheduler shut_sink(local, 1, shut.completed_tick());
    ui::SuperweaponSwitchClick forced;
    forced.unit = tsw_id;
    forced.stage = TswStage::start_power_up;
    expect(static_cast<bool>(shut_sink.issue(intent_for(forced))), "HCB-47: the forced request is queued");
    for (auto& command : shut_sink.take(shut.completed_tick())) expect(static_cast<bool>(shut.submit(command)), "HCB-47: the forced request reaches the session");
    events = step_events(shut, 2);
    expect(events.size() == 1 && events[0].kind == tactical::EventKind::order_rejected && shut.tsw_state(tsw_id)->phase == TswPhase::ready,
        "HCB-47: the simulation refuses it the same way");
    // F1: the click only queued a request that the simulation refused. The viewer latches only from snapshots (no click
    // latch), so after the refusal the switch is shown ready and the observe step leaves the lever up: usable again.
    ui::SuperweaponSwitchFired refused_latch;
    view = view_of(shut);
    ui::superweapon_switch_observe(refused_latch, view, shut.completed_tick());
    expect(!refused_latch.tick && view.enabled && view.phase == TswPhase::ready && view.icon_frame == 0
            && view.tooltip == ui::SuperweaponSwitchTooltip::ready && view.countdown_visible,
        "HCB-47: a request the simulation refused leaves the switch usable");

    // A remote owner's object is shown but does nothing for the local player.
    ui::SuperweaponSwitchInput other;
    const auto snapshot = ready.snapshot();
    other.rows = snapshot->tsw_views();
    other.local_player = remote;
    other.completed_tick = snapshot->completed_tick();
    other.art = {5, 5};
    const auto remote_view = ui::superweapon_switch_view(other);
    expect(remote_view.visible && !remote_view.owned && !remote_view.enabled
            && ui::superweapon_switch_click(remote_view).feedback == ui::SuperweaponSwitchFeedback::none,
        "HCB-47: an opponent sees the switch, and a click does nothing");
}

// HCB-47: the whole lever panel takes the pointer, at every placement. The panel is the shell's mesh slot and the drawn art;
// a point anywhere on either (and on the rim around them) is inside the hit rect once it is placed on the screen.
void hit_rect_covers_the_panel() {
    const data::ui::ReferenceRect mesh{350.04F, 20.0F, 49.92F, 105.4F};
    const data::ui::ReferenceRect art{349.5F, 18.0F, 50.0F, 106.0F}; // the texel-sized art around its bone
    const auto hit = ui::superweapon_switch_hit(mesh, art);
    constexpr float eps = 1e-3F;
    constexpr float margin = ui::superweapon_switch_hit_margin;
    expect(hit.x <= mesh.x - margin + eps && hit.x <= art.x - margin + eps && hit.y <= mesh.y - margin + eps && hit.y <= art.y - margin + eps
            && hit.right() >= mesh.right() + margin - eps && hit.right() >= art.right() + margin - eps
            && hit.top() >= mesh.top() + margin - eps && hit.top() >= art.top() + margin - eps,
        "HCB-47: the hit rect holds the mesh slot and the art with the margin on every side");
    // A placement (screen pixels per shell unit, the shell's lower-left corner) maps the panel's corners and middle into
    // the placed hit rect at the common resolutions.
    for (const double scale : {0.5, 0.9375, 1.0, 1.5, 2.0}) {
        const presentation::ui::ShellPlacement placement{0.0, 1080.0 * scale, scale};
        const auto placed = presentation::ui::shell_to_screen(hit, placement);
        const std::array<std::array<float, 2>, 5> points{{{mesh.x, mesh.y}, {mesh.right(), mesh.top()},
            {art.x, art.top()}, {art.right(), art.y}, {mesh.x + mesh.width / 2.0F, mesh.y + mesh.height / 2.0F}}};
        for (const auto& point : points) {
            const auto screen = presentation::ui::shell_point_to_screen(point[0], point[1], placement);
            expect(screen.x >= placed.x && screen.x <= placed.x + placed.width && screen.y >= placed.y && screen.y <= placed.y + placed.height,
                ("HCB-47: a point on the panel is inside the placed hit rect at scale " + std::to_string(scale)).c_str());
        }
    }
}

} // namespace

int main() {
    hit_rect_covers_the_panel();
    table();
    fired_latch_and_animation();
    click_to_command_bytes();
    sink_round_trip();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " super weapon switch contract(s) failed\n";
        return 1;
    }
    std::cout << "super weapon switch contracts passed\n";
    return 0;
}

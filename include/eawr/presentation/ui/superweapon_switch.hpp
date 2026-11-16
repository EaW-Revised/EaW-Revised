#pragma once

#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/sim/tactical/tsw.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// HCB-47 (docs/behaviour/hud-command-bar.md, space-tactical-superweapon.md TSW-13): the Death Star switch button and its
// countdown readout. Everything here is a pure function of the published snapshot rows (TswView), the local player and
// two small local facts; it draws nothing and writes no simulation state (UI-C1: the click only becomes a command
// through the command sink).
namespace eawr::presentation::ui {

// The three tooltip keys of the component, in the order the data lists them (too far, ready, fired). Which one shows
// when is not captured (TSW-C3): the played run showed the ready text both before and after the click, so the presenter
// uses `fired` for no state (unverified, marked in the behaviour doc); the key stays so the data's order is kept.
enum class SuperweaponSwitchTooltip : std::uint8_t { too_far = 0, ready = 1, fired = 2 };

// Which local sound a click earns: the object's own button-press event on an accepted request, the generic GUI
// negative-feedback event on a refused one (TSW-W8).
enum class SuperweaponSwitchFeedback : std::uint8_t { none = 0, press = 1, negative = 2 };

// The icon art: the base icon while the button is disabled or idle, then the alternates in data order. The click plays
// the alternates once at the component's Anim_FPS without looping and rests on the last one (TSW-W7, TSW-W8).
inline constexpr int superweapon_switch_base_icon = -1;

// The component's own animation facts, read from the data by the caller: how many alternates Icon_Alternate_Texture_Name
// lists and the Anim_FPS rate. With no alternates the icon never leaves the base art.
struct SuperweaponSwitchArt {
    std::uint32_t alternate_count{};
    std::uint32_t anim_fps{};
    bool operator==(const SuperweaponSwitchArt&) const = default;
};

// Frames of the sequence (30 per second) that one animation step lasts at `anim_fps` (the component's Anim_FPS).
[[nodiscard]] constexpr std::uint32_t superweapon_switch_frames_per_step(const std::uint32_t anim_fps) noexcept {
    return anim_fps == 0 ? 1U : std::max<std::uint32_t>(1U, sim::tactical::logical_frames_per_second / anim_fps);
}

// The local "fired" fact: the tick of the first snapshot that shows the sequence running, whoever started it (this
// player's click, a script or another peer). Retail never clears it (TSW-W8), so the lever stays down for the rest of
// the battle.
struct SuperweaponSwitchFired {
    std::optional<std::uint64_t> tick;
    bool operator==(const SuperweaponSwitchFired&) const = default;
};

struct SuperweaponSwitchInput {
    std::span<const sim::tactical::TswView> rows; // the snapshot's rows, ascending entity id
    sim::tactical::PlayerId local_player{};
    std::uint64_t completed_tick{};
    SuperweaponSwitchFired fired;
    // The command bar's own conditions (TSW-W7): space mode, no hide counter, and the local faction being the command
    // bar's "evil faction" (value unread: unverified), supplied by the caller. The default shows the button.
    bool shell_allows{true};
    SuperweaponSwitchArt art;
};

struct SuperweaponSwitchView {
    bool visible{};
    bool enabled{};                  // the press can reach the command sink
    int icon_frame{superweapon_switch_base_icon}; // -1 base icon, 0..4 the alternates
    SuperweaponSwitchTooltip tooltip{SuperweaponSwitchTooltip::too_far};
    sim::EntityId unit{};            // the first object with the behaviour, whoever owns it (TSW-W7)
    bool owned{};                    // the local player owns it: only then does a click do anything
    bool fired{};                    // the lever is down for the rest of the battle (the sequence runs, or a click was latched)
    sim::tactical::TswPhase phase{sim::tactical::TswPhase::idle};
    bool gate_open{};
    bool countdown_visible{};
    std::string countdown_text;      // " %03d.%03d", seconds then thousandths
    std::array<std::uint8_t, 4> countdown_colour{255, 255, 255, 255};
    bool operator==(const SuperweaponSwitchView&) const = default;
};

// The readout text: the time to arrival as seconds and thousandths of a second (the debug build formats two
// three-digit fields; that the second is thousandths is unverified), with the leading space the format carries.
[[nodiscard]] inline std::string superweapon_countdown_text(const std::uint64_t eta_frames) {
    const auto fps = static_cast<std::uint64_t>(sim::tactical::logical_frames_per_second);
    const auto seconds = eta_frames / fps;
    const auto thousandths = (eta_frames % fps) * 1000U / fps;
    const auto field = [](std::uint64_t value) {
        auto text = std::to_string(value);
        if (text.size() < 3) text.insert(0, 3 - text.size(), '0');
        return text;
    };
    return " " + field(seconds) + "." + field(thousandths);
}

// TSW-W7/W8: green while approaching, red once ready. HCB-47 / TSW-C2: the ready capture
// has opaque channel-red glyph interiors with a black outline. The approach tint remains
// unverified (the played reference only covers ready); retain its existing green until captured.
inline constexpr std::array<std::uint8_t, 4> superweapon_countdown_green{128, 255, 128, 255};
inline constexpr std::array<std::uint8_t, 4> superweapon_countdown_red{255, 0, 0, 255};

[[nodiscard]] inline SuperweaponSwitchView superweapon_switch_view(const SuperweaponSwitchInput& input) {
    using sim::tactical::TswPhase;
    SuperweaponSwitchView view;
    // TSW-W7: the first object in the list is used, whoever owns it; with none the button and readout are hidden.
    if (input.rows.empty() || !input.shell_allows) return view;
    const auto& row = input.rows.front();
    view.visible = true;
    view.unit = row.unit;
    view.phase = row.phase;
    view.gate_open = row.gate_open;
    view.owned = row.owner == input.local_player && row.owner != 0;
    // The sequence running is a fired switch whoever started it (a script call, or another peer's click).
    const bool sequence = sim::tactical::tsw_holds_victory(row.phase);
    const bool fired = input.fired.tick.has_value() || sequence;
    view.fired = fired;
    const bool ready = row.phase == TswPhase::ready && row.has_target;
    // TSW-C3: after the click the hover text was the same as before it, so a fired switch keeps the ready text.
    view.tooltip = (fired || ready) ? SuperweaponSwitchTooltip::ready : SuperweaponSwitchTooltip::too_far;
    // TSW-W7: ready and not yet fired: enabled with the first alternate; any other state the base icon, disabled; once
    // fired the click animation owns the icon and the button stays disabled.
    if (fired) {
        const auto since = input.fired.tick && input.completed_tick > *input.fired.tick
            ? input.completed_tick - *input.fired.tick : std::uint64_t{};
        // The first presentation of a sequence that began elsewhere (a script, another seat) has no latch yet and shows the
        // animation's last frame; the viewer latches it from the snapshot at once (superweapon_switch_observe), after which
        // the alternates play from that tick like any other fired switch.
        const auto last = std::uint64_t{input.art.alternate_count == 0 ? 0U : input.art.alternate_count - 1U};
        const auto step = input.fired.tick ? since / superweapon_switch_frames_per_step(input.art.anim_fps) : last;
        view.icon_frame = input.art.alternate_count == 0 ? superweapon_switch_base_icon : static_cast<int>(std::min(step, last));
        view.enabled = false;
    } else if (ready) {
        view.icon_frame = input.art.alternate_count == 0 ? superweapon_switch_base_icon : 0;
        view.enabled = view.owned;
    }
    // TSW-W8: the readout shows while approaching or ready, once the sequence has not been fired, with a time to arrival.
    const bool counting = row.has_target && row.eta_frames != sim::tactical::TswView::no_eta
        && (row.phase == TswPhase::approaching || row.phase == TswPhase::ready) && !input.fired.tick.has_value();
    if (counting) {
        view.countdown_visible = true;
        view.countdown_text = superweapon_countdown_text(row.eta_frames);
        view.countdown_colour = row.phase == TswPhase::ready ? superweapon_countdown_red : superweapon_countdown_green;
    }
    return view;
}

// What a click on the switch does. Retail finds the first object with the behaviour, does nothing unless the local player
// owns it, then asks for the next stage; an accepted request plays the press sound and a refused one the generic
// negative-feedback sound (TSW-W8). The admission is the one the simulation uses (TSW-07), so the sound does not wait for
// a tick; the simulation checks the request again at execution.
struct SuperweaponSwitchClick {
    bool send{};                      // hand `stage` for `unit` to the command sink
    sim::tactical::TswStage stage{sim::tactical::TswStage::start_power_up};
    sim::EntityId unit{};
    SuperweaponSwitchFeedback feedback{SuperweaponSwitchFeedback::none};
    bool operator==(const SuperweaponSwitchClick&) const = default;
};

[[nodiscard]] inline SuperweaponSwitchClick superweapon_switch_click(const SuperweaponSwitchView& view) {
    SuperweaponSwitchClick click;
    // A hidden switch takes no click, and a switch the local player does not own does nothing (no sound). Project
    // deviation (owner feedback on a played run, where a pull of the lever while the object was still approaching gave no
    // sign at all): an owned switch that is not ready answers with the negative-feedback sound and sends nothing; once the
    // sequence has run the lever is down for good and stays silent.
    if (!view.visible || !view.owned) return click;
    click.unit = view.unit;
    click.stage = sim::tactical::TswStage::start_power_up;
    if (!view.enabled) {
        if (!view.fired) click.feedback = SuperweaponSwitchFeedback::negative;
        return click;
    }
    if (sim::tactical::tsw_request_accepted(click.stage, view.phase, view.gate_open)) {
        click.send = true;
        click.feedback = SuperweaponSwitchFeedback::press;
    } else {
        click.feedback = SuperweaponSwitchFeedback::negative;
    }
    return click;
}

// The pointer's rect for the switch, in shell units: the whole lever panel, i.e. the shell's mesh slot and the drawn art
// (the base icon and the lever) together, grown by a small margin so the panel's rim takes the click too. The mesh
// slot alone was the smaller of the two at some placements (owner feedback: the lever was hard to hit).
inline constexpr float superweapon_switch_hit_margin = 6.0F;
[[nodiscard]] inline data::ui::ReferenceRect superweapon_switch_hit(const data::ui::ReferenceRect& mesh,
    const data::ui::ReferenceRect& art) noexcept {
    const float left = std::min(mesh.x, art.x) - superweapon_switch_hit_margin;
    const float bottom = std::min(mesh.y, art.y) - superweapon_switch_hit_margin;
    const float right = std::max(mesh.right(), art.right()) + superweapon_switch_hit_margin;
    const float top = std::max(mesh.top(), art.top()) + superweapon_switch_hit_margin;
    return {left, bottom, right - left, top - bottom};
}

// The latch that keeps the lever down. Called once per presented snapshot with the view; deliberately never cleared
// (TSW-W8: the retail flag is never cleared in the walked code). Only a snapshot that shows the sequence running sets
// it, never the click itself: a click only queues a request, and the simulation may still refuse it at execution (the
// switch then stays usable).
inline void superweapon_switch_observe(SuperweaponSwitchFired& fired, const SuperweaponSwitchView& view,
    const std::uint64_t completed_tick) {
    if (fired.tick || !view.visible) return;
    if (sim::tactical::tsw_holds_victory(view.phase)) fired.tick = completed_tick;
}

} // namespace eawr::presentation::ui

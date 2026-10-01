// #530: the command bar's station production (docs/behaviour/space-purchasing.md PU-60 to PU-67).
// The build buttons' states, the two build queues' slots and the front's percentage, the
// reinforcement pool folded by type and the credits and population texts. The Godot half
// (drawing, the pointer, the commands) runs in the viewer (tests/presentation/renderer).

#include "eawr/presentation/ui/production.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
namespace tactical = sim::tactical;
using sim::math::Fixed;
using test::ui::expect;

constexpr tactical::TypeId x_wing = 10;
constexpr tactical::TypeId y_wing = 11;
constexpr tactical::TypeId upgrade = 12;

Fixed credits(const std::int64_t whole) { return Fixed::from_raw(whole * Fixed::scale); }

tactical::StationMenu menu() {
    tactical::StationMenu result;
    result.options = {
        {x_wing, tactical::BuildKind::unit, tactical::BuildQueue::units, credits(500), 450, 450, 1, true},
        {y_wing, tactical::BuildKind::unit, tactical::BuildQueue::units, credits(550), 510, 510, 1, true},
        {upgrade, tactical::BuildKind::upgrade, tactical::BuildQueue::upgrades, credits(1000), 900, 900, 0, false}};
    return result;
}

void test_buttons() {
    const auto buttons = ui::layout_build_buttons(menu(), credits(6000), {0, 0}, 5, 24);
    expect(buttons.size() == 3, "PU-60: one button per option");
    expect(buttons[0].slot == 0 && buttons[1].slot == 1 && buttons[2].slot == 2, "PU-60: in list order");
    expect(buttons[0].enabled && buttons[1].enabled, "PU-61: affordable units with room are enabled");
    expect(!buttons[2].enabled && buttons[2].state == ui::BuildButtonState::normal, "PU-20: an upgrade shows disabled");
    expect(buttons[0].price == 500 && buttons[1].price == 550, "PU-62: the price in whole credits");

    const auto poor = ui::layout_build_buttons(menu(), Fixed::from_raw(credits(550).raw() - 1), {0, 0}, 5, 24);
    expect(poor[0].enabled && !poor[1].enabled && poor[1].state == ui::BuildButtonState::unaffordable,
           "PU-61: 549.99 credits buy an X-wing, not a Y-wing");
    const auto full = ui::layout_build_buttons(menu(), credits(6000), {5, 0}, 5, 24);
    expect(!full[0].enabled && full[0].state == ui::BuildButtonState::queue_full, "PU-61: a full unit queue");
    const auto both = ui::layout_build_buttons(menu(), credits(100), {5, 0}, 5, 24);
    expect(both[0].state == ui::BuildButtonState::unaffordable && !both[0].room,
           "PU-61, PU-62: unaffordable wins over a full queue; the price still shows no room");
    expect(ui::layout_build_buttons(menu(), credits(6000), {0, 0}, 5, 2).size() == 2, "PU-60: no more than the slots");
}

void test_queue() {
    std::array<std::vector<tactical::QueueEntry>, tactical::build_queue_count> queues;
    queues[0] = {{x_wing, 1, credits(500), 450, 460}, {y_wing, 1, credits(550), 510, 0}};
    auto slots = ui::layout_build_queue(queues, 235);
    expect(slots.size() == 2, "PU-63: every entry");
    expect(slots[0].component == 5 && slots[1].component == 6, "PU-63: units take tqueue05 on");
    expect(slots[0].percent == "50%" && slots[0].progress == 0.5, "PU-64: the front at 225 of 450 frames");
    expect(!slots[1].percent && slots[1].progress == 1.0, "PU-64: the others show full and no text");
    slots = ui::layout_build_queue(queues, 10);
    expect(slots[0].percent == "0%", "PU-64: just queued");
    slots = ui::layout_build_queue(queues, 459);
    expect(slots[0].percent == "99%", "PU-64: the text truncates");
    queues[1] = {{upgrade, 1, credits(1000), 900, 1000}};
    slots = ui::layout_build_queue(queues, 100);
    expect(slots.size() == 3 && slots[0].component == 0 && slots[0].queue == tactical::BuildQueue::upgrades,
           "PU-63: upgrades take tqueue00 on, first in component order");
    expect(ui::queue_component(tactical::BuildQueue::units, 5) == std::nullopt, "PU-63: five slots per queue");
}

void test_pool() {
    const std::vector<tactical::TypeId> pool{y_wing, x_wing, y_wing, y_wing};
    const auto value = [](const tactical::TypeId type) { return type == y_wing ? 2U : 1U; };
    auto slots = ui::layout_pool(pool, 20, 25, value);
    expect(slots.size() == 2 && slots[0].type == y_wing && slots[1].type == x_wing, "PU-66: by type, first completion first");
    expect(slots[0].count == 3 && slots[0].text == "x3" && slots[1].text.empty(), "PU-66: x<n> for more than one");
    expect(slots[0].enabled && slots[1].enabled, "PU-66: room for both");
    slots = ui::layout_pool(pool, 24, 25, value);
    expect(!slots[0].enabled && slots[1].enabled, "PU-66: a type whose population exceeds the room is disabled");
    expect(ui::pool_rows(0) == 0 && ui::pool_rows(4) == 0 && ui::pool_rows(5) == 1 && ui::pool_rows(20) == 4
               && ui::pool_rows(40) == 4,
           "PU-67: rows past the first");
}

void test_texts() {
    expect(ui::credits_text(Fixed::from_raw(credits(6050).raw() - 1)) == "6049", "PU-07: rounded down");
    expect(ui::credits_text(credits(6000)) == "6000", "PU-07: whole credits");
    expect(ui::population_text(1, 25) == "1/25", "PU-65: used over cap");
}

} // namespace

int main() {
    test_buttons();
    test_queue();
    test_pool();
    test_texts();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " production check(s) failed\n";
        return 1;
    }
    std::cout << "production: all checks passed\n";
    return 0;
}

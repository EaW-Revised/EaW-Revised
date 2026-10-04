// #530: the command bar's station production (docs/behaviour/space-purchasing.md PU-60 to PU-67).
// The build buttons' states, the two build queues' slots and the front's percentage, the
// reinforcement pool folded by type and the credits and population texts. The Godot half
// (drawing, the pointer, the commands) runs in the viewer (tests/presentation/renderer).

#include "eawr/presentation/ui/production.hpp"
#include "eawr/presentation/ui/pads.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <new>

namespace allocation_probe {
bool armed{};
std::size_t count{};
}

void* operator new(const std::size_t size) {
    if (allocation_probe::armed) ++allocation_probe::count;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

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
    auto gated_menu = menu();
    gated_menu.options[0].available = false;
    gated_menu.options[0].disabled_reason = "Unsupported primary weapon.";
    const auto gated = ui::layout_build_buttons(gated_menu, credits(6000), {0, 0}, 5, 24);
    expect(gated.size() == 3 && !gated[0].enabled && gated[0].disabled_reason == "Unsupported primary weapon."
        && gated[1].enabled, "RG-04: gated entry remains visible with its reason; supported neighbors stay buyable");
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

void test_menu_limits() {
    auto options = menu();
    options.options[2].available = true;
    options.options[2].requirements.current_allies = 1;
    tactical::ProductionCounts research;
    auto hero = options.options[0];
    hero.requirements.current_player = 1;
    tactical::ProductionCounts hero_counts;
    const auto read_counts = [&](const tactical::TypeId type) {
        return type == upgrade ? research : type == x_wing ? hero_counts : tactical::ProductionCounts{};
    };
    const auto layout = [&](const std::size_t slots = 24) {
        return ui::layout_build_buttons(options, credits(6000), {0, 0}, 5, slots,
            [&](const auto& option) { return ui::build_option_state(option, read_counts); });
    };
    research.current_allies = research.queued_allies = 1;
    auto cards = layout();
    expect(cards.size() == 3 && !cards[2].enabled,
        "WPR-62: allied queued research stays visible and disabled");
    research = {};
    expect(layout()[2].enabled, "WPR-62: cancel releases research reservation");
    research.current_allies = 1;
    cards = layout();
    expect(cards.size() == 2 && cards[0].type == x_wing && cards[1].type == y_wing,
        "WPR-61: completed research is absent; remaining order stays stable");
    hero_counts.current_player = hero_counts.current_allies = 1;
    expect(!ui::build_option_state(hero, read_counts).visible,
        "WPR-61: pooled hero hides its purchase option");
    hero_counts.owned_player = 1;
    expect(!ui::build_option_state(hero, read_counts).visible,
        "WPR-61: deployed hero remains hidden with its logical purchase identity");
    hero_counts = {}; hero_counts.lifetime_player = 1;
    expect(ui::build_option_state(hero, read_counts).visible,
        "WPR-62: hero death releases current limit even after a completed purchase");
    auto use = hero;
    hero_counts.current_player = hero_counts.current_allies = 1;
    expect(!ui::build_option_state(use, read_counts).visible, "WPR-62: held superweapon use hides");
    hero_counts.current_player = 0;
    expect(ui::build_option_state(use, read_counts).visible,
        "WPR-62: spending own use reoffers even while an ally holds another use");
    use.requirements.current_player.reset(); use.requirements.lifetime_player = 1;
    hero_counts.lifetime_player = 1;
    expect(!ui::build_option_state(use, read_counts).visible, "WPR-61: lifetime limit stays hidden after consumption");
    use.requirements.lifetime_player.reset(); use.requirements.current_player = 0;
    hero_counts = {};
    expect(!ui::build_option_state(use, read_counts).visible, "WPR-61: authored zero hides at zero ownership");
    options.options[0].requirements.current_player = 1;
    hero_counts.current_player = hero_counts.current_allies = 1;
    cards = layout(1);
    expect(cards.size() == 1 && cards[0].type == y_wing && cards[0].slot == 0 && cards[0].option == 1,
        "WPR-60: hidden entries do not consume slots; click retains authored option index");
    auto successor = options.options[1]; successor.higher_upgrades = {upgrade};
    expect(!ui::build_option_state(successor, read_counts).visible,
        "WPR-63: an allied completed successor keeps removed lower research hidden");
    research.queued_allies = 1;
    expect(ui::build_option_state(successor, read_counts).visible,
        "WPR-63: a queued successor is not completed ownership");
    successor.higher_upgrades.clear(); successor.requirements.prerequisites = {upgrade};
    expect(!ui::build_option_state(successor, read_counts).visible,
        "WPR-63: a queued prerequisite does not expose the option");
    research.queued_allies = 0;
    const auto allied_prerequisite = ui::build_option_state(successor, read_counts);
    expect(allied_prerequisite.visible && !allied_prerequisite.enabled,
        "WPR-63: allied prerequisite exposes the card while preserving the existing own-only purchase gate");
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

void test_pool_notifications() {
    tactical::PlayerEconomy ledger;
    tactical::EconomyPlayer player;
    ledger.queues[0] = {{x_wing, 1, credits(500), 1, 1}, {x_wing, 1, credits(500), 1, 0}};
    const auto service = [&](const std::uint64_t frame) {
        tactical::service_production(ledger, player, frame,
            [](const auto&) { return true; }, [](const auto&) { return tactical::BuildKind::unit; });
    };
    service(1);
    expect(ledger.pool_version == 1 && ledger.pool_additions == 1 && ledger.pool_addition_frame == 1,
           "PU-71: completion records an authoritative addition");
    ledger.pool.clear(); // consumed before the next presented snapshot
    service(2);
    tactical::EconomyView view;
    view.pool = ledger.pool;
    view.pool_version = ledger.pool_version;
    view.pool_additions = ledger.pool_additions;
    view.pool_addition_frame = ledger.pool_addition_frame;
    ui::PoolNotifications notification;
    notification.observe(view);
    expect(notification.additions == 2 && notification.frame == 3,
           "PU-71: skipped completion and deployment still notify, including duplicate types");
    service(3);
    expect(ledger.pool_additions == 2, "PU-71: idle production never notifies");
}

void test_warmed_menu_allocations() {
    auto gated_menu = menu();
    gated_menu.options[0].available = false;
    gated_menu.options[0].disabled_reason = std::string(256, 'x');
    auto buttons = ui::layout_build_buttons(gated_menu, credits(6000), {0, 0}, 5, 24);
    ui::BuildMenuCache cache;
    std::size_t card_builds{};
    const auto sync = [&] {
        if (!cache.refresh(buttons)) return;
        ++card_builds;
        // Card construction owns long type names and reasons; it must run only
        // on a menu change, just as the viewer's production card sync does.
        std::vector<std::string> card_strings;
        for (const auto& button : buttons) {
            card_strings.push_back(std::string(128, 'n'));
            card_strings.push_back(button.disabled_reason);
        }
    };
    sync();
    ui::AbilityButton ability;
    static constexpr std::string_view ability_reason = "This authored ability is unavailable until its tactical behavior is supported.";
    allocation_probe::count = 0;
    allocation_probe::armed = true;
    for (std::size_t frame = 0; frame < 10000; ++frame) {
        ability.disabled_reason = ability_reason;
        ui::update_build_buttons(buttons, gated_menu, credits(6000), {0, 0}, 5, 24);
        sync();
    }
    allocation_probe::armed = false;
    expect(ability.disabled_reason.data() == ability_reason.data(), "RG-03: ability reasons borrow immutable policy storage");
    expect(allocation_probe::count == 0 && card_builds == 1,
        "RG-04: unchanged open build menu allocates zero and constructs no cards");
    ui::update_build_buttons(buttons, gated_menu, credits(100), {0, 0}, 5, 24);
    expect(buttons[1].state == ui::BuildButtonState::unaffordable, "cached menu becomes unaffordable");
    ui::update_build_buttons(buttons, gated_menu, credits(6000), {5, 0}, 5, 24);
    expect(buttons[1].state == ui::BuildButtonState::queue_full, "cached menu becomes queue full");
    ui::update_build_buttons(buttons, gated_menu, credits(6000), {0, 0}, 5, 24);
    expect(buttons[1].enabled && buttons[1].state == ui::BuildButtonState::normal,
        "cached menu recovers its normal state");
    buttons[0].disabled_reason[0] = 'y';
    sync();
    expect(cache.buttons[0].disabled_reason == buttons[0].disabled_reason && card_builds == 2,
        "RG-04: a reason change rebuilds the owning tooltip once");
    buttons[1].enabled = false;
    sync();
    buttons[1].room = false;
    sync();
    buttons[1].price = 900;
    sync();
    buttons.pop_back();
    sync();
    expect(card_builds == 6, "menu state, price and size changes each rebuild once");
    cache.close();
    sync();
    expect(card_builds == 7, "reopening the same menu restores cards after selection mode");
    ui::BuildMenuCache empty;
    expect(empty.refresh({}) && !empty.refresh({}), "empty menus initialize once");
}

void test_warmed_pool_allocations() {
    tactical::EconomyView view;
    view.pool.assign(4096, x_wing);
    view.population_cap = 25;
    view.pool_additions = 4096;
    view.pool_addition_frame = 80;
    ui::PoolNotifications notification;
    ui::PoolLayoutCache cache;
    std::size_t visits{};
    const std::function<std::uint32_t(tactical::TypeId)> population_of = [](auto) { return 1U; };
    const auto update = [&] {
        notification.observe(view);
        if (cache.refresh(view)) {
            visits += view.pool.size();
            return ui::layout_pool(view.pool, view.population, view.population_cap, population_of);
        }
        return std::vector<ui::PoolSlot>{};
    };
    static_cast<void>(update());
    const auto warmed_visits = visits;
    allocation_probe::count = 0;
    allocation_probe::armed = true;
    for (std::size_t frame = 0; frame < 10000; ++frame) static_cast<void>(update());
    allocation_probe::armed = false;
    expect(allocation_probe::count == 0, "HUD: 10000 warmed nonempty-pool frames allocate zero");
    expect(visits == warmed_visits && cache.rebuilds == 1, "HUD: unchanged pool is never scanned again");
    expect(notification.additions == 4096 && notification.frame == 81, "HUD: unchanged notification phase stays fixed in completed ticks");
    ++view.pool_version;
    static_cast<void>(update());
    ++view.population;
    static_cast<void>(update());
    ++view.population_cap;
    static_cast<void>(update());
    expect(cache.rebuilds == 4, "HUD: pool, population and cap changes each invalidate once");
}

void test_warmed_pad_allocations() {
    auto pad_menu = menu();
    pad_menu.options.resize(ui::pad_slot_count, pad_menu.options.front());
    pad_menu.options[1].disabled_reason = std::string(256, 'g');
    pad_menu.options[1].available = false;
    std::vector<ui::BuildButton> buttons;
    const std::array<std::uint32_t, ui::pad_slot_count> rows{};
    ui::update_pad_buttons(buttons, pad_menu, credits(6000), 0, 0, 24, rows);
    const auto* storage = buttons.data();
    ui::BuildMenuCache cache;
    std::size_t metadata_builds{};
    const auto sync = [&] {
        if (!cache.refresh(buttons) || !cache.layout_changed) return;
        ++metadata_builds;
        std::vector<std::string> names;
        for (const auto& button : buttons) {
            names.push_back(std::string(128, 'n'));
            names.push_back(button.disabled_reason);
        }
    };
    sync();
    allocation_probe::count = 0;
    allocation_probe::armed = true;
    for (std::uint64_t frame = 0; frame < 10000; ++frame) {
        ui::update_pad_buttons(buttons, pad_menu, credits(6000), frame, 0, 24, rows);
        sync();
    }
    allocation_probe::armed = false;
    expect(allocation_probe::count == 0 && buttons.data() == storage && metadata_builds == 1
               && cache.layout_rebuilds == 1 && cache.state_updates == 0,
           "WBP-36: 10000 unchanged pad snapshots retain six-button storage and allocate zero");
    allocation_probe::count = 0;
    allocation_probe::armed = true;
    for (std::uint64_t frame = 0; frame < 10000; ++frame) {
        ui::update_pad_buttons(buttons, pad_menu, credits(6000), frame, 10000, 24, rows);
        sync();
    }
    allocation_probe::armed = false;
    expect(allocation_probe::count == 0 && buttons.data() == storage && metadata_builds == 1
               && cache.layout_rebuilds == 1 && cache.state_updates == 10000 && !buttons[0].enabled
               && buttons[0].cooldown_progress == 0.9999,
           "WBP-36: advancing recharge updates six buttons in place without allocation");
    ui::update_pad_buttons(buttons, pad_menu, credits(100), 10000, 10000, 24, rows);
    sync();
    expect(buttons[0].state == ui::BuildButtonState::unaffordable && !buttons[0].enabled,
           "WBP-36: retained pad buttons become unaffordable after recharge");
    ui::update_pad_buttons(buttons, pad_menu, credits(6000), 10000, 10000, 24, rows);
    sync();
    expect(buttons[0].state == ui::BuildButtonState::normal && buttons[0].enabled,
           "WBP-36: retained pad buttons recover their normal state");
    pad_menu.options[0].available = false;
    ui::update_pad_buttons(buttons, pad_menu, credits(6000), 10000, 10000, 24, rows);
    sync();
    expect(!buttons[0].enabled && metadata_builds == 1, "WBP-36: availability updates state without rebuilding names");
    pad_menu.options[0].type = y_wing;
    ui::update_pad_buttons(buttons, pad_menu, credits(6000), 10000, 10000, 24, rows);
    sync();
    expect(metadata_builds == 2 && cache.layout_rebuilds == 2, "WBP-36: a changed menu type rebuilds metadata once");
    cache.close();
    sync();
    expect(metadata_builds == 3, "WBP-36: reopening restores the retained menu once after ordinary selection");
}

void test_pad_station_transitions() {
    auto shared_menu = menu();
    std::vector<ui::BuildButton> buttons;
    ui::BuildMenuCache cache;
    ui::update_pad_buttons(buttons, shared_menu, credits(6000), 50, 100);
    expect(cache.refresh(buttons) && buttons[0].pad && buttons[0].cooldown_progress == 0.5,
           "WBP-36: transition starts with a cooling pad palette");
    ui::update_build_buttons(buttons, shared_menu, credits(6000), {0, 0}, 5, 24);
    expect(cache.refresh(buttons) && cache.layout_changed
               && std::all_of(buttons.begin(), buttons.end(), [](const auto& button) {
                   return !button.pad && button.cooldown_progress == 1.0;
               }), "WBP-36/PU-61: normal producer clears every retained pad tint, tooltip and dial flag");
    shared_menu.options[0].build_frames = 900;
    ui::update_build_buttons(buttons, shared_menu, credits(6000), {0, 0}, 5, 24);
    expect(cache.refresh(buttons) && !cache.layout_changed && buttons[0].build_frames == 900,
           "WPR-50: changed build duration refreshes the cached tooltip without rebuilding layout");
    shared_menu.options[0].build_frames = 450;
    ui::update_pad_buttons(buttons, shared_menu, credits(6000), 75, 100);
    expect(cache.refresh(buttons) && cache.layout_changed && buttons[0].pad
               && buttons[0].build_frames == 450
               && !buttons[0].enabled && buttons[0].cooldown_progress == 0.75,
           "WBP-36: returning to a pad restores its own cooldown state");
}

void test_capture_colorization() {
    constexpr std::array<std::uint8_t, 3> white{255, 255, 255};
    constexpr std::array<std::uint8_t, 3> blue{0, 128, 255};
    const auto neutral = ui::capture_colorization(0.0, false, white);
    const auto claiming = ui::capture_colorization(0.5, false, blue);
    const auto captured = ui::capture_colorization(0.0, true, blue);
    const auto neutralizing = ui::capture_colorization(0.5, true, blue);
    expect(neutral == std::array<float, 3>{1, 1, 1}, "WBP-52: neutral fallback preserves authored texels");
    expect(claiming[0] == 0.5F && claiming[2] == 1.0F && claiming[1] > 0.75F,
        "WBP-52: half capture retains the existing target fade");
    expect(captured[0] == 0.0F && captured[2] == 1.0F && captured[1] > 0.5F,
        "WBP-52: captured empty pad retains its owner's constant");
    expect(neutralizing == claiming, "WBP-52: neutralization reverses the same fade");
    allocation_probe::count = 0;
    allocation_probe::armed = true;
    for (std::size_t frame = 0; frame < 10000; ++frame) {
        const auto colour = ui::capture_colorization(static_cast<double>(frame) / 10000.0, false, blue);
        if (colour[2] != 1.0F) std::abort();
    }
    allocation_probe::armed = false;
    expect(allocation_probe::count == 0, "WBP-52: advancing capture colour allocates zero");
}

} // namespace

int main() {
    ui::PadPalette palette;
    expect(!palette.open(42, false) && !palette.entity(), "WBP-08: proximity/fog/alliance gate opening");
    const auto options = ui::layout_pad_buttons(menu(), credits(6000), 0, 0);
    expect(palette.open(42, true), "WBP-08: eligible empty allied pad opens");
    const auto build = palette.click(0, options);
    expect(build && build->first == 42 && build->second == x_wing && !palette.entity(),
        "WBP-36: build names the pad and UC type, then closes");
    palette.open(42, true);
    expect(!palette.click(2, options) && !palette.entity(), "WBP-36: a disabled click also closes");
    expect(ui::pad_pick_target(42, true, 43, 44) == 43 && ui::pad_pick_target(42, true, 0, 44) == 44
        && ui::pad_pick_target(42, false, 43, 44) == 42, "WBP-35: hidden-child pick substitution");
    expect(ui::capture_fill(0.25, true) == 0.75 && ui::capture_fill(0.25, false) == 0.25,
        "WBP-37: neutralization reverses owner fill");
    expect(ui::pad_time_progress(50, 0, 100) == 0.5 && ui::construction_alternate(0.1, 3) == 3
        && ui::construction_alternate(0.8, 3) == 1, "WBP-17/37: hull regression is separate from elapsed time");
    const auto pad = ui::layout_pad_buttons(menu(), credits(550), 100, 100);
    expect(pad.size() == 3 && pad[0].pad && pad[0].enabled && pad[1].enabled && !pad[2].enabled,
        "WBP-36: UC menu availability and exact affordable boundary");
    const auto cooling = ui::layout_pad_buttons(menu(), credits(6000), 99, 100);
    expect(!cooling[0].enabled && cooling[0].cooldown_progress == 0.99,
        "WBP-36: cooldown disables independently of queue room");
    const std::array<std::uint32_t, 3> rows{0, 0, 1};
    const auto authored = ui::layout_pad_buttons(menu(), credits(6000), 50, 100, 24, rows);
    expect(authored[0].slot == 0 && authored[1].slot == 2 && authored[2].slot == 5,
        "WBP-36: GUI_Row maps options to authored card columns");
    auto large_menu = menu();
    large_menu.options.resize(10, large_menu.options.front());
    expect(ui::layout_pad_buttons(large_menu, credits(6000), 0, 0, 24).size() == 6,
        "WBP-36: a pad exposes at most six authored card slots");
    test_buttons();
    test_menu_limits();
    test_queue();
    test_pool();
    test_texts();
    test_pool_notifications();
    test_warmed_pool_allocations();
    test_warmed_pad_allocations();
    test_pad_station_transitions();
    test_capture_colorization();
    test_warmed_menu_allocations();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " production check(s) failed\n";
        return 1;
    }
    std::cout << "production: all checks passed\n";
    return 0;
}

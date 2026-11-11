#include "eawr/presentation/hull_batching.hpp"
#include "ui_test_support.hpp"

#include <bit>
#include <string_view>

#include <algorithm>

namespace {
using namespace eawr::presentation::hull_batching;
using Change = Registry::Change;

Key key(std::uint64_t asset, float colour = -1.0F) {
    Key k;
    k.asset = asset;
    if (colour >= 0.0F) {
        k.has_colour = true;
        const auto bits = std::bit_cast<std::uint32_t>(colour);
        k.colour = {bits, bits, bits};
    }
    return k;
}
std::size_t count(const std::vector<Change>& changes, Change::Kind kind) {
    return static_cast<std::size_t>(std::count_if(changes.begin(), changes.end(), [kind](const Change& c) { return c.kind == kind; }));
}
} // namespace

int main() {
    using eawr::test::ui::expect;
    expect(enabled && launch_state(false) && !launch_state(true),
        "hull batching is on by default and the launch argument turns it off");
    expect(std::string_view(state_name(true)) == "on" && std::string_view(state_name(false)) == "off"
            && std::string_view(off_argument) == "--eawr-no-instanced-hulls",
        "the startup log names the actual state and the documented argument");
    {
        Registry registry;
        registry.observe(1, key(7), 0);
        registry.observe(2, key(7), 0);
        registry.observe(3, key(8), 0);
        expect(registry.reconcile(1).empty(), "pieces join only after they stayed eligible for join_after submits");
        const auto changes = registry.reconcile(2);
        expect(count(changes, Change::Kind::create) == 1 && count(changes, Change::Kind::join) == 2,
            "two matured identical pieces open one group; the lone key opens none");
        expect(registry.group_of(3) == 0 && registry.group_of(1) != 0 && registry.group_of(1) == registry.group_of(2),
            "same key shares a group, a different key stays single");
        expect(registry.slot_of(1) != registry.slot_of(2), "members get distinct slots");
        registry.observe(4, key(7), 2);
        expect(registry.reconcile(3).empty() && registry.group_of(4) == 0, "a late joiner waits its own hysteresis");
        const auto joined = registry.reconcile(4);
        expect(count(joined, Change::Kind::create) == 0 && count(joined, Change::Kind::join) == 1 && registry.members(registry.group_of(1)) == 3,
            "a matured piece joins the existing group without a second one");
    }
    {
        Registry registry;
        for (std::uint64_t e = 1; e <= 3; ++e) registry.observe(e, key(5), 0);
        static_cast<void>(registry.reconcile(2));
        const auto group = registry.group_of(1);
        registry.observe(1, key(5, 0.25F), 3);  // colour changed: leaves at once, swap-remove moves the last member
        const auto changes = registry.reconcile(3);
        expect(count(changes, Change::Kind::leave) == 1 && registry.group_of(1) == 0 && registry.members(group) == 2,
            "a state change leaves the group immediately");
        const auto leave = std::find_if(changes.begin(), changes.end(), [](const Change& c) { return c.kind == Change::Kind::leave; });
        expect(leave != changes.end() && leave->moved && registry.slot_of(*leave->moved) == leave->slot,
            "swap-remove reports the member moved into the freed slot");
        std::vector<std::uint32_t> slots{registry.slot_of(2), registry.slot_of(3)};
        std::sort(slots.begin(), slots.end());
        expect(slots == std::vector<std::uint32_t>{0, 1}, "slots stay dense after a leave");
    }
    {
        Registry registry;
        registry.observe(1, key(9), 0);
        registry.observe(2, key(9), 0);
        static_cast<void>(registry.reconcile(2));
        registry.observe(1, std::nullopt, 5, true);  // pose-driven exit: sticky
        static_cast<void>(registry.reconcile(5));
        registry.observe(1, key(9), 6);
        expect(registry.reconcile(8).empty() && registry.group_of(1) == 0, "a churn exit stays single for the sticky window");
        expect(registry.stats().sticky_blocks == 1, "sticky exits are counted");
        const auto later = registry.reconcile(5 + Config{}.sticky + 1);
        expect(count(later, Change::Kind::join) == 1 && registry.group_of(1) != 0, "the piece rejoins once the window passed");
    }
    {
        Registry registry;
        registry.observe(1, key(2), 0);
        registry.observe(2, key(2), 0);
        static_cast<void>(registry.reconcile(2));
        const auto group = registry.group_of(1);
        registry.forget(1);
        auto changes = registry.reconcile(3);
        expect(count(changes, Change::Kind::leave) == 1 && changes.front().forgotten && registry.members(group) == 1,
            "a retired piece leaves without restoring anything");
        registry.forget(2);
        changes = registry.reconcile(4);
        expect(count(changes, Change::Kind::drop) == 1 && registry.group_count() == 0 && registry.stats().groups_dropped == 1,
            "the last member leaving drops the group");
    }
    {
        Registry registry(Config{.join_after = 1, .min_members = 2, .sticky = 10, .max_joins = 3});
        for (std::uint64_t e = 1; e <= 8; ++e) registry.observe(e, key(4), 0);
        expect(count(registry.reconcile(1), Change::Kind::join) == 3, "joins per reconcile are capped");
        expect(count(registry.reconcile(2), Change::Kind::join) == 3 && count(registry.reconcile(3), Change::Kind::join) == 2 &&
            registry.members(registry.group_of(1)) == 8, "the remainder joins on later submits");
    }
    {
        Registry registry;
        registry.observe(1, key(3), 0);
        registry.observe(1, key(3), 1);  // unchanged key must not restart the hysteresis clock
        registry.observe(2, key(3), 0);
        expect(count(registry.reconcile(2), Change::Kind::join) == 2, "re-observing an unchanged key keeps its age");
    }
    {
        Registry registry;
        for (std::uint64_t tick = 0; tick < 10000; ++tick) {
            registry.observe(1, key(1 + tick % 2), tick);
            static_cast<void>(registry.reconcile(tick));
            expect(registry.maturing_count() <= 1, "key churn retains at most one maturing entry");
        }
        registry.forget(1);
        static_cast<void>(registry.reconcile(10001));
        expect(registry.maturing_count() == 0, "forgotten entity is pruned");
    }
    return eawr::test::ui::failures() == 0 ? 0 : 1;
}

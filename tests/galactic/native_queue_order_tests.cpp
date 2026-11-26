#include "native_production_admission.hpp"

#include <iostream>
#include <string>
#include <vector>

// WGA-509: which waiting AI entry a planet queue starts next. Pure queue-order contracts; the full
// session path is covered by the production and stock AI tests.
namespace gc = eawr::sim::galactic;
namespace sd = eawr::sim::galactic::session_detail;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAILED: " << what << '\n'; ++failures; }
}
gc::ProductionEntry entry(gc::TypeId type, gc::PlayerId builder, eawr::sim::EntityId id, std::int64_t seconds, std::uint64_t created, bool payg = true) {
    gc::ProductionEntry row;
    row.type = type; row.builder = builder; row.build_id = id; row.pay_as_you_go = payg;
    row.full_seconds = eawr::sim::math::Fixed::from_integer(seconds).value();
    row.last_remaining = row.full_seconds; row.start_frame = created;
    return row;
}
std::vector<gc::ProductionPlayer> players() {
    std::vector<gc::ProductionPlayer> rows(2);
    rows[0].player = 1; rows[0].human = true;
    rows[1].player = 2; rows[1].human = false; rows[1].pay_as_you_go = true;
    return rows;
}
std::vector<eawr::sim::EntityId> ids(const std::vector<gc::ProductionEntry>& queue) {
    std::vector<eawr::sim::EntityId> out;
    for (const auto& row : queue) out.push_back(row.build_id);
    return out;
}
} // namespace

int main() {
    const auto rows = players();
    // The Bespin case: the short structure order of one goal (admitted later) waits behind three ships of another goal.
    {
        std::vector<gc::ProductionEntry> queue{entry(10, 2, 1, 40, 657), entry(11, 2, 2, 30, 657), entry(12, 2, 3, 30, 657), entry(20, 2, 4, 60, 669)};
        sd::native_production_start_next(queue, rows, 1000);
        check(queue.front().build_id == 4, "structure order of the shorter goal starts before the ships of the longer goal");
        check((ids(queue) == std::vector<eawr::sim::EntityId>{4, 1, 2, 3}), "the other entries keep their arrival order");
        check(queue.front().start_frame == 1000, "the chosen entry starts at the given frame");
    }
    // Inside one goal's order the longest build goes first (estimate minus own build time).
    {
        std::vector<gc::ProductionEntry> queue{entry(10, 2, 1, 20, 500), entry(11, 2, 2, 50, 500), entry(12, 2, 3, 30, 500)};
        sd::native_production_start_next(queue, rows, 700);
        check((ids(queue) == std::vector<eawr::sim::EntityId>{2, 1, 3}), "within one order the longest build starts first");
    }
    // Equal slack keeps arrival order.
    {
        std::vector<gc::ProductionEntry> queue{entry(10, 2, 1, 30, 500), entry(11, 2, 2, 30, 600), entry(12, 2, 3, 30, 700)};
        sd::native_production_start_next(queue, rows, 800);
        check((ids(queue) == std::vector<eawr::sim::EntityId>{1, 2, 3}), "equal slack keeps arrival order");
    }
    // A queue whose next entry is a human's, or not pay-as-you-go, stays in arrival order.
    {
        std::vector<gc::ProductionEntry> queue{entry(10, 1, 1, 90, 500), entry(11, 2, 2, 10, 600)};
        sd::native_production_start_next(queue, rows, 800);
        check((ids(queue) == std::vector<eawr::sim::EntityId>{1, 2}), "a human's entry first: arrival order");
        std::vector<gc::ProductionEntry> manual{entry(10, 2, 1, 90, 500, false), entry(11, 2, 2, 10, 600)};
        sd::native_production_start_next(manual, rows, 800);
        check((ids(manual) == std::vector<eawr::sim::EntityId>{1, 2}), "a non pay-as-you-go entry first: arrival order");
    }
    // Entries of other builders do not count as the same goal and human entries are never promoted.
    {
        std::vector<gc::ProductionEntry> queue{entry(10, 2, 1, 40, 657), entry(11, 1, 2, 5, 657), entry(12, 2, 3, 20, 700)};
        sd::native_production_start_next(queue, rows, 900);
        check(queue.front().build_id == 3 || queue.front().build_id == 1, "only AI entries compete");
        check(queue.front().build_id != 2, "a human's entry is not promoted ahead of AI entries");
    }
    // One entry: just started.
    {
        std::vector<gc::ProductionEntry> queue{entry(10, 2, 1, 40, 657)};
        sd::native_production_start_next(queue, rows, 900);
        check(queue.front().start_frame == 900, "a single entry starts at the given frame");
    }
    if (failures) return 1;
    std::cout << "native queue order contracts passed\n";
    return 0;
}

#include "eawr/sim/tactical/land_placement.hpp"
#include "land_test_support.hpp"
#include "../support/check.hpp"

#include <iostream>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
using sim::math::Fixed;
testing::Checker checks;
void expect(const bool condition, const char* message) { checks.check(condition, message, std::cerr); }

void common_class() {
    auto field = make_field({"..", ".."});
    const std::vector<std::uint32_t> masks{hover, infantry, walker};
    expect(land_common_mask(masks) == walker, "WLPA-12: the group mask is the members' intersection");
    expect(land_is_authored_class(*field.terrain, walker), "the intersection is an authored class here");
    const std::vector<std::uint32_t> crossing{hover, infantry};
    expect(land_common_mask(crossing) == clear, "water and infantry-only ground leave only clear ground");
    expect(!land_common_mask({}), "an empty group has no mask");
    const std::vector<std::uint32_t> odd{water, infantry_only};
    expect(land_common_mask(odd) == 0u && !land_is_authored_class(*field.terrain, 0), "WLPA-12: an unauthored intersection is its own failure");
}

void zones() {
    auto field = make_field({
        "..#..",
        "..#..",
        "..#..",
        "..#..",
    });
    const auto left = LandZones::build(field.occupancy, walker);
    expect(left.zone_count() == 2, "a full wall splits the ground into two zones");
    expect(!left.connected(0, 4) && left.connected(0, 1 * 5 + 1) && left.zone_of(2) == 0, "zones separate the sides; a closed cell is zone 0");
    auto lake = make_field({"..~..", "..~..", "..~..", "..~.."});
    expect(LandZones::build(lake.occupancy, walker).zone_count() == 2 && LandZones::build(lake.occupancy, hover).zone_count() == 1,
        "WLPA-13: a class that crosses water joins the shores");
    auto diagonal = make_field({".#", "#."});
    expect(LandZones::build(diagonal.occupancy, walker).zone_count() == 2, "a diagonal join needs both orthogonal neighbours");

    // The cache rebuilds a class only when the occupancy revision moved.
    auto open = make_field({"....", "....", "...."});
    LandZoneCache cache;
    const std::vector<std::uint32_t> wanted{walker};
    cache.prepare(open.occupancy, wanted);
    expect(cache.find(walker) && cache.find(walker)->zone_count() == 1 && !cache.find(hover), "the cache holds the prepared class only");
    sim::InlineExecutor executor;
    const std::vector<LandOccupation> wall_column{{1, wall, 0, {1, 5, 9}, {}}};
    expect(bool(open.occupancy.commit(wall_column, executor)), "wall committed");
    expect(cache.find(walker)->zone_count() == 1, "the cache is stale until prepared");
    cache.prepare(open.occupancy, wanted);
    expect(cache.find(walker)->zone_count() == 2 && !cache.find(walker)->connected(0, 3), "prepare rebuilds the class after a commit");
}

void padding_and_width() {
    expect(land_placement_padding(whole(8), whole(0)) == whole(2), "WLPA-24: a quarter of the hard radius");
    expect(land_placement_padding(whole(2), Fixed::from_ratio(1, 2).value()) == Fixed::from_raw(Fixed::scale / 2 * 6),
        "0.3 times the spacing times 20: a spacing of 0.5 gives 3");
    expect(land_placement_padding(whole(0), whole(0)) == whole(0), "no radius and no spacing give no padding");
    expect(land_formation_width(1, whole(3), whole(9)) == whole(3), "WLPA-29: one member is its own extent");
    expect(land_formation_width(4, whole(2), whole(8)) == Fixed::from_raw(Fixed::scale * 17 / 4),
        "four members take two rows of the largest plus a quarter when the area term is smaller");
    const auto crowded = land_formation_width(4, whole(2), whole(16));
    expect(crowded > Fixed::from_raw(Fixed::scale * 5) && crowded < Fixed::from_raw(Fixed::scale * 6), "a large area sum takes the area term, sqrt(28.8)");
    expect(land_formation_width(9, whole(1), whole(0)) == Fixed::from_raw(Fixed::scale * 13 / 4), "three rows plus a quarter; the area floor is one");
    expect(land_formation_width(0, whole(1), whole(1)) == whole(0), "no members, no width");
}

class RejectNear final : public LandPlacementFilter {
public:
    explicit RejectNear(const sim::math::Vec2 centre) : centre_(centre) {}
    bool rejects(const sim::math::Vec2 position) const override {
        const auto dx = position.x.raw() - centre_.x.raw(), dy = position.y.raw() - centre_.y.raw();
        return dx > -(15 * Fixed::scale) && dx < 15 * Fixed::scale && dy > -(15 * Fixed::scale) && dy < 15 * Fixed::scale;
    }
private:
    sim::math::Vec2 centre_;
};

void placement() {
    auto field = make_field({
        "...............",
        "...............",
        "...............",
        "....#####......",
        "....#####......",
        "....#####......",
        "...............",
        "...............",
        "...............",
    });
    LandPlacementQuery query;
    query.mask = walker;
    query.from = centre(1, 1);
    query.desired = centre(2, 2);
    auto exact = land_find_open_position(field.occupancy, query);
    expect(exact.status == LandPlacementStatus::exact && exact.position == query.desired && exact.tries == 1, "a legal destination is kept");

    // A destination inside the block moves out of it, toward the mover's side.
    query.desired = centre(6, 4);
    const auto moved = land_find_open_position(field.occupancy, query);
    expect(moved.status == LandPlacementStatus::adjusted && moved.tries > 1, "a blocked destination moves");
    const auto column = land_cell_of(moved.position.x, whole(20), 15), row = land_cell_of(moved.position.y, whole(20), 9);
    expect(column && row && field.occupancy.admits(*column, *row, walker), "the new position is on open ground");
    expect(moved.position.x < query.desired.x && moved.position.y < query.desired.y, "the first legal sample lies on the mover's side");
    expect(land_find_open_position(field.occupancy, query).position == moved.position, "the search is deterministic");

    // A wide object needs more room: the footprint plus padding must fit.
    LandPlacementQuery wide = query;
    wide.desired = centre(3, 4);
    wide.radius = whole(14);
    const auto roomy = land_find_open_position(field.occupancy, wide);
    expect(roomy.status == LandPlacementStatus::adjusted, "a wide object moves off the wall's edge");
    const auto wide_column = land_cell_of(roomy.position.x, whole(20), 15), wide_row = land_cell_of(roomy.position.y, whole(20), 9);
    const auto padded = land_footprint(Fixed::from_raw(wide.radius.raw() + land_placement_padding(wide.radius, wide.formation_spacing).raw()), whole(20));
    expect(wide_column && wide_row && field.occupancy.admits_footprint(*wide_column + padded.low, *wide_row + padded.low,
            *wide_column + padded.high, *wide_row + padded.high, walker), "the whole padded footprint is open");

    // The caller's filter rejects candidates after the grid gates.
    RejectNear near_filter(centre(2, 2));
    LandPlacementQuery filtered;
    filtered.mask = walker;
    filtered.from = centre(0, 0);
    filtered.desired = centre(2, 2);
    filtered.filter = &near_filter;
    const auto away = land_find_open_position(field.occupancy, filtered);
    expect(away.status == LandPlacementStatus::adjusted && !near_filter.rejects(away.position), "the filter pushes the position out");

    // The mover's zone gates candidates (WLPA-25): a pocket the mover cannot reach is refused.
    auto pocket = make_field({
        "...#........",
        "...#........",
        "...#........",
        "...#........",
    });
    LandZoneCache cache;
    const std::vector<std::uint32_t> wanted{walker};
    cache.prepare(pocket.occupancy, wanted);
    LandPlacementQuery gated;
    gated.mask = walker;
    gated.from = centre(1, 1);
    gated.desired = centre(8, 1);
    gated.zones = cache.find(walker);
    const auto cut_off = land_find_open_position(pocket.occupancy, gated);
    expect(cut_off.status == LandPlacementStatus::none && cut_off.position == gated.desired && cut_off.tries == land_placement_tries,
        "a destination in another zone has no legal position and keeps the request");
    gated.zones = nullptr;
    expect(land_find_open_position(pocket.occupancy, gated).status == LandPlacementStatus::exact, "without the zone gate the cut-off side is fine");

    // Nothing within twenty rings: a large closed block.
    auto closed = make_field({
        "..............................",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        ".############################.",
        "..............................",
    });
    LandPlacementQuery buried;
    buried.mask = walker;
    buried.from = centre(0, 0);
    buried.desired = centre(15, 8);
    expect(land_find_open_position(closed.occupancy, buried).status == LandPlacementStatus::none, "a destination deeper than the rings reach has no position");
}
} // namespace

int main() {
    common_class(); zones(); padding_and_width(); placement();
    return checks.finish();
}

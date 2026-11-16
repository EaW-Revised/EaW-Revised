// Retreat escape flight contracts (FS-R07, FC-R04, FC-R05, FS-R09 object fade).
//
// Exercises include/eawr/presentation/camera/retreat_flight.hpp through its public API. The frame numbers, the
// 8.125 step and the 0.2 second brightening are the debug-build rules the header cites; the faction increment and
// wait, the geometry and the ships are test data.

#include "../../support/check.hpp"
#include "eawr/presentation/camera/retreat_flight.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace camera = eawr::presentation::camera;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}

void expect_close(const float actual, const float expected, const std::string_view message, const float tolerance = 1e-3F) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << message << " (expected " << expected << ", got " << actual << ")\n";
        ++failures;
    }
}

constexpr camera::RetreatFlightRates stock{500.0F, 1}; // Rebel and Underworld author 500 and 1

void flight_steps_follow_the_prefix() {
    // FSC-R02: below frame 30 every member takes 8.125; frame 30 accelerates member 0 only, 31 members 0..1, ...
    for (std::uint32_t call = 1; call < 30; ++call)
        for (std::size_t n = 0; n < 3; ++n)
            expect_close(camera::retreat_flight_step(n, 3, call, stock), 8.125F, "below 30 every member takes the base step");
    expect_close(camera::retreat_flight_step(0, 3, 30, stock), 500.0F, "k=30 member 0 accelerates");
    expect_close(camera::retreat_flight_step(1, 3, 30, stock), 8.125F, "k=30 member 1 does not yet");
    expect_close(camera::retreat_flight_step(1, 3, 31, stock), 500.0F, "k=31 member 1 accelerates");
    expect_close(camera::retreat_flight_step(2, 3, 31, stock), 8.125F, "k=31 member 2 does not yet");
    expect_close(camera::retreat_flight_step(2, 3, 32, stock), 500.0F, "k=32 member 2 accelerates");
    expect_close(camera::retreat_flight_step(0, 3, 89, stock), 500.0F, "k=89 still moves");
    expect_close(camera::retreat_flight_step(0, 3, 90, stock), 0.0F, "k=90 no flight movement");
    expect_close(camera::retreat_flight_step(0, 3, 400, stock), 0.0F, "no movement afterwards");
}

void stock_wait_accelerates_at_thirty_plus_n() {
    for (std::size_t n = 0; n < 40; ++n) {
        const auto at = camera::retreat_flight_accelerates_at(n, 60, stock);
        expect(at && *at == 30 + n, "stock wait 1: member n accelerates at k=30+n");
    }
    expect(camera::retreat_flight_accelerates_at(59, 60, stock) == std::optional<std::uint32_t>(89), "member 59 is the last that accelerates");
    expect(!camera::retreat_flight_accelerates_at(60, 100, stock), "a member behind frame 89 never accelerates");
}

void modded_wait_spaces_the_prefix() {
    // FSC-R05: wait 3 and three members accelerate at k=30, 31 and 34.
    const camera::RetreatFlightRates slow{500.0F, 3};
    expect(camera::retreat_flight_accelerates_at(0, 3, slow) == std::optional<std::uint32_t>(30), "wait 3 member 0 at 30");
    expect(camera::retreat_flight_accelerates_at(1, 3, slow) == std::optional<std::uint32_t>(31), "wait 3 member 1 at 31");
    expect(camera::retreat_flight_accelerates_at(2, 3, slow) == std::optional<std::uint32_t>(34), "wait 3 member 2 at 34");
    expect_close(camera::retreat_flight_step(1, 3, 33, slow), 500.0F, "member 1 keeps accelerating while the wait runs");
    expect_close(camera::retreat_flight_step(2, 3, 33, slow), 8.125F, "member 2 waits until k=34");
}

void prefix_stops_at_the_last_member() {
    // FC-R04: capped at the last member; a one-ship roster accelerates from 30 and never indexes past itself.
    expect_close(camera::retreat_flight_step(0, 1, 80, stock), 500.0F, "single member keeps the increment");
    expect(camera::retreat_flight_accelerates_at(0, 1, stock) == std::optional<std::uint32_t>(30), "single member accelerates at 30");
}

void distance_adds_the_steps() {
    expect_close(camera::retreat_flight_distance(0, 3, 0, stock), 0.0F, "no distance at k=0");
    expect_close(camera::retreat_flight_distance(0, 3, 29, stock), 29.0F * 8.125F, "k=29: twenty-nine base steps");
    expect_close(camera::retreat_flight_distance(0, 3, 30, stock), 29.0F * 8.125F + 500.0F, "k=30: member 0 takes the increment");
    expect_close(camera::retreat_flight_distance(1, 3, 30, stock), 30.0F * 8.125F, "k=30: member 1 still takes the base step");
    expect_close(camera::retreat_flight_distance(1, 3, 31, stock), 30.0F * 8.125F + 500.0F, "k=31: member 1 accelerates");
    const float end = camera::retreat_flight_distance(0, 3, 89, stock);
    expect_close(end, 29.0F * 8.125F + 60.0F * 500.0F, "k=89: sixty accelerated steps", 0.05F);
    expect_close(camera::retreat_flight_distance(0, 3, 90, stock), end, "k=90 adds nothing");
    expect_close(camera::retreat_flight_distance(0, 3, 150, stock), end, "the flight holds after 90");
    // The presented time between two frames only interpolates the next call's step.
    expect_close(camera::retreat_flight_distance(0, 3, 10, stock, 0.5F), 10.0F * 8.125F + 0.5F * 8.125F, "half a frame of base step");
    expect_close(camera::retreat_flight_distance(0, 3, 29, stock, 0.5F), 29.0F * 8.125F + 250.0F, "half a frame into the accelerated step");
    expect_close(camera::retreat_flight_distance(0, 3, 89, stock, 0.9F), end, "no interpolation past the last call");
}

void engines_brighten_in_roster_order() {
    expect(!camera::retreat_engine_brightness(0, 19), "member 0 has not started at k=19");
    expect_close(*camera::retreat_engine_brightness(0, 20), 0.2F, "member 0 starts from the at-rest brightness at k=20");
    expect(!camera::retreat_engine_brightness(5, 24), "member 5 starts at k=25");
    expect(camera::retreat_engine_brightness(5, 25).has_value(), "member 5 started at k=25");
    expect_close(*camera::retreat_engine_brightness(0, 23), 0.2F + 0.8F * 0.5F, "half way after three frames (0.2 s is six frames)");
    expect_close(*camera::retreat_engine_brightness(0, 26), 1.0F, "full after 0.2 seconds");
    expect_close(*camera::retreat_engine_brightness(0, 200), 1.0F, "it holds");
    expect_close(*camera::retreat_engine_brightness(0, 20, 0.5F), 0.2F + 0.8F * (0.5F / 6.0F), "the fraction moves it between frames");
    expect(!camera::retreat_engine_brightness(70, 300), "a member whose start is not before frame 90 never brightens");
    expect(camera::retreat_engine_brightness(69, 89).has_value(), "member 69 starts at k=89");
}

void object_fade_follows_the_camera_distance() {
    expect_close(camera::retreat_object_fade(5000.0F, 10000.0F, 11000.0F), 1.0F, "inside the fade begin");
    expect_close(camera::retreat_object_fade(10000.0F, 10000.0F, 11000.0F), 1.0F, "at the fade begin");
    expect_close(camera::retreat_object_fade(10500.0F, 10000.0F, 11000.0F), 0.5F, "half way");
    expect_close(camera::retreat_object_fade(11000.0F, 10000.0F, 11000.0F), 0.0F, "gone at the fade end");
    expect_close(camera::retreat_object_fade(20000.0F, 10000.0F, 11000.0F), 0.0F, "gone beyond it");
    expect_close(camera::retreat_object_fade(10500.0F, 10000.0F, 10000.0F), 0.0F, "a degenerate range cuts at the begin");
}

void front_ships_leave_the_view_by_the_end_of_the_flight() {
    // 500 per accelerated call against the fade 10000..11000 from the camera at the marker.
    const std::size_t count = 20;
    for (std::size_t n = 0; n < count; ++n) {
        const float distance = camera::retreat_flight_distance(n, count, 89, stock);
        expect(distance > 11000.0F, "every ship of a twenty-ship roster ends beyond the object fade");
    }
}

camera::RetreatFlightGeometry geometry() {
    camera::RetreatFlightGeometry value;
    value.entry = {100.0F, -200.0F, 7.0F};
    value.departure_direction = {-1.0F, 0.0F, 0.0F};
    return value;
}

void staging_fits_ships_around_the_marker() {
    std::vector<camera::RetreatFlightMember> members;
    for (std::uint64_t id = 1; id <= 12; ++id) members.push_back({id, 120.0F, 1000.0F + static_cast<float>(id)});
    const auto plan = camera::plan_retreat_flight(members, geometry());
    expect(plan.size() == members.size(), "every member is staged");
    for (const auto& ship : plan) {
        const float dx = ship.staged[0] - 100.0F;
        const float dy = ship.staged[1] + 200.0F;
        expect(std::sqrt(dx * dx + dy * dy) <= 2500.0F + 1e-2F, "staged within 2500 units of the marker");
        expect_close(ship.staged[2], 7.0F, "staged on the marker's height");
    }
    for (std::size_t a = 0; a < plan.size(); ++a)
        for (std::size_t b = a + 1; b < plan.size(); ++b) {
            const float dx = plan[a].staged[0] - plan[b].staged[0];
            const float dy = plan[a].staged[1] - plan[b].staged[1];
            expect(std::sqrt(dx * dx + dy * dy) >= 240.0F - 1e-2F, "no two ships overlap");
        }
}

void staging_puts_the_strongest_ship_at_the_marker() {
    const std::vector<camera::RetreatFlightMember> members{{1, 100.0F, 500.0F}, {2, 100.0F, 9000.0F}, {3, 100.0F, 500.0F}};
    const auto plan = camera::plan_retreat_flight(members, geometry());
    const auto strongest = std::find_if(plan.begin(), plan.end(), [](const auto& ship) { return ship.id == 2; });
    expect(strongest != plan.end(), "the strongest ship is staged");
    if (strongest != plan.end()) {
        expect_close(strongest->staged[0], 100.0F, "the strongest ship takes the marker");
        expect_close(strongest->staged[1], -200.0F, "the strongest ship takes the marker");
    }
}

void departure_order_is_nearest_the_probe_first() {
    // The probe sits 10000 units along the departure direction (-X) from the marker.
    const std::vector<camera::RetreatFlightMember> members{{1, 100.0F, 3000.0F}, {2, 100.0F, 2000.0F}, {3, 100.0F, 1000.0F}};
    const auto plan = camera::plan_retreat_flight(members, geometry());
    expect(plan.size() == 3, "three ships");
    const auto probe_distance = [](const camera::RetreatFlightShip& ship) {
        const float dx = ship.staged[0] - (100.0F - 10000.0F);
        const float dy = ship.staged[1] + 200.0F;
        return std::sqrt(dx * dx + dy * dy);
    };
    for (std::size_t index = 1; index < plan.size(); ++index)
        expect(probe_distance(plan[index - 1]) <= probe_distance(plan[index]) + 1e-3F, "ships leave in order of distance to the probe");
}

void staging_is_deterministic_and_empty_is_empty() {
    expect(camera::plan_retreat_flight({}, geometry()).empty(), "no members, no plan");
    const std::vector<camera::RetreatFlightMember> members{{4, 80.0F, 10.0F}, {5, 80.0F, 10.0F}, {6, 80.0F, 10.0F}};
    const auto first = camera::plan_retreat_flight(members, geometry());
    const auto second = camera::plan_retreat_flight(members, geometry());
    expect(first.size() == second.size(), "same size");
    for (std::size_t index = 0; index < first.size(); ++index)
        expect(first[index].id == second[index].id && first[index].staged == second[index].staged, "same plan on every call");
}

void overfull_roster_still_stages_everyone() {
    std::vector<camera::RetreatFlightMember> members;
    for (std::uint64_t id = 1; id <= 40; ++id) members.push_back({id, 900.0F, 100.0F});
    const auto plan = camera::plan_retreat_flight(members, geometry());
    expect(plan.size() == 40, "ships that do not fit still stand somewhere inside the radius");
    for (const auto& ship : plan) {
        const float dx = ship.staged[0] - 100.0F;
        const float dy = ship.staged[1] + 200.0F;
        expect(std::sqrt(dx * dx + dy * dy) <= 2500.0F + 1e-1F, "inside the staging radius");
    }
}

void facing_follows_the_departure_direction() {
    expect_close(camera::retreat_flight_yaw_degrees({-1.0F, 0.0F, 0.0F}), 180.0F, "facing -X is yaw 180");
    expect_close(camera::retreat_flight_yaw_degrees({0.0F, 1.0F, 0.0F}), 90.0F, "facing +Y is yaw 90");
}

} // namespace

int main() {
    flight_steps_follow_the_prefix();
    stock_wait_accelerates_at_thirty_plus_n();
    modded_wait_spaces_the_prefix();
    prefix_stops_at_the_last_member();
    distance_adds_the_steps();
    engines_brighten_in_roster_order();
    object_fade_follows_the_camera_distance();
    front_ships_leave_the_view_by_the_end_of_the_flight();
    staging_fits_ships_around_the_marker();
    staging_puts_the_strongest_ship_at_the_marker();
    departure_order_is_nearest_the_probe_first();
    staging_is_deterministic_and_empty_is_empty();
    overfull_roster_still_stages_everyone();
    facing_follows_the_departure_direction();
    if (failures != 0) {
        std::cerr << failures << " retreat flight check(s) failed\n";
        return 1;
    }
    std::cout << "retreat flight contracts passed\n";
    return 0;
}

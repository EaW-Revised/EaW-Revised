#include "galactic/galactic_tour.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <optional>
#include <set>
#include <vector>

// #6291 (GCT-01..GCT-12): the galactic cinematic tour's maths, on synthetic planets only.
using namespace eawr::viewer::galactic;

namespace {
std::vector<TourPlanet> planets(int count) {
    std::vector<TourPlanet> list;
    for (int i = 0; i < count; ++i)
        list.push_back({static_cast<ObjectId>(100 + i), Vec3{static_cast<float>(i) * 300.0F, static_cast<float>((i * 7) % 5) * 200.0F, 0.0F}, 15.0F});
    return list;
}
float distance(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}
} // namespace

int case_galactic_tour_tests() {
    constexpr double key = GalacticTour::seconds_per_key;
    {
        // DEV-13: the Space binding. Off (the default setting) is the original's: the tour on release, never a pause. On: a
        // pause on press, never the tour. Exactly one of the two for either setting.
        constexpr SpaceKey off = space_key(false), on = space_key(true);
        static_assert(!off.pauses && off.starts_tour);
        static_assert(on.pauses && !on.starts_tour);
        assert(off.pauses != off.starts_tour && on.pauses != on.starts_tour);
    }
    {
        // GCT-01: the tour plays only on the map, running, with no prompt up.
        assert(tour_may_play({true, false, false}));
        assert(!tour_may_play({false, false, false})); // a battle or the loading screen
        assert(!tour_may_play({true, true, false}));   // paused: the bands would hide the pause banner and its Resume Game button
        assert(!tour_may_play({true, false, true}));   // a battle prompt (the original refuses while a battle is resolved)
        assert(!tour_may_play({}));
        // GCT-01: every modal of the one set stops the start, one case per member (GalaxyModals has eleven; its size check
        // forces this table to grow with it).
        constexpr bool GalaxyModals::*members[] = {&GalaxyModals::retreat_dialog, &GalaxyModals::save_menu, &GalaxyModals::campaign_score,
            &GalaxyModals::battle_prompt, &GalaxyModals::story_summary, &GalaxyModals::reference_pages, &GalaxyModals::hero_menu,
            &GalaxyModals::black_market, &GalaxyModals::weapon_menu, &GalaxyModals::corruption_menu, &GalaxyModals::action_menu};
        static_assert(sizeof(members) / sizeof(members[0]) == 11U);
        assert(!GalaxyModals{}.any());
        for (const auto member : members) {
            GalaxyModals modals;
            modals.*member = true;
            assert(modals.any());
            assert(!tour_may_play({true, false, modals.any()}));
        }
    }
    {
        // GCT-02: nothing to visit with fewer than two planets or no extent.
        TourRandom random(1);
        assert(!GalacticTour::begin({}, std::nullopt, 1000.0F, random));
        assert(!GalacticTour::begin(planets(1), std::nullopt, 1000.0F, random));
        assert(!GalacticTour::begin(planets(5), std::nullopt, 0.0F, random));
        assert(GalacticTour::begin(planets(2), std::nullopt, 1000.0F, random));
    }
    {
        // GCT-03/04: every planet but the first of the shuffled list gets three keys, and the tour lasts 21 s each.
        TourRandom random(7);
        const auto tour = GalacticTour::begin(planets(9), std::nullopt, 2000.0F, random);
        assert(tour && tour->visited().size() == 8U);
        assert(std::abs(tour->duration() - 8.0 * 3.0 * key) < 1e-9);
        const std::set<ObjectId> distinct(tour->visited().begin(), tour->visited().end());
        assert(distinct.size() == 8U); // none twice
    }
    {
        // GCT-12: the selected planet is moved to the front and is the one not visited first.
        TourRandom random(3);
        const auto tour = GalacticTour::begin(planets(6), ObjectId{103}, 2000.0F, random);
        assert(tour && tour->visited().size() == 5U);
        for (const ObjectId id : tour->visited()) assert(id != 103);
    }
    {
        // GCT-05: at the planet's own slot start the eye is the galaxy extent away on a 15..30 degree line, at the
        // middle key it is five extent lengths away in the plane, and the target is the planet throughout the slot.
        TourRandom random(11);
        const auto all = planets(5);
        const float galaxy = 2500.0F;
        const auto tour = GalacticTour::begin(all, std::nullopt, galaxy, random);
        assert(tour);
        const ObjectId first = tour->visited().front();
        const auto planet = *std::find_if(all.begin(), all.end(), [&](const TourPlanet& p) { return p.id == first; });
        const TourPose start = tour->pose(0.0);
        assert(std::abs(distance(start.eye, planet.position) - galaxy) < 0.5F);
        assert(start.eye.z - planet.position.z > 0.0F); // above the plane
        const float lift = (start.eye.z - planet.position.z) / galaxy;
        assert(lift >= std::sin(15.0F * 3.14159265F / 180.0F) - 1e-3F && lift <= std::sin(30.0F * 3.14159265F / 180.0F) + 1e-3F);
        assert(start.target == Vec3(planet.position));
        const TourPose close = tour->pose(key);
        assert(std::abs(distance(close.eye, planet.position) - planet.extent * GalacticTour::close_pass_extents) < 0.5F);
        assert(std::abs(close.eye.z - planet.position.z) < 1e-3F);
        const TourPose opposite = tour->pose(2.0 * key);
        // GCT-05: the third key is the first mirrored through the planet.
        assert(std::abs((opposite.eye.x - planet.position.x) + (start.eye.x - planet.position.x)) < 0.5F);
        assert(std::abs((opposite.eye.z - planet.position.z) + (start.eye.z - planet.position.z)) < 0.5F);
        assert(tour->visiting(0.0) == first && tour->visiting(3.0 * key - 0.1) == first);
        assert(tour->visiting(3.0 * key + 0.1) == tour->visited()[1]);
    }
    {
        // GCT-12: the eased path is continuous, passes each key at its time and stays between the keys' extremes.
        TourRandom random(5);
        const auto tour = GalacticTour::begin(planets(4), std::nullopt, 1800.0F, random);
        assert(tour);
        TourPose previous = tour->pose(0.0);
        for (double t = 0.05; t <= tour->duration() + 1.0; t += 0.05) {
            const TourPose now = tour->pose(t);
            assert(distance(now.eye, previous.eye) < 400.0F); // no jump between two samples
            previous = now;
        }
        // Held at the last key to the end, and before the start.
        assert(tour->pose(tour->duration()) == tour->pose(tour->duration() + 50.0));
        assert(tour->pose(-4.0) == tour->pose(0.0));
    }
    {
        // GCT-06: the same seed gives the same tour; another seed differs.
        TourRandom a(21), b(21), c(22);
        const auto one = GalacticTour::begin(planets(7), std::nullopt, 2000.0F, a);
        const auto two = GalacticTour::begin(planets(7), std::nullopt, 2000.0F, b);
        const auto other = GalacticTour::begin(planets(7), std::nullopt, 2000.0F, c);
        assert(one && two && other && one->visited() == two->visited() && one->pose(30.0) == two->pose(30.0));
        assert(one->visited() != other->visited() || !(one->pose(30.0) == other->pose(30.0)));
    }
    return 0;
}

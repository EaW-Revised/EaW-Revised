#include "eawr/presentation/render_categories.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>

namespace categories = eawr::presentation::render_categories;
namespace {
struct Piece final {
    categories::Role role{categories::Role::unknown};
    std::uint64_t last_submit{};
    bool casts{};
    std::uint64_t surfaces{};
};
struct Fixture final {
    std::vector<Piece> pieces;
    mutable std::size_t reads{};
    std::uint64_t viewport{7};
    categories::Source source{this, [](const void* owner, categories::Sample& sample) {
        const auto& fixture = *static_cast<const Fixture*>(owner);
        ++fixture.reads;
        for (const auto& piece : fixture.pieces)
            sample.record(piece.role, piece.last_submit == sample.frame, piece.casts, piece.surfaces);
    }, [](const void* owner) { return static_cast<const Fixture*>(owner)->viewport; }};
};
void expect(bool condition, const char* message) { eawr::test::ui::expect(condition, message); }
}

int main() {
    Fixture main;
    main.pieces = {{categories::Role::hull, 10, true, 3}, {categories::Role::hardpoint, 10, true, 1},
                   {categories::Role::effect, 9, false, 2}, {categories::Role::unknown, 10, false, 0}};
    categories::enabled = false;
    auto sample = categories::Source::capture(10, 7);
    expect(main.reads == 0 && sample.roles[0].retained == 0, "disabled captures never enumerate sources");
    categories::enabled = true;
    Fixture other_viewport;
    other_viewport.viewport = 8;
    other_viewport.pieces = {{categories::Role::hull, 10, true, 99}};
    {
        Fixture particles;
        particles.pieces = {{categories::Role::particle, 10, false, 1}};
        sample = categories::Source::capture(10, 7);
        expect(other_viewport.reads == 0, "root sample does not traverse other viewport instances");
        std::uint64_t submitted{}, retained{}, casters{}, surfaces{};
        for (const auto& role : sample.roles) {
            submitted += role.submitted; retained += role.retained;
            casters += role.eligible_casters; surfaces += role.retained_surfaces;
        }
        expect(submitted == 4 && retained == 5 && casters == 2 && surfaces == 7,
            "independent role subtotals reconcile; caster eligibility overlaps provenance");
        // A repeat capture reads the same stored per-instance frame stamp.
        const auto repeat = categories::Source::capture(10, 7);
        expect(repeat.roles[1].submitted == 1, "repeated capture preserves the stamped submission count");
    }
    sample = categories::Source::capture(11, 7);
    expect(sample.roles[4].retained == 0, "backend teardown removes its source");
    expect(sample.roles[1].submitted == 0 && sample.roles[1].retained == 1,
        "frame reset forgets submissions but preserves retained geometry");
    main.pieces.erase(main.pieces.begin() + 1);
    sample = categories::Source::capture(11, 7);
    expect(sample.roles[2].retained == 0 && sample.roles[2].eligible_casters == 0,
        "retirement removes both retained geometry and eligibility");
    sample.record(static_cast<categories::Role>(255), false, false, 0);
    expect(sample.roles[0].retained == 2, "unrecognized provenance stays in unknown bucket");
    std::ostringstream json;
    categories::write_json(json, sample);
    expect(json.str().find("\"viewport_draws\"") != std::string::npos
        && json.str().find("\"retained_surfaces\"") != std::string::npos,
        "report names separate actual draws and retained surfaces");

    // A warmed diagnostic traversal, not a GPU/frame speedup. Production also
    // records the complete sampler cost (including viewport queries) per frame.
    main.pieces.assign(10000, {categories::Role::hardpoint, 12, true, 1});
    std::vector<double> durations;
    durations.reserve(200);
    for (int repeat = 0; repeat < 200; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        const auto measured = categories::Source::capture(12, 7);
        const auto stop = std::chrono::steady_clock::now();
        expect(measured.roles[2].retained == 10000, "benchmark consumes its result");
        durations.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    std::sort(durations.begin(), durations.end());
    std::cout << "render category warmed traversal, 10000 instances: median_ms=" << durations[100]
              << " p95_ms=" << durations[190] << '\n';
    categories::enabled = false;
    return eawr::test::ui::failures() == 0 ? 0 : 1;
}

#include "eawr/presentation/render_categories.hpp"
#include "eawr/presentation/hull_surface_diagnostic.hpp"
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
    expect(categories::authored_fighter(" AntiFighter, fighter "), "authored fighter token accepted case insensitively");
    expect(categories::authored_fighter("BOMBER"), "bomber is included");
    expect(!categories::authored_fighter("ANTI_FIGHTER FIGHTERHERO") && !categories::authored_fighter(""), "names and token substrings do not imply fighter class");
    categories::ablation = "fighter-control";
    expect(!categories::fighter_hidden(true, categories::Role::hull), "fighter control keeps every hull");
    categories::ablation = "fighter-hull";
    expect(categories::fighter_hidden(true, categories::Role::hull)
        && !categories::fighter_hidden(false, categories::Role::hull), "hull ablation changes only authored fighter owners");
    expect(!categories::fighter_hidden(true, categories::Role::hardpoint), "hull ablation retains separately composed fighter hardpoints");
    categories::ablation = "fighter-all";
    expect(categories::fighter_hidden(true, categories::Role::effect)
        && !categories::fighter_hidden(false, categories::Role::effect), "full owner ablation retains other owners' effects");
    categories::ablation = "fighter-shadows";
    expect(!categories::engine_casts(true) && !categories::fighter_hidden(true, categories::Role::hull), "global shadow ceiling retains colour geometry");
    categories::ablation = "off";
    categories::begin_fighter_frame(1);
    categories::fighter_work.idle_samples = 3;
    const auto first_pose = categories::Source::capture(1, 7);
    expect(first_pose.fighter_pose.idle_samples == 3, "pose work belongs to its producing frame");
    expect(categories::Source::capture(2, 7).fighter_pose.idle_samples == 0, "a frame without pose work must not reuse old counts");
    categories::begin_fighter_frame(2);
    expect(first_pose.fighter_pose.idle_samples == 3 && categories::fighter_work.idle_samples == 0, "captured pose work is immutable across frames");
    std::vector<eawr::assets::Bone> bones(4);
    bones[1].parent = 0;
    bones[2].parent = 1;
    bones[3].parent = 0;
    const std::array<std::size_t, 1> roots{1};
    eawr::assets::Mesh mesh{};
    eawr::assets::Submesh surface{};
    mesh.bone = 2;
    expect(categories::hull_part(mesh, surface, bones, roots) == categories::HullPart::turret,
        "declared turret descendants carry integrated ownership without name heuristics");
    mesh.bone = 3;
    expect(categories::hull_part(mesh, surface, bones, roots) == categories::HullPart::body,
        "a rigid surface outside declared roots belongs to other hull");
    expect(categories::hull_part(mesh, surface, bones, {}) == categories::HullPart::unclassified,
        "absence of declared roots cannot prove a hull split");
    expect(categories::hull_part(mesh, surface, bones, roots, false) == categories::HullPart::unclassified,
        "unresolved declared roots cannot prove a hull split");
    surface.skin_bones = {2, 3};
    eawr::assets::Vertex vertex{};
    vertex.bone_indices = {0, 1, 0, 0};
    vertex.bone_weights = {1, 0, 0, 0};
    surface.vertices = {vertex};
    expect(categories::hull_part(mesh, surface, bones, roots) == categories::HullPart::turret,
        "skinning confined to declared turret descendants stays attributable");
    surface.vertices[0].bone_weights = {0.5F, 0.5F, 0, 0};
    expect(categories::hull_part(mesh, surface, bones, roots) == categories::HullPart::unclassified,
        "mixed turret/body influences remain unknown");
    surface.skin_bones.clear();
    bones[3].parent = 3;
    expect(categories::hull_part(mesh, surface, bones, roots) == categories::HullPart::unclassified,
        "ancestry cycles remain unknown");
    expect(std::abs(categories::projected_diameter(1, 101, 0.1, 1) - 0.01) < 1e-9,
        "sphere diameter is a viewport-height fraction and uses nearest depth");
    expect(categories::projected_diameter(1, 1, 0.1, 1) < 0, "near-plane crossing keeps geometry");
    expect(categories::projected_diameter(1, -20, 0.1, 1) < 0, "behind-camera geometry cannot enter a band");
    expect(categories::projected_diameter(0, 20, 0.1, 1) < 0, "missing bounds cannot enter a band");
    expect(categories::projected_diameter(1, 101, 0.1, 1, 101) >= 0.02,
        "off-axis perspective expansion cannot underestimate projected diameter");
    categories::ablation = "hardpoint-band-010";
    expect(categories::band_hidden(0.009, false), "band enters below projected diameter cutoff");
    expect(!categories::band_hidden(0.01, false), "band does not enter at boundary");
    expect(categories::band_hidden(0.0105, true), "band keeps hidden state inside hysteresis");
    expect(!categories::band_hidden(0.0111, true), "band exits above hysteresis boundary");
    expect(!categories::band_hidden(-1, true) && !categories::band_hidden(0, true), "unknown and empty bounds are conservative");
    expect(categories::engine_visible(categories::Role::hardpoint, true), "band selection does not globally hide the category");
    for (const auto name : categories::experiments) expect(categories::valid_ablation(name), "all registered experiments accepted");
    expect(categories::fighter_impostor_small(7.9 / 720.0, 720), "quad bound enters below eight pixels");
    expect(!categories::fighter_impostor_small(8.0 / 720.0, 720), "quad boundary keeps authored geometry");
    expect(!categories::fighter_impostor_small(-1, 720) && !categories::fighter_impostor_small(0.001, 0),
        "quad selection keeps unknown bounds and missing camera dimensions");
    categories::ablation = "off";
    expect(!categories::band_hidden(0.0001, true), "flag off clears diagnostic hysteresis");
    categories::ablation = "hull";
    expect(!categories::engine_visible(categories::Role::hull, true), "hull ablation suppresses an otherwise visible hull");
    expect(categories::engine_visible(categories::Role::hardpoint, true), "hull ablation preserves attachments");
    expect(!categories::engine_visible(categories::Role::hardpoint, false), "ablation never shows logically hidden art");
    categories::ablation = "none";
    expect(categories::engine_visible(categories::Role::hull, true), "control restores requested visible state");
    expect(!categories::engine_visible(categories::Role::hull, false), "control preserves requested hidden state");
    categories::ablation = "shadows";
    expect(!categories::engine_casts(true) && !categories::engine_casts(false), "shadow ablation preserves noncasters");
    categories::ablation = "off";
    expect(categories::engine_casts(true) && !categories::engine_casts(false), "off restores exact caster policy");
    expect(!categories::valid_ablation("projectile"), "incomplete producer paths are not advertised as separable");
    categories::Sample geometry;
    geometry.record(categories::Role::hull, true, true, 4, 90, 4);
    expect(geometry.roles[1].retained_triangles == 90 && geometry.roles[1].material_incidence == 4
        && geometry.roles[1].potential_shadow_surfaces == 4, "producer geometry and single shadow-pass estimate reconcile");
    Fixture main;
    main.pieces = std::vector<Piece>{{categories::Role::hull, 10, true, 3}, {categories::Role::hardpoint, 10, true, 1},
                   {categories::Role::effect, 9, false, 2}, {categories::Role::unknown, 10, false, 0}};
    categories::enabled = false;
    auto sample = categories::Source::capture(10, 7);
    expect(main.reads == 0 && sample.roles[0].retained == 0, "disabled captures never enumerate sources");
    categories::enabled = true;
    Fixture other_viewport;
    other_viewport.viewport = 8;
    other_viewport.pieces = std::vector<Piece>{{categories::Role::hull, 10, true, 99}};
    {
        Fixture particles;
        particles.pieces = std::vector<Piece>{{categories::Role::particle, 10, false, 1}};
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
    expect(json.str().find("rest_pose_batching") == std::string::npos, "flag-off report keeps the production schema");
    {
        categories::Sample yield;
        yield.t1_groups = 2; yield.t1_collapsed_surfaces = 30; yield.t1_collapsed_shadow_surfaces = 12;
        categories::ablation = "none";
        std::ostringstream diagnostic;
        categories::write_json(diagnostic, yield);
        categories::ablation = "off";
        expect(diagnostic.str().find("\"rest_pose_batching\":{\"visible_pieces\":0") != std::string::npos
            && diagnostic.str().find("\"collapsed_surfaces\":30,\"collapsed_shadow_surfaces\":12") != std::string::npos
            && diagnostic.str().back() == '}', "diagnostic report carries the rest-pose batching yield");
    }

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

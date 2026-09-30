// CPU contracts for the opt-in space fog admission and visibility rules
// (apps/viewer/src/space_fog.*, P1-07 #28). No engine, no assets.
#include "space_fog.hpp"

#include "eawr/scene/scene.hpp"
#include "eawr/sim/fog.hpp"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace eawr;
namespace sf = presentation::godot_backend::space_fog;

namespace {

scene::Placement placement(const std::uint64_t ordinal, std::string object, const bool transform,
                           std::vector<std::pair<std::string, bool>> surfaces,
                           std::vector<scene::Issue> issues = {}) {
    scene::Placement result;
    result.scene_ordinal = ordinal;
    result.object_id = std::move(object);
    result.asset_id = 1;
    if (transform) result.transform = scene::Transform{};
    for (auto& [shader, supported] : surfaces) {
        scene::Surface surface;
        surface.shader = shader;
        surface.supported = supported;
        result.surfaces.push_back(surface);
    }
    result.issues = std::move(issues);
    return result;
}

} // namespace

int main() {
    // Admission list validation: explicit, unique XML element names only.
    assert(!sf::validate_admission({}).empty());
    const std::vector<std::string> unit{"SpaceUnit"};
    assert(sf::validate_admission(unit).empty());
    for (const std::string bad : {"", "Space-Unit", "Space Unit", "<SpaceUnit>", "Space\xC3\xA9"}) {
        const std::vector<std::string> types{bad};
        assert(!sf::validate_admission(types).empty());
    }
    const std::vector<std::string> twice{"SpaceUnit", "Squadron", "SpaceUnit"};
    assert(sf::validate_admission(twice).find("declared twice") != std::string::npos);
    std::vector<std::string> many;
    for (std::size_t index = 0; index < sf::max_admitted_types; ++index) many.push_back("T" + std::to_string(index));
    assert(sf::validate_admission(many).empty());
    many.push_back("Extra");
    assert(!sf::validate_admission(many).empty());

    // Classification: only declared element types, composed whole or blocked.
    scene::Scene built;
    built.placements.push_back(placement(0, "UNIT_A", true, {{"BatchMeshGloss.fx", true}}));
    built.placements.push_back(placement(1, "PROP_B", true, {{"BatchMeshGloss.fx", true}}));
    built.placements.push_back(placement(2, "", true, {{"BatchMeshGloss.fx", true}}));
    built.placements.push_back(placement(3, "UNIT_C", true,
        {{"BatchMeshGloss.fx", true}, {"MeshShadowVolume.fx", false}},
        {{scene::Cause::shader_unsupported, "MeshShadowVolume.fx"}}));
    built.placements.push_back(placement(4, "UNIT_D", false, {{"BatchMeshGloss.fx", true}}));
    built.placements.push_back(placement(5, "UNKNOWN_E", true, {{"BatchMeshGloss.fx", true}}));
    built.placements.push_back(placement(6, "UNIT_F", true, {{"EawrInvented.fx", true}}));
    built.placements.push_back(placement(7, "LOWER_G", true, {{"BatchMeshGloss.fx", true}}));
    // A legacy selector that is not a fog consumer is admitted here; the
    // renderer's declare_fog_consumer refuses it at composition (EAWR-FOG-0006).
    built.placements.push_back(placement(8, "UNIT_H", true, {{"MeshGloss.fx", true}}));
    std::vector<std::string> looked_up;
    const auto type_of = [&](const std::string_view id) -> std::optional<std::string> {
        looked_up.emplace_back(id);
        if (id.starts_with("UNIT_")) return std::string("SpaceUnit");
        if (id == "PROP_B") return std::string("SpaceProp");
        if (id == "LOWER_G") return std::string("spaceunit");
        return std::nullopt;
    };
    const auto decisions = sf::classify(built, type_of, unit);
    assert(decisions.size() == built.placements.size());
    using D = sf::Decision;
    const std::array<D, 9> expected{D::admitted, D::not_admitted, D::uncatalogued, D::admitted_blocked,
                                    D::admitted_blocked, D::uncatalogued, D::admitted_blocked, D::not_admitted,
                                    D::admitted};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        assert(decisions[index].decision == expected[index]);
        assert(decisions[index].scene_ordinal == index);
    }
    // An empty object id never reaches the catalog lookup.
    for (const auto& id : looked_up) assert(!id.empty());
    assert(decisions[0].reasons.empty() && decisions[0].type_name == "SpaceUnit");
    assert(decisions[1].type_name == "SpaceProp" && decisions[1].reasons.empty());
    assert(decisions[2].type_name.empty());
    // Case matters: the element name is compared exactly.
    assert(decisions[7].type_name == "spaceunit");
    const auto& blocked = decisions[3].reasons;
    assert(blocked.size() == 2);
    assert(blocked[0] == "shader_unsupported MeshShadowVolume.fx");
    assert(blocked[1].find("surface 1 shader MeshShadowVolume.fx") != std::string::npos);
    assert(decisions[4].reasons.front() == "placement is not drawable");
    // A supported-looking surface without a legacy selector is blocked.
    assert(decisions[6].reasons.size() == 1 && decisions[6].reasons[0].find("EawrInvented.fx") != std::string::npos);
    assert(sf::to_string(D::admitted_blocked) == "admitted_blocked");
    // Nothing admitted: every catalogued placement is not_admitted.
    const std::vector<std::string> squadron{"Squadron"};
    for (const auto& decision : sf::classify(built, type_of, squadron)) {
        assert(decision.decision == D::not_admitted || decision.decision == D::uncatalogued);
    }

    // Visibility: half-open cells from origin (-150, 100), 100 x 100, 3 x 2.
    constexpr std::int64_t q = 1LL << 24;
    const sim::fog::FogGridDesc desc{.team_id = 2, .width = 3, .height = 2,
        .origin_x_raw = -150 * q, .origin_y_raw = 100 * q, .cell_x_raw = 100 * q, .cell_y_raw = 100 * q,
        .revision = 1};
    const std::array<std::uint8_t, 6> cells{255, 60, 128, 200, 90, 0};
    const auto grid = sim::fog::FogGrid::create(desc, cells);
    assert(grid);
    const auto* g = &grid.value();
    assert(sf::can_reveal(g, {-90, -10, 140, 160}));   // cells (0,0) and (1,0)
    assert(!sf::can_reveal(g, {60, 90, 240, 260}));    // only (2,1) = 0
    assert(!sf::can_reveal(g, {50, 90, 240, 260}));    // x = 50 belongs to column 2
    assert(sf::can_reveal(g, {49.5, 90, 240, 260}));   // touches column 1 (90)
    assert(!sf::can_reveal(g, {160, 240, 280, 300}));  // right of the grid: dark
    assert(!sf::can_reveal(g, {-200, -150, 140, 160})); // ends on the left edge
    assert(!sf::can_reveal(g, {0, 20, 20, 100}));      // ends on the bottom edge
    assert(sf::can_reveal(g, {-400, 400, -400, 400})); // covers everything
    assert(!sf::can_reveal(g, {10, -10, 140, 160}));   // inverted bounds
    assert(!sf::can_reveal(nullptr, {-90, -10, 140, 160}));
    const std::array<std::uint8_t, 6> zero{};
    const auto dark = sim::fog::FogGrid::create(desc, zero);
    assert(dark && !sf::can_reveal(&dark.value(), {-400, 400, -400, 400}));

    // Max edges are exclusive at interior edges too. Rows [60, 160) and
    // [160, 260); only column 2 of row 0 and all of row 1 are nonzero.
    const sim::fog::FogGridDesc edge_desc{.team_id = 2, .width = 3, .height = 2,
        .origin_x_raw = -150 * q, .origin_y_raw = 60 * q, .cell_x_raw = 100 * q, .cell_y_raw = 100 * q,
        .revision = 1};
    const std::array<std::uint8_t, 6> edge_cells{0, 0, 50, 200, 200, 200};
    const auto edge_grid = sim::fog::FogGrid::create(edge_desc, edge_cells);
    assert(edge_grid);
    const auto* e = &edge_grid.value();
    assert(!sf::can_reveal(e, {-90, -10, 140, 160}));    // ends on the row 1 edge: row 0 only
    assert(sf::can_reveal(e, {-90, -10, 140, 160.5}));   // crosses into row 1
    assert(!sf::can_reveal(e, {-90, 50, 70, 150}));      // ends on the column 2 edge
    assert(sf::can_reveal(e, {-90, 50.5, 70, 150}));     // crosses into column 2
    assert(sf::can_reveal(e, {-90, -10, 160, 160}));     // a degenerate extent is its point, in row 1
    assert(!sf::can_reveal(e, {-90, -10, 100, 100}));    // the same point rule in the zero row
    assert(sf::can_reveal(e, {50, 50, 100, 100}));       // a point on an edge belongs to the cell above it
    assert(!sf::can_reveal(e, {150, 150, 100, 100}));    // a point on the grid's far edge is outside
    return 0;
}

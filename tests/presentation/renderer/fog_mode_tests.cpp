#include "fog_mode.hpp"

#include "eawr/sim/fog.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/world.hpp"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace eawr;

int main() {
    namespace sf = sim::fog;
    const auto path = std::filesystem::temp_directory_path() / "eawr-map-fog-mode-contract";
    std::filesystem::create_directories(path);
    const sf::FogGridDesc a{.team_id = 2, .width = 3, .height = 2,
        .origin_x_raw = -5 * (1LL << 24), .origin_y_raw = 7 * (1LL << 24),
        .cell_x_raw = 2 * (1LL << 24), .cell_y_raw = 3 * (1LL << 23),
        .revision = 1};
    const sf::FogGridDesc b{.team_id = 7, .width = 3, .height = 2,
        .origin_x_raw = -5 * (1LL << 24), .origin_y_raw = 7 * (1LL << 24),
        .cell_x_raw = 2 * (1LL << 24), .cell_y_raw = 3 * (1LL << 23),
        .revision = 1};
    const std::array<std::uint8_t, 6> a_cells{0, 32, 64, 128, 192, 255};
    const std::array<std::uint8_t, 6> b_cells{255, 192, 128, 64, 32, 0};
    const auto grid_a = sf::FogGrid::create(a, a_cells);
    const auto grid_b = sf::FogGrid::create(b, b_cells);
    assert(grid_a && grid_b);
    const auto write = [&](const std::string& name, const sf::FogGrid& grid) {
        const auto bytes = grid.canonical_bytes();
        const auto file = path / name;
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        return file;
    };
    const auto file_a = write("a.eawr-fog", grid_a.value());
    const auto file_b = write("b.eawr-fog", grid_b.value());
    std::string error;
    auto loaded = presentation::godot_backend::FogMode::load({file_a, file_b}, 2, 47, error);
    assert(loaded && error.empty());
    auto& mode = *loaded;
    std::ifstream replay_file(EAWR_REPLAY_FIXTURE_PATH, std::ios::binary);
    std::vector<std::uint8_t> replay_bytes{
        std::istreambuf_iterator<char>(replay_file), std::istreambuf_iterator<char>()};
    const auto replay = sim::parse_replay(replay_bytes);
    assert(replay);
    auto world = sim::World::create(replay.value());
    assert(world);
    const auto world_hash = world.value().state_sha256();
    const auto world_snapshot = world.value().snapshot();
    const auto source_hash = mode.source().find(2)->sha256();
    const sim::RenderInstance instance{.entity_id = 42, .asset_id = 6};
    const auto initial = mode.snapshot({instance});
    assert(initial->completed_tick() == 47 && initial->fog_grids().find(2));
    assert(mode.set_painting(true));
    assert(mode.paint_cell(1, 0, 255));
    const auto painted = mode.snapshot({instance});
    assert(painted->fog_grids().find(2)->cell(1, 0) == 255);
    assert(painted->fog_grids().find(2)->revision() == 2);
    assert(initial->fog_grids().find(2)->cell(1, 0) == 32);
    assert(initial->instances().size() == 1 && painted->instances()[0] == instance);
    assert(world.value().state_sha256() == world_hash);
    sf::FogGridDesc maximum = a;
    maximum.revision = std::numeric_limits<std::uint64_t>::max();
    const auto max_grid = sf::FogGrid::create(maximum, a_cells);
    assert(max_grid);
    const auto max_file = write("max.eawr-fog", max_grid.value());
    auto max_mode = presentation::godot_backend::FogMode::load({max_file}, 2, 47, error);
    assert(max_mode && max_mode->set_painting(true));
    assert(max_mode->paint_cell(1, 0, 255));
    assert(max_mode->stream() > 1 && max_mode->effective().find(2)->revision() == 1);
    assert(world.value().snapshot() == world_snapshot);
    assert(mode.source().find(2)->sha256() == source_hash);
    assert(mode.set_painting(false));
    assert(!mode.override_active() && mode.stream() > 1);
    assert(mode.snapshot({})->fog_grids().find(2)->cell(1, 0) == 32);
    assert(mode.set_team(7));
    assert(mode.team() == 7 && mode.snapshot({})->fog_grids().find(7)->cell(0, 0) == 255);
    mode.lock_capture();
    assert(!mode.set_painting(true) && !mode.paint_cell(1, 0, 0) && !mode.set_team(2));
    assert(!mode.override_active());
    assert(mode.snapshot({})->fog_grids().find(7)->sha256() == grid_b.value().sha256());
    assert(world.value().state_sha256() == world_hash);
    std::filesystem::remove(file_a);
    std::filesystem::remove(file_b);
    std::filesystem::remove(max_file);
    std::filesystem::remove(path);
}

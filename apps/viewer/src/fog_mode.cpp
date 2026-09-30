#include "fog_mode.hpp"

#include "eawr/sim/replay.hpp"

#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <utility>

namespace eawr::presentation::godot_backend {

FogMode::FogMode(sim::fog::FogGridSet source, std::vector<Source> sources,
    const std::uint32_t team, const std::uint64_t tick)
    : source_(std::move(source)), sources_(std::move(sources)), team_(team), tick_(tick) {}

std::optional<FogMode> FogMode::load(const std::vector<std::filesystem::path>& paths,
    const std::uint32_t team, const std::uint64_t tick, std::string& error) {
    if (paths.empty()) {
        error = "fog requires at least one canonical --eawr-fog-grid file";
        return std::nullopt;
    }
    std::vector<sim::fog::FogGrid> grids;
    std::vector<Source> sources;
    for (const auto& path : paths) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = "cannot read fog grid: " + path.string();
            return std::nullopt;
        }
        // A canonical grid is bounded by the validated grid contract; refuse
        // a larger file before copying it into the parser.
        file.seekg(0, std::ios::end);
        const auto length = file.tellg();
        if (length < 0 || static_cast<std::uint64_t>(length)
                > sim::fog::grid_header_size + sim::fog::max_collection_cells) {
            error = "fog grid file exceeds the canonical limit: " + path.string();
            return std::nullopt;
        }
        file.seekg(0);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) {
            error = "cannot read full fog grid: " + path.string();
            return std::nullopt;
        }
        auto parsed = sim::fog::parse_fog_grid(bytes, path.string());
        if (!parsed) {
            error = "invalid fog grid " + path.string();
            return std::nullopt;
        }
        sources.push_back({path, sim::sha256_hex(bytes), parsed.value().team_id(),
            parsed.value().revision()});
        grids.push_back(std::move(parsed.value()));
    }
    auto set = sim::fog::FogGridSet::create(std::move(grids));
    if (!set || !set.value().find(team)) {
        error = !set ? "fog grid teams must be unique and ordered" : "selected fog team has no grid";
        return std::nullopt;
    }
    return FogMode(std::move(set.value()), std::move(sources), team, tick);
}

void FogMode::clear_override() noexcept {
    changed_ = false;
    cells_.clear();
    paint_revision_ = 0;
}

void FogMode::lock_capture() noexcept {
    fixed_capture_ = true;
    painting_ = false;
    clear_override();
    ++stream_;
}

bool FogMode::set_team(const std::uint32_t team) noexcept {
    if (fixed_capture_ || !source_.find(team)) return false;
    if (team_ == team) return true;
    team_ = team;
    clear_override();
    ++stream_;
    return true;
}

bool FogMode::set_painting(const bool enabled) noexcept {
    if (fixed_capture_) return false;
    if (painting_ == enabled) return true;
    if (!enabled && changed_) ++stream_;
    painting_ = enabled;
    clear_override();
    return true;
}

bool FogMode::paint_cell(const std::uint32_t x, const std::uint32_t y,
    const std::uint8_t value) {
    if (fixed_capture_ || !painting_) return false;
    const auto* grid = source_.find(team_);
    if (!grid || x >= grid->desc().width || y >= grid->desc().height) return false;
    const std::size_t index = static_cast<std::size_t>(y) * grid->desc().width + x;
    if (!changed_ && grid->cells()[index] == value) return true;
    if (!changed_) {
        cells_.assign(grid->cells().begin(), grid->cells().end());
        changed_ = true;
        paint_revision_ = grid->revision();
    }
    if (cells_[index] == value) return true;
    if (paint_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        ++stream_;
        paint_revision_ = 0;
    }
    cells_[index] = value;
    ++paint_revision_;
    return true;
}

sim::fog::FogGridSet FogMode::effective() const {
    if (!override_active()) return source_;
    std::vector<sim::fog::FogGrid> grids;
    for (const auto& grid : source_.grids()) {
        if (grid.team_id() != team_) {
            grids.push_back(grid);
            continue;
        }
        auto desc = grid.desc();
        desc.revision = paint_revision_;
        auto painted = sim::fog::FogGrid::create(desc, cells_);
        if (painted) grids.push_back(std::move(painted.value()));
    }
    auto set = sim::fog::FogGridSet::create(std::move(grids));
    return set ? std::move(set.value()) : source_;
}

std::shared_ptr<const sim::RenderSnapshot> FogMode::snapshot(
    std::vector<sim::RenderInstance> instances) const {
    return std::make_shared<const sim::RenderSnapshot>(tick_, std::move(instances), effective());
}

} // namespace eawr::presentation::godot_backend

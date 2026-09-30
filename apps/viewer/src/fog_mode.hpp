#pragma once

#include "eawr/sim/fog.hpp"
#include "eawr/sim/snapshot.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// Map-owned presentation state. The parsed source grids are never edited.
class FogMode final {
public:
    struct Source final {
        std::filesystem::path path;
        std::string sha256;
        std::uint32_t team{};
        std::uint64_t revision{};
    };

    [[nodiscard]] static std::optional<FogMode> load(
        const std::vector<std::filesystem::path>& paths, std::uint32_t team,
        std::uint64_t tick, std::string& error);
    [[nodiscard]] const sim::fog::FogGridSet& source() const noexcept { return source_; }
    [[nodiscard]] const std::vector<Source>& sources() const noexcept { return sources_; }
    [[nodiscard]] std::uint32_t team() const noexcept { return team_; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    [[nodiscard]] std::uint64_t stream() const noexcept { return stream_; }
    [[nodiscard]] bool override_active() const noexcept { return painting_ && changed_; }
    [[nodiscard]] bool painting() const noexcept { return painting_; }
    [[nodiscard]] bool fixed_capture() const noexcept { return fixed_capture_; }
    void lock_capture() noexcept;
    [[nodiscard]] bool set_team(std::uint32_t team) noexcept;
    [[nodiscard]] bool set_painting(bool enabled) noexcept;
    [[nodiscard]] bool paint_cell(std::uint32_t x, std::uint32_t y, std::uint8_t value);
    [[nodiscard]] sim::fog::FogGridSet effective() const;
    [[nodiscard]] std::shared_ptr<const sim::RenderSnapshot> snapshot(
        std::vector<sim::RenderInstance> instances) const;

private:
    FogMode(sim::fog::FogGridSet source, std::vector<Source> sources,
        std::uint32_t team, std::uint64_t tick);
    void clear_override() noexcept;
    sim::fog::FogGridSet source_;
    std::vector<Source> sources_;
    std::uint32_t team_{};
    std::uint64_t tick_{};
    std::uint64_t stream_{1};
    bool fixed_capture_{};
    bool painting_{};
    bool changed_{};
    std::vector<std::uint8_t> cells_;
    std::uint64_t paint_revision_{};
};

} // namespace eawr::presentation::godot_backend

#pragma once

#include "galactic_mode.hpp"
#include "planet_search_widget.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace eawr::presentation::godot_backend {

// PS-08: the search rows' live planet pictures. Each onscreen row gets a cell
// of one shared offscreen atlas in which the planet's own galaxy model turns on
// the galaxy animation clock; the row icon is that cell of the live texture.
// Created on the first open of the search; it does no work in a frame in which
// no row asks for a picture (both viewports stay disabled then).
class PlanetThumbnails final : public PlanetSearchThumbnails {
public:
    explicit PlanetThumbnails(GalacticMode::State& state);
    ~PlanetThumbnails() override;
    PlanetThumbnails(const PlanetThumbnails&) = delete;
    PlanetThumbnails& operator=(const PlanetThumbnails&) = delete;

    godot::Ref<godot::Texture2D> thumbnail(std::uint64_t planet, std::int32_t text_height) override;
    [[nodiscard]] std::string report_json() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::presentation::godot_backend

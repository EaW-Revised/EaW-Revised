#pragma once

// Private support for the space contract runner (space_tests.cpp), which keeps the
// helper definitions; the cases are grouped into space_{surface,geometry,effect}_tests.cpp.

#include "eawr/presentation/space/space.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr_space_test {

namespace space = eawr::presentation::space;
using eawr::assets::Vec3f;

extern int failures;

void expect(const bool condition, const std::string& message);

eawr::assets::Source source(const std::string& path);

eawr::assets::Map space_map();

eawr::assets::Submesh quad(const Vec3f origin, const Vec3f u_axis, const Vec3f v_axis,
                           const std::string& shader, const std::string& texture);

constexpr const char* qualified_shader = "EawrSyntheticOpaqueDiffuse.fx";

eawr::assets::Model sky_model();

eawr::assets::Texture rgba_texture(const std::string& path, const std::uint8_t red);

struct Harness final {
    eawr::assets::Map map = space_map();
    eawr::assets::ObjectTypeRef type;
    eawr::assets::Model model = sky_model();
    std::vector<std::string> texture_requests;
    space::ModelLookup::Status model_status{space::ModelLookup::Status::resolved};
    std::optional<std::string> missing_texture;
    std::optional<std::string> undecodable_texture;
    std::optional<eawr::assets::PixelFormat> texture_format;
    std::optional<eawr::assets::Texture> supplied_texture;
    std::span<const space::MaterialRouteRow> routes;

    Harness() {
        type.logical_name = "EAWR_SPACE_PRIMARY_SKY";
        type.source.logical_path = "data/xml/eawr_space_objects.xml";
        type.source.line = 3;
        type.space_model_name = "eawr_space_sky.alo";
        type.model_name = "eawr_space_sky_generic.alo";
    }

    space::SkyPlan plan() {
        space::PlanInput input;
        input.map = &map;
        input.sky_type = &type;
        input.catalog_loaded = true;
        input.qualifications = space::qualifications();
        input.material_routes = routes;
        input.model = [this](const std::string_view name) {
            space::ModelLookup lookup;
            lookup.status = model_status;
            lookup.logical_path = "data/art/models/" + std::string(name);
            lookup.sha256 = std::string(64, 'a');
            if (model_status == space::ModelLookup::Status::resolved) lookup.model = model;
            else lookup.failure = "synthetic model failure";
            return lookup;
        };
        input.texture = [this](const std::string_view name) {
            texture_requests.emplace_back(name);
            space::TextureLookup lookup;
            lookup.logical_path = "data/art/textures/" + std::string(name);
            if (missing_texture && *missing_texture == name) {
                lookup.status = space::TextureLookup::Status::not_in_vfs;
                return lookup;
            }
            if (undecodable_texture && *undecodable_texture == name) {
                lookup.status = space::TextureLookup::Status::failed_to_decode;
                lookup.failure = "synthetic decode failure";
                return lookup;
            }
            lookup.status = space::TextureLookup::Status::resolved;
            lookup.sha256 = std::string(64, 'c');
            eawr::assets::Texture texture = rgba_texture(lookup.logical_path,
                static_cast<std::uint8_t>(texture_requests.size() * 100));
            if (supplied_texture) texture = *supplied_texture;
            if (texture_format) texture.format = *texture_format;
            lookup.texture = texture;
            return lookup;
        };
        return space::build_plan(input);
    }
};

// space_surface_tests.cpp
void test_environment_selection();
void test_model_selection();
void test_two_surface_plan();
void test_surface_rejections();
void test_texture_normalisation();

// space_geometry_tests.cpp
void test_camera();
void test_projection_and_basis();
void test_morphology();
void test_pixel_evaluation();
void test_sun_reference_geometry();

// space_effect_tests.cpp
void test_meshgloss_route();
void test_meshgloss_arithmetic();
void test_meshadditive_route();
void test_meshadditive_arithmetic();
void test_environment_effect_routes();
void test_environment_effect_clock();

} // namespace eawr_space_test

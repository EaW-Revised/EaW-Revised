#include "live_fog_view.hpp"

#include "stored_output.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace eawr::presentation::godot_backend {

using namespace godot;

namespace {

// FW-02, FW-03, FW-04, FW-13: the plane's colour is the fog texture's (the cell's ramp colour),
// its alpha the fog texture's alpha times the grid tile's; the tile repeats 200 times across the
// fog rectangle. Blended over what is behind it without writing depth, so units above the plane
// hide it and the backdrop shows through the gaps. FW-15: where an opaque surface already lies
// behind the plane (the planet, backdrop objects) the plane draws nothing, as the retail stills
// show the planet over the fog; the sky writes no depth and is fogged. Stored-value colour
// (docs/rendering.md).
constexpr std::string_view fog_plane_shader = R"(shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, fog_disabled, shadows_disabled;
uniform sampler2D eawr_fog_cells : filter_linear, repeat_disable;
uniform sampler2D eawr_fog_grid : filter_linear_mipmap, repeat_enable;
uniform vec2 eawr_fog_top_left = vec2(0.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform float eawr_fog_grid_repeats = 200.0;
uniform sampler2D eawr_fog_depth : hint_depth_texture, filter_nearest;
uniform bool eawr_fog_reverse_z = true;
varying vec2 eawr_fog_source;
void vertex() {
    vec3 world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    eawr_fog_source = vec2(world.x, -world.z);
}
void fragment() {
    float behind = texture(eawr_fog_depth, SCREEN_UV).r;
    bool drawn = eawr_fog_reverse_z ? behind > 0.0 : behind < 1.0;
    bool farther = eawr_fog_reverse_z ? behind < FRAGCOORD.z : behind > FRAGCOORD.z;
    if (drawn && farther) discard;
    vec2 uv = vec2(eawr_fog_source.x - eawr_fog_top_left.x, eawr_fog_top_left.y - eawr_fog_source.y) / eawr_fog_size;
    vec4 fog = texture(eawr_fog_cells, uv);
    float tile = texture(eawr_fog_grid, vec2(eawr_fog_source.x, -eawr_fog_source.y) / eawr_fog_size * eawr_fog_grid_repeats).a;
    ALBEDO = fog.rgb;
    ALPHA = fog.a * tile;
}
)";

// After the sky's transparent layers (the transparent pass, priority 0), whose spheres sort
// nearest the camera, so the fog lies over the backdrop.
constexpr int fog_render_priority = 1;

[[nodiscard]] std::string json(const std::string_view text) {
    std::ostringstream output;
    output << '"';
    for (const char character : text) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(character)) << std::dec;
            } else {
                output << character;
            }
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] double to_double(const sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}

[[nodiscard]] Ref<Image> cells_image(const space::FogField& field) {
    const auto texels = field.texels();
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(texels.size()));
    std::memcpy(bytes.ptrw(), texels.data(), texels.size());
    return Image::create_from_data(static_cast<int32_t>(field.layout().wide), static_cast<int32_t>(field.layout().tall),
                                   false, Image::FORMAT_RGBA8, bytes);
}

} // namespace

LiveFogView::LiveFogView(Node3D& host) : host_(&host) {
    if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
}

LiveFogView::~LiveFogView() { release(); }

void LiveFogView::prepare(const vfs::Vfs& filesystem, const bool revealed, const bool deploy_overlay) {
    prepared_ = true;
    revealed_ = revealed;
    deploy_overlay_ = deploy_overlay && !revealed;
    if (auto constants = data::load_document(filesystem, "data/xml/gameconstants.xml")) {
        looks_ = space::fog_looks(constants.value().root);
    } else {
        notes_.push_back("data/xml/gameconstants.xml: " + core::format_diagnostic(constants.error())
                         + "; the FoC values are used");
    }
    for (const core::Diagnostic& diagnostic : looks_.diagnostics) notes_.push_back(core::format_diagnostic(diagnostic));
    if (revealed_) {
        status_ = "revealed";
        return;
    }
    // FW-03: the grid tile FoC loads as W_Space_FOW_Grid.tga (shipped as a .dds). FW-22: and the
    // reinforcement overlay's, W_Space_Reinforce_FOW_Grid.tga.
    const auto load_grid = [&](const std::array<const char*, 2>& candidates, Ref<ImageTexture>& target,
                               std::string& source) {
        for (const std::string candidate : candidates) {
            if (!filesystem.stat(candidate)) continue;
            auto loaded = assets::load_texture(filesystem, candidate);
            if (!loaded) {
                notes_.push_back(candidate + ": " + core::format_diagnostic(loaded.error()));
                continue;
            }
            std::string failure;
            Ref<Image> image = texture_image(loaded.value(), failure);
            if (image.is_null()) {
                notes_.push_back(candidate + ": " + failure);
                continue;
            }
            if (image->is_compressed()) image->decompress();
            image->generate_mipmaps();
            target = ImageTexture::create_from_image(image);
            source = candidate;
            break;
        }
    };
    load_grid({"data/art/textures/w_space_fow_grid.tga", "data/art/textures/w_space_fow_grid.dds"}, grid_, grid_source_);
    if (deploy_overlay_) {
        load_grid({"data/art/textures/w_space_reinforce_fow_grid.tga", "data/art/textures/w_space_reinforce_fow_grid.dds"},
                  reinforce_grid_, reinforce_grid_source_);
        if (reinforce_grid_.is_null()) notes_.push_back("no reinforcement grid texture; the overlay is not drawn");
    }
    status_ = grid_.is_valid() && (!deploy_overlay_ || reinforce_grid_.is_valid()) ? "awaiting bounds" : "no grid texture";
}

bool LiveFogView::create_plane(const space::FogFieldLayout& layout) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr || !scenario_.is_valid()) {
        status_ = "no rendering scenario";
        return false;
    }
    shader_.instantiate();
    const std::string code = stored_output::backend_source(fog_plane_shader);
    shader_->set_code(String::utf8(code.data(), static_cast<int64_t>(code.size())));
    material_.instantiate();
    material_->set_shader(shader_);
    material_->set_render_priority(fog_render_priority);
    // RenderingDevice backends clear depth to 0 (reverse Z); Compatibility clears to 1.
    material_->set_shader_parameter(StringName("eawr_fog_reverse_z"), stored_output::active());
    material_->set_shader_parameter(StringName("eawr_fog_cells"), cells_);
    material_->set_shader_parameter(StringName("eawr_fog_grid"), deploy_overlay_ ? reinforce_grid_ : grid_);
    material_->set_shader_parameter(StringName("eawr_fog_top_left"),
                                    Vector2(static_cast<float>(layout.left), static_cast<float>(layout.top)));
    material_->set_shader_parameter(StringName("eawr_fog_size"),
                                    Vector2(static_cast<float>(layout.width()), static_cast<float>(layout.height())));
    // FW-13: the plane covers the fog rectangle and nothing beyond it.
    mesh_.instantiate();
    mesh_->set_size(Vector2(static_cast<float>(layout.width()), static_cast<float>(layout.height())));
    mesh_->set_material(material_);
    instance_ = rendering->instance_create2(mesh_->get_rid(), scenario_);
    rendering->instance_geometry_set_cast_shadows_setting(instance_, RenderingServer::SHADOW_CASTING_SETTING_OFF);
    // Source (x, y, z) is render (x, z, -y).
    const double centre_x = layout.left + layout.width() / 2.0;
    const double centre_y = layout.top - layout.height() / 2.0;
    Transform3D transform;
    transform.origin = Vector3(static_cast<float>(centre_x), static_cast<float>(looks_.height), static_cast<float>(-centre_y));
    rendering->instance_set_transform(instance_, transform);
    return true;
}

void LiveFogView::frame(const LiveSessionView& live, const std::optional<camera::SourceTargetBounds>& bounds) {
    if (!prepared_ || revealed_ || grid_.is_null() || status_ == "failed") return;
    const auto& battle = live.battle_frame();
    if (!battle.latest) return;
    const std::shared_ptr<const platform::LiveFog>& fog = battle.fog;
    if (!field_) {
        std::optional<space::FogFieldLayout> layout;
        if (fog) {
            // FW-07: the session's fog grid, one texel per cell (the map's fog extents, V-18).
            const auto& rules = fog->rules;
            layout = space::FogFieldLayout{to_double(rules.map_left), to_double(rules.map_top), to_double(rules.cell_size),
                                           rules.cells_wide, rules.cells_tall};
            source_ = "cells";
        } else {
            if (!bounds) return;
            // FW-07: without fog cells, DesiredSpaceFOWCellSize cells over the playable rectangle.
            layout = space::fog_field_layout(bounds->min_x, bounds->max_x, bounds->min_y, bounds->max_y, looks_.cell_size);
            source_ = "sensors";
        }
        if (!layout) {
            status_ = "failed";
            notes_.push_back("the camera bounds give no fog grid");
            return;
        }
        field_.emplace(*layout, looks_);
        // FW-22 (#563): nothing in M2 blocks a deployment point, so only the fog and the border draw red.
        if (deploy_overlay_) field_->set_deployment_overlay(true);
        cells_ = ImageTexture::create_from_image(cells_image(*field_));
        if (!create_plane(*layout)) {
            field_.reset();
            status_ = "failed";
            return;
        }
        status_ = "drawn";
    }
    // FW-08: the local team's sensors, as the minimap's fog reads them (MM-10).
    std::vector<space::FogFieldRevealer> revealers;
    const auto local_team = live.team_of(live.local_player());
    for (const sim::tactical::TacticalInstance& instance : battle.latest->instances()) {
        if (!instance.reveal_range || !local_team || instance.team != *local_team) continue;
        revealers.push_back({to_double(instance.fixed_transform.rows[0][3]), to_double(instance.fixed_transform.rows[1][3]),
                             to_double(*instance.reveal_range)});
    }
    // FW-05: fades run on the battle's logical frames, so a paused battle holds them.
    const double tick = battle.presented_tick;
    const double frames = last_tick_ ? std::max(0.0, tick - *last_tick_) : 0.0;
    last_tick_ = tick;
    if (source_ == "sensors") {
        field_->advance(revealers, frames);
    } else if (fog) {
        // FW-10: the frames since the local player's grid was last serviced (V-12's phase).
        const std::uint64_t period = std::max<std::uint32_t>(fog->rules.service_period, 1U);
        const std::uint64_t since = (fog->tick % period + period - fog->player % period) % period;
        field_->advance(fog->values, static_cast<double>(since), frames);
        cell_tick_ = fog->tick;
    }
    if (field_->changed()) {
        cells_->update(cells_image(*field_));
        ++uploads_;
    }
    ++frames_;
    advanced_frames_ += frames;
    revealers_ = revealers.size();
    held_cells_ = field_->held_cells();
    fogged_cells_ = field_->fogged_cells();
    max_held_cells_ = std::max(max_held_cells_, held_cells_);
}

void LiveFogView::release() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering != nullptr && instance_.is_valid()) rendering->free_rid(instance_);
    instance_ = RID();
    mesh_.unref();
    material_.unref();
    shader_.unref();
    cells_.unref();
    grid_.unref();
    reinforce_grid_.unref();
}

void LiveFogView::write_report(std::ostream& output) const {
    output << "  \"live_fog\": {\"status\": " << json(status_) << ", \"revealed\": " << (revealed_ ? "true" : "false")
           << ", \"colour\": [" << static_cast<int>(looks_.colour[0]) << ", " << static_cast<int>(looks_.colour[1]) << ", "
           << static_cast<int>(looks_.colour[2]) << ", " << static_cast<int>(looks_.colour[3]) << "]"
           << ", \"height\": " << looks_.height << ", \"cell_size\": " << looks_.cell_size
           << ", \"regrow_seconds\": " << looks_.regrow_seconds
           << ", \"fade_step_per_frame\": " << space::fog_fade_step_per_frame(looks_.regrow_seconds)
           << ", \"grid_texture\": " << json(deploy_overlay_ ? reinforce_grid_source_ : grid_source_)
           << ", \"deploy_overlay\": " << (deploy_overlay_ ? "true" : "false")
           << ", \"overlay_colour\": [" << static_cast<int>(looks_.reinforce_colour[0]) << ", "
           << static_cast<int>(looks_.reinforce_colour[1]) << ", " << static_cast<int>(looks_.reinforce_colour[2]) << ", "
           << static_cast<int>(looks_.reinforce_colour[3]) << "]"
           << ", \"source\": " << json(source_)
           << ", \"cell_tick\": " << cell_tick_ << ", \"grid\": ";
    if (field_) {
        const auto& layout = field_->layout();
        output << "{\"left\": " << layout.left << ", \"top\": " << layout.top << ", \"cell\": " << layout.cell
               << ", \"wide\": " << layout.wide << ", \"tall\": " << layout.tall << "}";
    } else {
        output << "null";
    }
    output << ", \"frames\": " << frames_ << ", \"advanced_logical_frames\": " << advanced_frames_
           << ", \"uploads\": " << uploads_ << ", \"revealers\": " << revealers_ << ", \"held_cells\": " << held_cells_
           << ", \"max_held_cells\": " << max_held_cells_ << ", \"fogged_cells\": " << fogged_cells_ << ", \"notes\": [";
    for (std::size_t index = 0; index < notes_.size(); ++index) output << (index ? ", " : "") << json(notes_[index]);
    output << "]},\n";
}

} // namespace eawr::presentation::godot_backend

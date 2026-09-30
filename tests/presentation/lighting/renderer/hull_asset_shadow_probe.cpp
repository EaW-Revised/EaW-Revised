// Real MC-50 Hull shadow exercise through the production renderer (P1-04, #25).
// The pinned Remake MC-50 Hull (model, MeshBumpColorize.fx material and
// BaseTexture, rest pose) is the receiver. A second instance of the same
// pinned Hull on the source-backed sun line is the caster, because the Hull
// alone barely self-occludes. Masks come from the CPU ray tracer before any
// capture. Private asset bytes and captures stay in the caller's output
// directory; this is not an original-game comparison.
//
// Required user args: --eawr-game-root, --eawr-mod-root,
// --eawr-lighting-report, --eawr-lighting-output. Every other flag is a
// recorded diagnostic that changes the scene or light from the declared
// fixture: --eawr-receiver-casts false, --eawr-shadow-bias-policy
// production_space (the default)|adapter_default|
// directional_light_node_defaults|explicit (with
// --eawr-shadow-bias and --eawr-shadow-normal-bias), --eawr-shadow-max-
// distance-scale, --eawr-caster-sun-distance, --eawr-caster-lateral-offset,
// --eawr-hull-upload rigid_skinned|unskinned (the matched skinning control:
// the same vertices, material, pose and scene without the Hull's bone binding).
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/snapshot.hpp"
#include "hull_asset_fixture.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <span>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace godot;

namespace eawr::lighting_probe {
namespace {

namespace sim = eawr::sim;
namespace presentation = eawr::presentation;
using eawr::presentation::godot_backend::GodotRenderer;

constexpr sim::AssetId receiver_asset = 1;
constexpr sim::AssetId caster_asset = 2;
constexpr sim::AssetId caster_no_cast_asset = 3;
constexpr sim::EntityId receiver_entity = 1;
constexpr sim::EntityId caster_entity = 2;
constexpr sim::EntityId caster_no_cast_entity = 3;
// Never submitted: a skin pose bound to it only asks the renderer whether the
// receiver asset was uploaded on its skinning path.
constexpr sim::EntityId route_probe_entity = 99;

[[nodiscard]] sim::math::Mat3x4 placement(const std::array<float, 3>& translation) {
    sim::math::Mat3x4 result;
    for (std::size_t i = 0; i < 3; ++i) {
        result.rows[i][i] = sim::math::Fixed::from_raw(sim::math::Fixed::scale);
        result.rows[i][3] = sim::math::Fixed::from_raw(static_cast<std::int64_t>(
            std::llround(static_cast<double>(translation[i]) * static_cast<double>(sim::math::Fixed::scale))));
    }
    return result;
}

[[nodiscard]] std::string quote(const std::string& value) {
    std::string result = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') result.push_back('\\');
        if (static_cast<unsigned char>(c) >= 32) result.push_back(c);
    }
    return result + '"';
}

[[nodiscard]] std::string triple(const std::array<float, 3>& v) {
    std::ostringstream out;
    out.precision(9);
    out << '[' << v[0] << ',' << v[1] << ',' << v[2] << ']';
    return out.str();
}

struct Means { std::size_t pixels{}; std::array<double, 3> rgb{}; };

[[nodiscard]] Means region(const PackedByteArray& data, const MaskPlan& masks, PixelClass wanted) {
    Means result;
    for (std::size_t index = 0; index < masks.classes.size(); ++index) {
        if (masks.classes[index] != wanted) continue;
        const auto offset = static_cast<std::int64_t>(index * 4);
        if (offset + 3 >= data.size()) continue;
        ++result.pixels;
        for (int c = 0; c < 3; ++c) result.rgb[static_cast<std::size_t>(c)] += data[offset + c];
    }
    if (result.pixels > 0) for (double& c : result.rgb) c /= static_cast<double>(result.pixels);
    return result;
}

[[nodiscard]] double luminance(const Means& value) {
    return 0.2126 * value.rgb[0] + 0.7152 * value.rgb[1] + 0.0722 * value.rgb[2];
}

[[nodiscard]] std::string means_json(const Means& value) {
    std::ostringstream out;
    out.precision(9);
    out << "{\"pixels\":" << value.pixels << ",\"rgb\":["
        << value.rgb[0] << ',' << value.rgb[1] << ',' << value.rgb[2] << "]}";
    return out.str();
}

[[nodiscard]] std::filesystem::path utf8_path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] std::string hex_sha256(const std::vector<std::uint8_t>& bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

} // namespace

class EawrHullAssetShadowProbe final : public Node {
    GDCLASS(EawrHullAssetShadowProbe, Node)
protected:
    static void _bind_methods() {}
public:
    void _ready() override;
    void _process(double) override;
private:
    struct Capture { std::string name; Means receiver; Means control; std::string file; };
    struct Step { std::string name; std::function<void()> action; };
    void check(bool condition, const std::string& reason);
    void make_renderer(bool receiver_before_lighting);
    void submit(bool with_caster, bool noncasting = false);
    void capture(const std::string& name);
    void finish();
    SubViewport* viewport_{};
    Node3D* host_{};
    std::unique_ptr<GodotRenderer> renderer_;
    std::optional<Fixture> fixture_;
    GodotRenderer::LightingState lighting_{};
    std::vector<Step> steps_;
    std::vector<Capture> captures_;
    std::vector<std::string> failures_;
    std::string output_dir_;
    std::string report_path_;
    std::string mask_sha256_;
    std::string bias_policy_{"production_space"};
    bool receiver_casts_{true};
    UploadRoute upload_route_{UploadRoute::rigid_skinned};
    std::string upload_route_arg_{"rigid_skinned"};
    std::string surface_sha256_;
    std::int32_t upload_mesh_bone_{-1};
    std::size_t upload_bone_count_{};
    std::string skin_pose_probe_;
    float max_distance_scale_{1.0F};
    FixtureOptions options_;
    std::size_t step_{};
    int wait_frames_{};
    int frames_{};
    std::size_t normal_receivers_{};
    std::size_t no_receiver_count_{};
    std::size_t variant_failures_{};
    std::size_t normal_instances_{};
    std::size_t skinned_bindings_{};
    bool finished_{};
};

void EawrHullAssetShadowProbe::check(bool condition, const std::string& reason) {
    if (!condition) failures_.push_back(reason);
}

void EawrHullAssetShadowProbe::make_renderer(bool receiver_before_lighting) {
    renderer_.reset();
    renderer_ = std::make_unique<GodotRenderer>(*host_);
    renderer_->set_camera(fixture_->camera);
    const auto upload = [this](sim::AssetId asset, const char* what) {
        const auto result = renderer_->upload(asset, fixture_->model, fixture_->texture, fixture_->material);
        check(result.has_value(), std::string(what) + " upload");
    };
    if (receiver_before_lighting) upload(receiver_asset, "receiver-disabled Hull");
    renderer_->set_lighting(lighting_);
    check(lighting_.shadows, "shadows were not enabled before normal upload");
    if (!receiver_before_lighting) upload(receiver_asset, "receiver Hull");
    upload(caster_asset, "caster Hull");
    upload(caster_no_cast_asset, "noncasting caster Hull");
    renderer_->set_casts_shadows(caster_no_cast_asset, false);
    // Route evidence from the production adapter: it accepts a skin pose only
    // for an asset it uploaded with bones and a skeleton binding.
    {
        std::vector<presentation::animation::BonePose> identity(fixture_->model.bones.size());
        for (presentation::animation::BonePose& bone : identity) {
            bone.skin_asset = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        }
        const bool accepted = renderer_->set_skin_pose(route_probe_entity, receiver_asset, identity).has_value();
        const std::string evidence = accepted ? "accepted" : "rejected";
        check(skin_pose_probe_.empty() || skin_pose_probe_ == evidence, "upload route changed between renderers");
        skin_pose_probe_ = evidence;
        check(accepted == (upload_route_ == UploadRoute::rigid_skinned), "renderer upload route contradicts request");
    }
    if (!receiver_casts_) renderer_->set_casts_shadows(receiver_asset, false);
    variant_failures_ += renderer_->shadow_variant_failures();
    check(renderer_->shadow_variant_failures() == 0, "shadow variant failure");
    const std::size_t count = renderer_->shadow_receiving_materials();
    check(count == (receiver_before_lighting ? 2U : 3U), "shadow receiving material count");
    if (receiver_before_lighting) no_receiver_count_ = count;
    else normal_receivers_ = count;
}

void EawrHullAssetShadowProbe::submit(bool with_caster, bool noncasting) {
    std::vector<sim::RenderInstance> instances{{receiver_entity, receiver_asset, placement({0, 0, 0})}};
    if (with_caster) {
        instances.push_back({noncasting ? caster_no_cast_entity : caster_entity,
            noncasting ? caster_no_cast_asset : caster_asset, placement(fixture_->caster_translation)});
    }
    renderer_->submit(std::make_shared<const sim::RenderSnapshot>(++frames_, std::move(instances)));
    const std::size_t expected = with_caster ? 2U : 1U;
    check(renderer_->instance_count() == expected, "instance count after submit");
    const auto submitted = renderer_->submission_evidence();
    check(submitted.size() == expected, "submission evidence count");
    for (const auto& item : submitted) check(item.pass == presentation::RenderPass::opaque, "nonopaque submitted pass");
    if (with_caster && !noncasting) normal_instances_ = submitted.size();
}

void EawrHullAssetShadowProbe::capture(const std::string& name) {
    Ref<Image> image = viewport_->get_texture()->get_image();
    check(image.is_valid() && image->get_width() == static_cast<int32_t>(capture_width)
        && image->get_height() == static_cast<int32_t>(capture_height), name + " viewport dimensions");
    if (!image.is_valid()) return;
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray pixels = image->get_data();
    const std::string file = name + ".png";
    check(image->save_png(String((output_dir_ + "/" + file).c_str())) == OK, name + " PNG save");
    const Means receiver = region(pixels, fixture_->masks, PixelClass::shadowed);
    const Means control = region(pixels, fixture_->masks, PixelClass::lit);
    check(receiver.pixels >= min_mask_pixels && control.pixels >= min_mask_pixels, name + " region coverage");
    captures_.push_back({name, receiver, control, file});
}

void EawrHullAssetShadowProbe::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) return;
    std::string game_root;
    std::string mod_root;
    std::string explicit_bias;
    std::string explicit_normal_bias;
    const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
    for (std::int64_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == String("--eawr-lighting-report")) report_path_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-lighting-output")) output_dir_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-game-root")) game_root = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-mod-root")) mod_root = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-shadow-bias-policy")) bias_policy_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-shadow-max-distance-scale")) {
            max_distance_scale_ = static_cast<float>(args[i + 1].to_float());
        }
        if (args[i] == String("--eawr-caster-lateral-offset")) {
            options_.caster_lateral_offset = static_cast<float>(args[i + 1].to_float());
        }
        if (args[i] == String("--eawr-caster-sun-distance")) {
            options_.caster_sun_distance = static_cast<float>(args[i + 1].to_float());
        }
        if (args[i] == String("--eawr-receiver-casts")) receiver_casts_ = args[i + 1] != String("false");
        if (args[i] == String("--eawr-hull-upload")) upload_route_arg_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-shadow-bias")) explicit_bias = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-shadow-normal-bias")) explicit_normal_bias = args[i + 1].utf8().get_data();
    }
    // production_space: bias 2 and normal bias 5, as the viewer's space
    // lighting sets them since #150; the RenderingServer light defaults acne
    // the self-shadowing hull on Forward+. adapter_default: the adapter sets
    // no bias (RenderingServer light defaults). directional_light_node_defaults:
    // the bias and normal bias Godot's own DirectionalLight3D node applies,
    // read from a fresh node.
    if (bias_policy_ == "production_space") {
        lighting_.shadow_bias = 2.0F;
        lighting_.shadow_normal_bias = 5.0F;
    } else if (bias_policy_ == "directional_light_node_defaults") {
        DirectionalLight3D* node = memnew(DirectionalLight3D);
        lighting_.shadow_bias = static_cast<float>(node->get_param(Light3D::PARAM_SHADOW_BIAS));
        lighting_.shadow_normal_bias = static_cast<float>(node->get_param(Light3D::PARAM_SHADOW_NORMAL_BIAS));
        memdelete(node);
    } else if (bias_policy_ == "explicit") {
        // Diagnostic sweep only: both values must be given.
        check(!explicit_bias.empty() && !explicit_normal_bias.empty(), "explicit bias policy needs both values");
        lighting_.shadow_bias = static_cast<float>(String(explicit_bias.c_str()).to_float());
        lighting_.shadow_normal_bias = static_cast<float>(String(explicit_normal_bias.c_str()).to_float());
    } else if (bias_policy_ != "adapter_default") {
        check(false, "unknown shadow bias policy: " + bias_policy_);
        finish();
        return;
    }
    if (upload_route_arg_ == "unskinned") upload_route_ = UploadRoute::unskinned;
    else if (upload_route_arg_ != "rigid_skinned") {
        check(false, "unknown hull upload route: " + upload_route_arg_);
        finish();
        return;
    }
    Prepared prepared = prepare_hull_fixture(utf8_path(game_root), utf8_path(mod_root), options_);
    if (!prepared.fixture) {
        check(false, "fixture: " + prepared.failure);
        finish();
        return;
    }
    fixture_ = std::move(prepared.fixture);
    // The masks, camera and caster placement come from the same triangles on
    // both routes. Bake the rigid rest bind into the plain route so #52's
    // renderer uploads identical bind-space vertices with either bone binding.
    auto unskinned = without_skinning(fixture_->model);
    if (!unskinned) {
        check(false, "unskinned Hull rest bind failed");
        finish();
        return;
    }
    surface_sha256_ = surface_sha256(*unskinned);
    if (upload_route_ == UploadRoute::unskinned) fixture_->model = std::move(*unskinned);
    upload_bone_count_ = fixture_->model.bones.size();
    for (const assets::Mesh& mesh : fixture_->model.meshes) upload_mesh_bone_ = mesh.bone;
    const auto pgm = encode_pgm(fixture_->masks);
    mask_sha256_ = hex_sha256(pgm);
    {
        std::ofstream file(utf8_path(output_dir_) / "masks.pgm", std::ios::binary);
        file.write(reinterpret_cast<const char*>(pgm.data()), static_cast<std::streamsize>(pgm.size()));
        check(static_cast<bool>(file), "mask PGM write");
    }
    viewport_ = memnew(SubViewport);
    viewport_->set_size(Vector2i(static_cast<int32_t>(capture_width), static_cast<int32_t>(capture_height)));
    viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
    viewport_->set_use_own_world_3d(true);
    add_child(viewport_);
    host_ = memnew(Node3D);
    viewport_->add_child(host_);

    lighting_.sph = fixture_->lighting.sph;
    lighting_.toward_light = fixture_->lighting.toward_light;
    lighting_.specular = fixture_->lighting.specular;
    lighting_.shadows = true;
    lighting_.shadow_floor = fixture_->lighting.shadow_floor;
    // Diagnostic override only; the fixture's derived coverage is the default.
    lighting_.shadow_max_distance = fixture_->shadow_max_distance * max_distance_scale_;
    lighting_.shadow_atlas_size = shadow_atlas_size;

    steps_.push_back({"setup", [this] { make_renderer(false); submit(true); }});
    steps_.push_back({"on", [this] { capture("on"); }});
    steps_.push_back({"off-toggle", [this] { renderer_->set_shadows_enabled(false); }});
    steps_.push_back({"off", [this] { capture("off"); }});
    steps_.push_back({"restore-toggle", [this] { renderer_->set_shadows_enabled(true); }});
    steps_.push_back({"restored_on", [this] { capture("restored_on"); }});
    steps_.push_back({"missing-caster-submit", [this] { submit(false); }});
    steps_.push_back({"missing_caster", [this] { capture("missing_caster"); }});
    steps_.push_back({"casting-disabled-submit", [this] { submit(true, true); }});
    steps_.push_back({"casting_disabled", [this] { capture("casting_disabled"); }});
    steps_.push_back({"receiving-disabled-setup", [this] { make_renderer(true); submit(true); }});
    steps_.push_back({"receiving_disabled", [this] { capture("receiving_disabled"); }});
    set_process(true);
}

void EawrHullAssetShadowProbe::_process(double) {
    if (finished_ || Engine::get_singleton()->is_editor_hint()) return;
    if (++frames_ > 240) { check(false, "frame budget exhausted"); finish(); return; }
    if (wait_frames_ > 0 && --wait_frames_ > 0) return;
    if (step_ < steps_.size()) {
        steps_[step_++].action();
        wait_frames_ = 5;
        return;
    }
    finish();
}

void EawrHullAssetShadowProbe::finish() {
    finished_ = true;
    const auto backend = renderer_ ? renderer_->backend_info() : presentation::BackendInfo{};
    if (renderer_) skinned_bindings_ = renderer_->skin_bindings().size();
    if (captures_.size() == 6) {
        const auto& on = captures_[0];
        const auto& off = captures_[1];
        check(luminance(off.receiver) - luminance(on.receiver) >= 6.0, "Hull receiver did not darken by six decoded levels");
        check(std::abs(luminance(off.control) - luminance(on.control)) <= 2.0, "lit Hull control changed");
        for (std::size_t index = 2; index < captures_.size(); ++index) {
            const auto& sample = captures_[index];
            const double target = index == 2 ? luminance(on.receiver) : luminance(off.receiver);
            check(std::abs(luminance(sample.receiver) - target) <= 2.0, sample.name + " receiver contradicts on/off control");
            check(std::abs(luminance(sample.control) - luminance(off.control)) <= 2.0, sample.name + " lit control changed");
        }
    } else check(false, "six captures missing");
    renderer_.reset();
    std::ostringstream out;
    out.precision(9);
    out << "{\"schema\":\"eawr-hull-asset-shadow-probe-v1\",\"status\":"
        << quote(failures_.empty() ? "passed" : "failed") << ",\"source\":\"alo_viewer_default\",\"policy\":\"sh\"";
    if (fixture_) {
        const Fixture& f = *fixture_;
        out << ",\"asset\":{\"model\":" << quote(f.model_path) << ",\"model_layer\":" << quote(f.model_layer)
            << ",\"model_sha256\":" << quote(f.model_sha256) << ",\"texture\":" << quote(f.texture_path)
            << ",\"texture_layer\":" << quote(f.texture_layer) << ",\"texture_sha256\":" << quote(f.texture_sha256)
            << ",\"mesh\":" << quote(f.mesh_name) << ",\"vertices\":" << f.vertex_count
            << ",\"indices\":" << f.index_count << ",\"triangles\":" << f.triangle_count
            << ",\"degenerate_dropped\":" << f.skipped_degenerate << ",\"bone_chain\":[";
        for (std::size_t i = 0; i < f.bone_chain.size(); ++i) out << (i ? "," : "") << quote(f.bone_chain[i]);
        out << "],\"rest_placement_delta\":" << f.rest_placement_delta << ",\"rest_tolerance\":" << f.rest_tolerance
            << "},\"upload\":{\"route\":" << quote(upload_route_name(upload_route_))
            << ",\"surface_sha256\":" << quote(surface_sha256_) << ",\"mesh_bone\":" << upload_mesh_bone_
            << ",\"model_bones\":" << upload_bone_count_ << ",\"skin_pose_probe\":" << quote(skin_pose_probe_)
            << "},\"material\":{\"program\":" << quote(f.program) << ",\"technique\":" << quote(f.technique)
            << ",\"pass_name\":" << quote(f.pass_name) << ",\"render_pass\":\"opaque\",\"bindings\":"
            << f.material.bindings.size() << "}"
            << ",\"scene\":{\"receiver\":{\"entity\":1,\"asset\":1,\"translation\":[0,0,0],\"casts_shadows\":"
            << (receiver_casts_ ? "true" : "false") << "},"
            << "\"caster\":{\"entity\":2,\"asset\":2,\"translation\":" << triple(f.caster_translation)
            << ",\"sun_distance\":" << f.options.caster_sun_distance
            << ",\"lateral_offset\":" << f.options.caster_lateral_offset
            << "},\"noncasting_caster\":{\"entity\":3,\"asset\":3}}"
            << ",\"lighting\":{\"toward_light\":" << triple(lighting_.toward_light)
            << ",\"sh_constant\":[" << lighting_.sph[0][15] << ',' << lighting_.sph[1][15] << ','
            << lighting_.sph[2][15] << "]}"
            << ",\"camera\":{\"width\":" << f.camera.width << ",\"height\":" << f.camera.height
            << ",\"eye\":" << triple(f.camera.eye) << ",\"target\":" << triple(f.camera.target)
            << ",\"up\":" << triple(f.camera.up) << ",\"fov\":" << f.camera.vertical_fov_degrees
            << ",\"near\":" << f.camera.near_plane << ",\"far\":" << f.camera.far_plane << "}"
            << ",\"view_selection\":{\"rule\":\"max min(lit,shadowed) over predeclared views with both >= "
            << min_mask_pixels << "\",\"selected\":" << f.selected << ",\"candidates\":[";
        for (std::size_t i = 0; i < f.candidates.size(); ++i) {
            const auto& c = f.candidates[i];
            out << (i ? "," : "") << "{\"view\":" << triple(c.view_direction) << ",\"eligible\":" << c.eligible
                << ",\"lit\":" << c.lit << ",\"shadowed\":" << c.shadowed << ",\"qualifies\":"
                << (c.qualifies ? "true" : "false") << '}';
        }
        out << "]},\"masks\":{\"file\":\"masks.pgm\",\"sha256\":" << quote(mask_sha256_)
            << ",\"lit\":" << f.masks.lit << ",\"shadowed\":" << f.masks.shadowed
            << ",\"policy\":{\"min_light_facing\":" << f.policy.min_light_facing
            << ",\"min_view_facing\":" << f.policy.min_view_facing
            << ",\"light_footprint\":" << f.policy.light_footprint
            << ",\"min_occluder_distance\":" << f.policy.min_occluder_distance
            << ",\"erosion_radius\":" << f.policy.erosion_radius
            << ",\"depth_tolerance\":" << f.policy.depth_tolerance
            << ",\"max_depth\":" << f.policy.max_depth << "}}"
            << ",\"shadows\":{\"projection\":\"orthogonal\",\"atlas_size\":" << lighting_.shadow_atlas_size
            << ",\"max_distance\":" << lighting_.shadow_max_distance << ",\"far_distance\":" << f.far_distance
            << ",\"assumed_fade_start\":" << assumed_fade_start << ",\"floor\":[" << lighting_.shadow_floor[0]
            << ',' << lighting_.shadow_floor[1] << ',' << lighting_.shadow_floor[2] << ']'
            << ",\"bias_policy\":" << quote(bias_policy_) << ",\"bias\":";
        if (lighting_.shadow_bias) out << *lighting_.shadow_bias; else out << "null";
        out << ",\"normal_bias\":";
        if (lighting_.shadow_normal_bias) out << *lighting_.shadow_normal_bias; else out << "null";
        out << ",\"filter\":\"engine default; adapter does not set\"}";
    }
    out << ",\"receiving_materials\":{\"normal\":" << normal_receivers_ << ",\"receiver_disabled\":"
        << no_receiver_count_ << ",\"variant_failures\":" << variant_failures_ << "}"
        << ",\"submitted_instances\":" << normal_instances_ << ",\"skin_bindings_with_pose\":" << skinned_bindings_
        << ",\"backend\":{\"version\":" << quote(String(Engine::get_singleton()->get_version_info()["string"]).utf8().get_data())
        << ",\"driver\":" << quote(RenderingServer::get_singleton()->get_current_rendering_driver_name().utf8().get_data())
        << ",\"method\":" << quote(RenderingServer::get_singleton()->get_current_rendering_method().utf8().get_data())
        << ",\"vendor\":" << quote(backend.adapter_vendor) << ",\"adapter\":" << quote(backend.adapter_name)
        << ",\"api\":" << quote(backend.driver_api) << "},\"captures\":[";
    for (std::size_t i = 0; i < captures_.size(); ++i) {
        const Capture& c = captures_[i];
        out << (i ? "," : "") << "{\"name\":" << quote(c.name) << ",\"png\":" << quote(c.file)
            << ",\"receiver\":" << means_json(c.receiver) << ",\"control\":" << means_json(c.control) << "}";
    }
    out << "],\"failures\":[";
    for (std::size_t i = 0; i < failures_.size(); ++i) out << (i ? "," : "") << quote(failures_[i]);
    out << "]}\n";
    bool written = false;
    if (!report_path_.empty()) {
        std::ofstream file(utf8_path(report_path_), std::ios::binary);
        file << out.str();
        written = static_cast<bool>(file);
    }
    UtilityFunctions::print(String("EAWR hull asset shadow probe ") + (failures_.empty() ? "passed" : "failed"));
    get_tree()->quit(failures_.empty() && written ? 0 : 1);
}

namespace {
void initialize(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(EawrHullAssetShadowProbe);
}
void uninitialize(const ModuleInitializationLevel) {}
} // namespace

} // namespace eawr::lighting_probe

extern "C" GDExtensionBool GDE_EXPORT eawr_hull_asset_shadow_probe_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(eawr::lighting_probe::initialize);
    init.register_terminator(eawr::lighting_probe::uninitialize);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}

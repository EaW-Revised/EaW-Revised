// Original synthetic ship-hull shadow exercise through the production renderer.
// All geometry and textures below are invented. This is not a retail comparison.
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/sim/snapshot.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
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
namespace {
namespace sim = eawr::sim;
namespace lighting = eawr::presentation::lighting;
namespace presentation = eawr::presentation;
using eawr::presentation::godot_backend::GodotRenderer;

constexpr int width = 480;
constexpr int height = 480;
constexpr float eye_height = 18.0F;
constexpr float fov = 45.0F;
constexpr float half_span = eye_height * 0.41421356237F; // tan(fov / 2)
constexpr sim::AssetId hull_asset = 1;
constexpr sim::AssetId bridge_asset = 2;
constexpr sim::AssetId bridge_no_cast_asset = 3;
constexpr sim::AssetId sky_asset = 4;
constexpr sim::EntityId hull_entity = 1;
constexpr sim::EntityId bridge_entity = 2;
constexpr sim::EntityId bridge_no_cast_entity = 3;
constexpr sim::EntityId sky_entity = 4;
constexpr std::int64_t fixed_one = std::int64_t{1} << 24;

[[nodiscard]] sim::math::Mat3x4 identity() {
    sim::math::Mat3x4 result;
    for (int i = 0; i < 3; ++i) result.rows[i][i] = sim::math::Fixed::from_raw(fixed_one);
    return result;
}

using Point = std::array<float, 3>;

// Source X,Y form the plan view; source Z is height. The adapter maps these to
// render X,-Z,Y. Both windings keep the invented broad deck visible under the
// legacy adapter's back-face culling after the basis change.
void triangle(eawr::assets::Submesh& mesh, const Point& a, const Point& b, const Point& c, bool skinned = false) {
    for (const Point& p : {a, b, c}) {
        eawr::assets::Vertex v;
        v.position = {p[0], p[1], p[2]};
        v.normal = {0.0F, 0.0F, 1.0F};
        v.tangent = {1.0F, 0.0F, 0.0F};
        v.binormal = {0.0F, 1.0F, 0.0F};
        v.texcoord[0] = {(p[0] + 4.0F) / 8.0F, (p[1] + 8.0F) / 16.0F};
        v.color = {1.0F, 1.0F, 1.0F, 1.0F};
        if (skinned) {
            v.bone_indices = {0, 0, 0, 0};
            v.bone_weights = {1.0F, 0.0F, 0.0F, 0.0F};
        }
        mesh.vertices.push_back(v);
    }
    const std::uint16_t base = static_cast<std::uint16_t>(mesh.vertices.size() - 3);
    for (const std::uint16_t index : {base, static_cast<std::uint16_t>(base + 1),
             static_cast<std::uint16_t>(base + 2), base, static_cast<std::uint16_t>(base + 2),
             static_cast<std::uint16_t>(base + 1)}) mesh.indices.push_back(index);
}

void quad(eawr::assets::Submesh& mesh, Point a, Point b, Point c, Point d, bool skinned = false) {
    triangle(mesh, a, b, c, skinned);
    triangle(mesh, a, c, d, skinned);
}

void add_one_bone(eawr::assets::Model& model) {
    eawr::assets::Bone bone;
    bone.name = "synthetic_root";
    bone.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    model.bones.push_back(bone);
}

[[nodiscard]] eawr::assets::Model hull(bool skinned = false) {
    eawr::assets::Submesh surface;
    surface.shader = "invented_hull";
    if (skinned) surface.skin_bones = {0};
    constexpr std::array<Point, 10> rim{{
        {-2.6F, -6.7F, 0.0F}, {2.6F, -6.7F, 0.0F}, {3.5F, -4.2F, 0.0F},
        {3.5F, 2.7F, 0.0F}, {2.6F, 4.7F, 0.0F}, {0.0F, 7.0F, 0.0F},
        {-2.6F, 4.7F, 0.0F}, {-3.5F, 2.7F, 0.0F}, {-3.5F, -4.2F, 0.0F},
        {-2.6F, -6.7F, 0.0F}}};
    for (std::size_t i = 0; i + 1 < rim.size(); ++i) triangle(surface, {0.0F, 0.0F, 0.0F}, rim[i], rim[i + 1], skinned);
    eawr::assets::Mesh mesh;
    mesh.name = "synthetic_tapered_ship_deck";
    mesh.bounds_min = {-3.5F, -6.7F, 0.0F};
    mesh.bounds_max = {3.5F, 7.0F, 0.0F};
    mesh.submeshes.push_back(std::move(surface));
    eawr::assets::Model model;
    model.meshes.push_back(std::move(mesh));
    if (skinned) add_one_bone(model);
    return model;
}

[[nodiscard]] eawr::assets::Model bridge(bool skinned = false) {
    eawr::assets::Submesh surface;
    surface.shader = "invented_bridge";
    if (skinned) surface.skin_bones = {0};
    // Narrow high superstructure, with a lower rear block for ship silhouette.
    const auto box = [&surface, skinned](float x0, float x1, float y0, float y1, float top) {
        quad(surface, {x0,y0,top}, {x1,y0,top}, {x1,y1,top}, {x0,y1,top}, skinned);
        quad(surface, {x0,y0,0.0F}, {x1,y0,0.0F}, {x1,y0,top}, {x0,y0,top}, skinned);
        quad(surface, {x1,y0,0.0F}, {x1,y1,0.0F}, {x1,y1,top}, {x1,y0,top}, skinned);
        quad(surface, {x1,y1,0.0F}, {x0,y1,0.0F}, {x0,y1,top}, {x1,y1,top}, skinned);
        quad(surface, {x0,y1,0.0F}, {x0,y0,0.0F}, {x0,y0,top}, {x0,y1,top}, skinned);
    };
    box(-1.1F, 1.1F, -1.6F, 0.4F, 2.6F);
    box(-1.7F, 1.7F, -3.4F, -2.2F, 0.8F);
    eawr::assets::Mesh mesh;
    mesh.name = "synthetic_raised_ship_bridge";
    mesh.bounds_min = {-1.7F, -3.4F, 0.0F};
    mesh.bounds_max = {1.7F, 0.4F, 2.6F};
    mesh.submeshes.push_back(std::move(surface));
    eawr::assets::Model model;
    model.meshes.push_back(std::move(mesh));
    if (skinned) add_one_bone(model);
    return model;
}

[[nodiscard]] eawr::assets::Model space_backdrop() {
    eawr::assets::Submesh surface;
    surface.shader = "invented_space";
    quad(surface, {-8.0F,-8.0F,-0.8F}, {8.0F,-8.0F,-0.8F},
        {8.0F,8.0F,-0.8F}, {-8.0F,8.0F,-0.8F});
    eawr::assets::Mesh mesh;
    mesh.name = "synthetic_noncasting_space_backdrop";
    mesh.bounds_min = {-8.0F,-8.0F,-0.8F};
    mesh.bounds_max = {8.0F,8.0F,-0.8F};
    mesh.submeshes.push_back(std::move(surface));
    eawr::assets::Model model;
    model.meshes.push_back(std::move(mesh));
    return model;
}

[[nodiscard]] eawr::assets::Texture texture(std::array<std::uint8_t, 4> rgba) {
    eawr::assets::Texture result;
    result.width = 4;
    result.height = 4;
    result.format = eawr::assets::PixelFormat::rgba8;
    eawr::assets::MipLevel mip{4, 4, 16, {}};
    for (int pixel = 0; pixel < 16; ++pixel)
        for (const std::uint8_t channel : rgba) mip.bytes.push_back(static_cast<std::byte>(channel));
    result.mips.push_back(std::move(mip));
    return result;
}

[[nodiscard]] eawr::assets::Texture space_texture() {
    eawr::assets::Texture result;
    result.width = 64;
    result.height = 64;
    result.format = eawr::assets::PixelFormat::rgba8;
    eawr::assets::MipLevel mip{64, 64, 256, {}};
    constexpr std::array<std::array<int, 2>, 14> stars{{
        {3, 5}, {12, 22}, {18, 49}, {25, 9}, {30, 31}, {35, 56}, {42, 14},
        {48, 43}, {56, 4}, {61, 26}, {6, 58}, {52, 59}, {39, 39}, {20, 34}}};
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            bool star = false;
            for (const auto& point : stars) star = star || (point[0] == x && point[1] == y);
            const auto rgba = star ? std::array<std::uint8_t, 4>{185, 205, 235, 255}
                                   : std::array<std::uint8_t, 4>{20, 26, 43, 255};
            for (const std::uint8_t channel : rgba) mip.bytes.push_back(static_cast<std::byte>(channel));
        }
    }
    result.mips.push_back(std::move(mip));
    return result;
}

[[nodiscard]] presentation::MaterialDescription material(bool receiving = true, const std::string& selector = "static") {
    presentation::MaterialDescription result;
    result.route = presentation::MaterialRoute::legacy_effect;
    result.pass = presentation::RenderPass::opaque;
    result.program = selector == "mesh_bump_colorize" ? "MeshBumpColorize.fx"
        : selector == "rskin_gloss" ? "RSkinGloss.fx" : "BatchMeshGloss.fx";
    result.technique = selector == "mesh_bump_colorize" ? "t0" : "sph_t1";
    result.pass_name = selector == "mesh_bump_colorize" ? "t0_p0" : "sph_t1_p0";
    if (!receiving) result.bindings.push_back({"Diffuse", eawr::assets::Vec4f{1.0F,1.0F,1.0F,1.0F}});
    return result;
}

[[nodiscard]] std::string quote(const std::string& value) {
    std::string result = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') result.push_back('\\');
        if (c == '\n') { result += "\\n"; continue; }
        if (static_cast<unsigned char>(c) >= 32) result.push_back(c);
    }
    return result + '"';
}

struct Means { std::size_t pixels{}; std::array<double, 3> rgb{}; };

// Masks are fixed before captures. They lie strictly inside the deck polygon,
// away from its edge, the bridge itself, and each other.
[[nodiscard]] Means region(const PackedByteArray& data, bool shadow_region) {
    Means result;
    for (int py = 0; py < height; ++py) {
        const float source_y = (height * 0.5F - py - 0.5F) * (2.0F * half_span / height);
        for (int px = 0; px < width; ++px) {
            const float source_x = (px + 0.5F - width * 0.5F) * (2.0F * half_span / width);
            const bool chosen = shadow_region
                ? source_x > -0.7F && source_x < 0.7F && source_y > 1.3F && source_y < 2.4F
                : source_x > 2.1F && source_x < 2.8F && source_y > 1.3F && source_y < 2.4F;
            if (!chosen) continue;
            const std::int64_t index = (static_cast<std::int64_t>(py) * width + px) * 4;
            if (index + 3 >= data.size()) continue;
            ++result.pixels;
            for (int c = 0; c < 3; ++c) result.rgb[static_cast<std::size_t>(c)] += data[index + c];
        }
    }
    if (result.pixels > 0) for (double& c : result.rgb) c /= static_cast<double>(result.pixels);
    return result;
}

[[nodiscard]] double luminance(const Means& value) {
    return 0.2126 * value.rgb[0] + 0.7152 * value.rgb[1] + 0.0722 * value.rgb[2];
}

[[nodiscard]] std::string means_json(const Means& value) {
    std::ostringstream out;
    out << "{\"pixels\":" << value.pixels << ",\"rgb\":["
        << value.rgb[0] << ',' << value.rgb[1] << ',' << value.rgb[2] << "]}";
    return out.str();
}

[[nodiscard]] std::string skin_matrix_json(float source_x) {
    std::ostringstream out;
    out << "[1,0,0,0,0,1,0,0,0,0,1,0," << source_x << ",0,0,1]";
    return out.str();
}

class EawrLightingRendererProbe final : public Node {
    GDCLASS(EawrLightingRendererProbe, Node)
protected:
    static void _bind_methods() {}
public:
    void _ready() override;
    void _process(double) override;
private:
    struct Capture { std::string name; Means receiver; Means control; std::string file; float bridge_source_x{}; };
    struct Step { std::string name; std::function<void()> action; };
    void check(bool condition, const std::string& reason);
    void make_renderer(bool receiver_before_lighting);
    void submit(bool with_bridge, bool noncasting = false);
    void bind_skin_poses(float bridge_source_x = 0.0F, bool noncasting = false);
    void capture(const std::string& name);
    void finish();
    SubViewport* viewport_{};
    Node3D* host_{};
    std::unique_ptr<GodotRenderer> renderer_;
    presentation::FixedCamera camera_{};
    GodotRenderer::LightingState lighting_{};
    std::vector<Step> steps_;
    std::vector<Capture> captures_;
    std::vector<std::string> failures_;
    std::string output_dir_;
    std::string report_path_;
    std::size_t step_{};
    int wait_frames_{};
    int frames_{};
    std::size_t normal_receivers_{};
    std::size_t no_receiver_count_{};
    std::size_t variant_failures_{};
    std::size_t normal_instances_{};
    std::string selector_ = "static";
    std::string source_sha256_;
    std::string library_sha256_;
    std::string build_identity_;
    float bridge_source_x_{};
    std::vector<GodotRenderer::SkinBindingEvidence> normal_bindings_;
    bool finished_{};
};

void EawrLightingRendererProbe::check(bool condition, const std::string& reason) {
    if (!condition) failures_.push_back(reason);
}

void EawrLightingRendererProbe::make_renderer(bool receiver_before_lighting) {
    renderer_.reset();
    renderer_ = std::make_unique<GodotRenderer>(*host_);
    renderer_->set_camera(camera_);
    const auto white = texture({210, 216, 225, 255});
    if (receiver_before_lighting) {
        const auto result = renderer_->upload(hull_asset, hull(selector_ != "static"), white,
            material(false, selector_));
        check(result.has_value(), "receiver-disabled hull upload");
    }
    renderer_->set_lighting(lighting_);
    check(lighting_.shadows, "shadows were not enabled before normal upload");
    if (!receiver_before_lighting) {
        const auto result = renderer_->upload(hull_asset, hull(selector_ != "static"), white,
            material(true, selector_));
        check(result.has_value(), "hull upload");
    }
    const auto dark = texture({85, 99, 120, 255});
    check(renderer_->upload(bridge_asset, bridge(selector_ != "static"), dark,
        material(true, selector_)).has_value(), "bridge upload");
    check(renderer_->upload(bridge_no_cast_asset, bridge(selector_ != "static"), dark,
        material(true, selector_)).has_value(), "noncasting bridge upload");
    check(renderer_->upload(sky_asset, space_backdrop(), space_texture(), material()).has_value(),
        "space backdrop upload");
    renderer_->set_casts_shadows(bridge_no_cast_asset, false);
    renderer_->set_casts_shadows(sky_asset, false);
    variant_failures_ += renderer_->shadow_variant_failures();
    check(renderer_->shadow_variant_failures() == 0, "shadow variant failure");
    const std::size_t count = renderer_->shadow_receiving_materials();
    check(count == (receiver_before_lighting ? 3U : 4U), "shadow receiving material count");
    if (receiver_before_lighting) no_receiver_count_ = count;
    else normal_receivers_ = count;
}

void EawrLightingRendererProbe::submit(bool with_bridge, bool noncasting) {
    std::vector<sim::RenderInstance> instances{{sky_entity, sky_asset, identity()},
        {hull_entity, hull_asset, identity()}};
    if (with_bridge) instances.push_back({noncasting ? bridge_no_cast_entity : bridge_entity,
        noncasting ? bridge_no_cast_asset : bridge_asset, identity()});
    renderer_->submit(std::make_shared<const sim::RenderSnapshot>(++frames_, std::move(instances)));
    if (selector_ != "static") bind_skin_poses(bridge_source_x_, noncasting);
    check(renderer_->instance_count() == (with_bridge ? 3U : 2U), "instance count after submit");
    const auto submitted = renderer_->submission_evidence();
    check(submitted.size() == (with_bridge ? 3U : 2U), "submission evidence count");
    for (const auto& item : submitted) check(item.pass == presentation::RenderPass::opaque, "nonopaque submitted pass");
    if (with_bridge && !noncasting) normal_instances_ = submitted.size();
}

void EawrLightingRendererProbe::bind_skin_poses(float bridge_source_x, bool noncasting) {
    const auto pose = [](float source_x) {
        presentation::animation::BonePose bone;
        bone.skin_asset = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, source_x, 0, 0, 1};
        bone.local_asset = bone.skin_asset;
        bone.model_asset = bone.skin_asset;
        return std::array<presentation::animation::BonePose, 1>{bone};
    };
    const auto deck = pose(0.0F);
    check(renderer_->set_skin_pose(hull_entity, hull_asset, deck).has_value(), "deck skin pose bind");
    if (renderer_->instance_count() == 3) {
        const auto caster = pose(bridge_source_x);
        check(renderer_->set_skin_pose(noncasting ? bridge_no_cast_entity : bridge_entity,
            noncasting ? bridge_no_cast_asset : bridge_asset, caster).has_value(), "bridge skin pose bind");
    }
    const auto bindings = renderer_->skin_bindings();
    const std::size_t expected = renderer_->instance_count() == 3 ? 2U : 1U;
    check(bindings.size() == expected, "skin binding count");
    const auto match = [&bindings](sim::EntityId entity, sim::AssetId asset) {
        for (const auto& binding : bindings)
            if (binding.entity_id == entity && binding.asset_id == asset && binding.bone_count == 1) return true;
        return false;
    };
    check(match(hull_entity, hull_asset), "deck entity/asset/one-bone binding");
    if (expected == 2) check(match(noncasting ? bridge_no_cast_entity : bridge_entity,
        noncasting ? bridge_no_cast_asset : bridge_asset), "bridge entity/asset/one-bone binding");
    if (expected == 2 && !noncasting && normal_bindings_.empty()) normal_bindings_ = bindings;
}

void EawrLightingRendererProbe::capture(const std::string& name) {
    Ref<Image> image = viewport_->get_texture()->get_image();
    check(image.is_valid() && image->get_width() == width && image->get_height() == height,
        name + " viewport dimensions");
    if (!image.is_valid()) return;
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray pixels = image->get_data();
    const std::string file = name + ".png";
    const Error saved = image->save_png(String((output_dir_ + "/" + file).c_str()));
    check(saved == OK, name + " PNG save");
    const Means receiver = region(pixels, true);
    const Means control = region(pixels, false);
    check(receiver.pixels > 500 && control.pixels > 250, name + " region coverage");
    captures_.push_back({name, receiver, control, file, bridge_source_x_});
}

void EawrLightingRendererProbe::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) return;
    const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
    for (std::int64_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == String("--eawr-lighting-report")) report_path_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-lighting-output")) output_dir_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-lighting-selector")) selector_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-lighting-source-sha256")) source_sha256_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-lighting-library-sha256")) library_sha256_ = args[i + 1].utf8().get_data();
        if (args[i] == String("--eawr-lighting-build-identity")) build_identity_ = args[i + 1].utf8().get_data();
    }
    if (selector_ != "static" && selector_ != "rskin_gloss" && selector_ != "mesh_bump_colorize") {
        UtilityFunctions::printerr("unknown RSKIN probe selector");
        get_tree()->quit(1);
        return;
    }
    viewport_ = memnew(SubViewport);
    viewport_->set_size(Vector2i(width, height));
    viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
    viewport_->set_use_own_world_3d(true);
    add_child(viewport_);
    host_ = memnew(Node3D);
    viewport_->add_child(host_);
    camera_.width = width;
    camera_.height = height;
    camera_.vertical_fov_degrees = fov;
    camera_.near_plane = 0.5F;
    camera_.far_plane = 80.0F;
    camera_.eye = {0.0F, eye_height, 0.0F};
    camera_.target = {0.0F, 0.0F, 0.0F};
    camera_.up = {0.0F, 0.0F, -1.0F};

    const auto environment = lighting::alo_viewer_default_environment();
    const auto matrices = lighting::source_to_render(lighting::sph_light_all(environment));
    lighting_.sph = matrices.rgb;
    const auto travel = lighting::source_to_render(environment.lights[0].direction);
    lighting_.toward_light = {-travel.x, -travel.y, -travel.z};
    lighting_.specular = {environment.specular.r, environment.specular.g, environment.specular.b};
    lighting_.shadows = true;
    lighting_.shadow_floor = {environment.shadow.r, environment.shadow.g, environment.shadow.b};
    // Camera-to-bounds farthest distance (18 high, 3.5 half-width, 7 bow)
    // times 1.1, rounded up to one source unit.
    lighting_.shadow_max_distance = std::ceil(1.1F * std::sqrt(eye_height * eye_height + 3.5F * 3.5F + 7.0F * 7.0F));
    lighting_.shadow_atlas_size = 2048;
    // The viewer's space shadow bias (#150); the RenderingServer light
    // defaults acne the whole deck on Forward+.
    lighting_.shadow_bias = 2.0F;
    lighting_.shadow_normal_bias = 5.0F;

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
    if (selector_ != "static") {
        steps_.push_back({"caster-palette-translate", [this] {
            // Recreate the normal receiving fixture so the bridge instance transform stays fixed.
            make_renderer(false);
            bridge_source_x_ = 8.0F;
            submit(true);
        }});
        steps_.push_back({"caster_palette_translated", [this] { capture("caster_palette_translated"); }});
        steps_.push_back({"caster-palette-restore", [this] {
            bridge_source_x_ = 0.0F;
            bind_skin_poses();
        }});
        steps_.push_back({"caster_palette_restored", [this] { capture("caster_palette_restored"); }});
    }
    set_process(true);
}

void EawrLightingRendererProbe::_process(double) {
    if (finished_ || Engine::get_singleton()->is_editor_hint()) return;
    if (++frames_ > (selector_ == "static" ? 180 : 240)) { check(false, "frame budget exhausted"); finish(); return; }
    if (wait_frames_ > 0 && --wait_frames_ > 0) return;
    if (step_ < steps_.size()) {
        steps_[step_++].action();
        wait_frames_ = 5;
        return;
    }
    finish();
}

void EawrLightingRendererProbe::finish() {
    finished_ = true;
    const auto backend = renderer_ ? renderer_->backend_info() : presentation::BackendInfo{};
    if (captures_.size() == (selector_ == "static" ? 6U : 8U)) {
        const auto& on = captures_[0];
        const auto& off = captures_[1];
        const double effect = luminance(off.receiver) - luminance(on.receiver);
        check(effect >= 6.0, "hull receiver did not darken by six decoded levels");
        const double control_delta = std::abs(luminance(off.control) - luminance(on.control));
        check(control_delta <= 2.0, "unaffected deck control changed");
        for (std::size_t index = 2; index < captures_.size(); ++index) {
            const auto& sample = captures_[index];
            const double target = index == 2 || index == 7 ? luminance(on.receiver) : luminance(off.receiver);
            check(std::abs(luminance(sample.receiver) - target) <= 2.0,
                sample.name + " receiver contradicts on/off control");
            check(std::abs(luminance(sample.control) - luminance(off.control)) <= 2.0,
                sample.name + " unaffected deck changed");
        }
    } else check(false, selector_ == "static" ? "six captures missing" : "eight captures missing");
    renderer_.reset();
    std::ostringstream out;
    out << "{\"schema\":" << quote(selector_ == "static" ? "eawr-hull-shadow-probe-v1"
        : "eawr-rskin-shadow-probe-v1") << ",\"status\":"
        << quote(failures_.empty() ? "passed" : "failed") << ",\"source\":\"alo_viewer_default\","
        << "\"policy\":\"sh\",\"material\":{\"program\":"
        << quote(selector_ == "mesh_bump_colorize" ? "MeshBumpColorize.fx"
            : selector_ == "rskin_gloss" ? "RSkinGloss.fx" : "BatchMeshGloss.fx")
        << ",\"technique\":" << quote(selector_ == "mesh_bump_colorize" ? "t0" : "sph_t1")
        << ",\"pass_name\":" << quote(selector_ == "mesh_bump_colorize" ? "t0_p0" : "sph_t1_p0")
        << ",\"render_pass\":\"opaque\"},"
        << "\"receiving_materials\":{\"normal\":" << normal_receivers_
        << ",\"receiver_disabled\":" << no_receiver_count_ << ",\"variant_failures\":"
        << variant_failures_ << "},"
        << "\"submitted_instances\":" << normal_instances_ << ",";
    if (selector_ != "static") {
        out << "\"probe_mode\":\"rskin\",\"selector_key\":" << quote(selector_)
            << ",\"identities\":{\"source_sha256\":" << quote(source_sha256_)
            << ",\"library_sha256\":" << quote(library_sha256_)
            << ",\"build\":" << quote(build_identity_) << "},"
            << "\"skeleton\":{\"deck_bones\":1,\"bridge_bones\":1,"
            << "\"rest\":\"identity\",\"skin_bones\":[0],\"vertex_indices\":[0,0,0,0],"
            << "\"vertex_weights\":[1,0,0,0]},"
            << "\"instance_transform\":\"identity_fixed\",\"skin_bindings\":[";
        for (std::size_t i = 0; i < normal_bindings_.size(); ++i) {
            const auto& binding = normal_bindings_[i];
            out << (i ? "," : "") << "{\"entity_id\":" << binding.entity_id
                << ",\"asset_id\":" << binding.asset_id << ",\"bone_count\":"
                << binding.bone_count << "}";
        }
        out << "],";
    }
    out
        << "\"space_background\":{\"selector\":\"BatchMeshGloss.fx/sph_t1/sph_t1_p0\","
        << "\"casts_shadows\":false,\"geometry\":\"64x64 authored starfield plate\"},"
        << "\"lighting\":{\"toward_light\":[" << lighting_.toward_light[0] << ','
        << lighting_.toward_light[1] << ',' << lighting_.toward_light[2]
        << "],\"sh_constant\":[" << lighting_.sph[0][15] << ',' << lighting_.sph[1][15]
        << ',' << lighting_.sph[2][15] << "]},"
        << "\"camera\":{\"width\":" << width << ",\"height\":" << height
        << ",\"eye\":[0," << eye_height << ",0],\"target\":[0,0,0],\"up\":[0,0,-1],"
        << "\"fov\":" << fov << ",\"near\":0.5,\"far\":80},"
        << "\"shadows\":{\"projection\":\"orthogonal\",\"atlas_size\":" << lighting_.shadow_atlas_size
        << ",\"max_distance\":" << lighting_.shadow_max_distance << ",\"floor\":[" << lighting_.shadow_floor[0]
        << ',' << lighting_.shadow_floor[1] << ',' << lighting_.shadow_floor[2] << ']'
        << ",\"bias\":" << *lighting_.shadow_bias << ",\"normal_bias\":" << *lighting_.shadow_normal_bias
        << ",\"filter\":\"engine default; adapter does not set\"},"
        << "\"backend\":{\"version\":" << quote(String(Engine::get_singleton()->get_version_info()["string"]).utf8().get_data())
        << ",\"driver\":" << quote(RenderingServer::get_singleton()->get_current_rendering_driver_name().utf8().get_data())
        << ",\"method\":" << quote(RenderingServer::get_singleton()->get_current_rendering_method().utf8().get_data())
        << ",\"vendor\":" << quote(backend.adapter_vendor) << ",\"adapter\":" << quote(backend.adapter_name)
        << ",\"api\":" << quote(backend.driver_api) << "},"
        << "\"regions\":{\"receiver\":{\"x\":[-0.7,0.7],\"y\":[1.3,2.4]},"
        << "\"control\":{\"x\":[2.1,2.8],\"y\":[1.3,2.4]}},\"captures\":[";
    for (std::size_t i = 0; i < captures_.size(); ++i) {
        const Capture& c = captures_[i];
        out << (i ? "," : "") << "{\"name\":" << quote(c.name) << ",\"png\":" << quote(c.file)
            << ",\"receiver\":" << means_json(c.receiver) << ",\"control\":" << means_json(c.control);
        if (selector_ != "static") {
            out << ",\"deck_skin_asset\":" << skin_matrix_json(0.0F)
                << ",\"bridge_skin_asset\":";
            if (c.name == "missing_caster") out << "null";
            else out << skin_matrix_json(c.bridge_source_x);
            out << ",\"bridge_skin_source_x\":" << c.bridge_source_x;
        }
        out << "}";
    }
    out << "],\"failures\":[";
    for (std::size_t i = 0; i < failures_.size(); ++i) out << (i ? "," : "") << quote(failures_[i]);
    out << "]}\n";
    bool written = false;
    if (!report_path_.empty()) { std::ofstream file(report_path_, std::ios::binary); file << out.str(); written = static_cast<bool>(file); }
    UtilityFunctions::print(String("EAWR hull shadow probe ") + (failures_.empty() ? "passed" : "failed"));
    get_tree()->quit(failures_.empty() && written ? 0 : 1);
}

void initialize(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(EawrLightingRendererProbe);
}
void uninitialize(const ModuleInitializationLevel) {}
} // namespace

extern "C" GDExtensionBool GDE_EXPORT eawr_lighting_renderer_probe_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize);
    init.register_terminator(uninitialize);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}

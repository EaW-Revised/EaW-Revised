// Opt-in GPU experiment using production upload_material/shader_set_code.
// Player-generated programs arrive only through the private test manifest.
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/snapshot.hpp"
#include "legacy/mesh_bump_spec_glow_colorize.hpp"
#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace godot;
namespace assets = eawr::assets;
namespace presentation = eawr::presentation;
namespace sim = eawr::sim;
using presentation::godot_backend::GodotRenderer;

class EawrShaderBridgeProbe final : public Node {
    GDCLASS(EawrShaderBridgeProbe, Node)
    struct Panel {
        std::string name;
        SubViewport* viewport{};
        std::unique_ptr<GodotRenderer> renderer;
    };
    std::vector<Panel> panels_;
    std::string output_;
    std::string failure_;
    int frames_{};
    std::thread::id main_thread_;
    std::atomic<bool> thread_observed_{};
    std::atomic<bool> separate_thread_{};
    static std::string utf8(const String& text) { return text.utf8().get_data(); }
    static assets::Texture texture() {
        assets::Texture result;
        result.width = 2; result.height = 2; result.format = assets::PixelFormat::rgba8;
        assets::MipLevel mip;
        mip.width = 2; mip.height = 2; mip.row_pitch = 8;
        for (const std::uint8_t value : std::array<std::uint8_t, 16>{
            64,128,192,255, 192,64,128,255, 128,192,64,255, 32,96,160,255}) {
            mip.bytes.push_back(std::byte{value});
        }
        result.mips.push_back(std::move(mip));
        return result;
    }
    static assets::Texture texture(const Dictionary& description) {
        if (description.is_empty()) return texture();
        assets::Texture result;
        result.width = static_cast<std::uint32_t>(static_cast<int64_t>(description["width"]));
        result.height = static_cast<std::uint32_t>(static_cast<int64_t>(description["height"]));
        if (!result.width || !result.height || result.width > 4096 || result.height > 4096) return {};
        result.format = assets::PixelFormat::rgba8;
        assets::MipLevel mip;
        mip.width = result.width; mip.height = result.height; mip.row_pitch = result.width * 4;
        if (description.has("file")) {
            const auto bytes = FileAccess::get_file_as_bytes(String(description["file"]));
            for (int64_t at = 0; at < bytes.size(); ++at) mip.bytes.push_back(std::byte{bytes[at]});
        } else {
            const Array bytes = description["rgba8"];
            for (int64_t at = 0; at < bytes.size(); ++at) {
                mip.bytes.push_back(std::byte{static_cast<std::uint8_t>(static_cast<int64_t>(bytes[at]))});
            }
        }
        if (mip.bytes.size() != static_cast<std::size_t>(result.width) * result.height * 4) return {};
        result.mips.push_back(std::move(mip));
        return result;
    }
    static assets::Model mesh() {
        assets::Submesh surface;
        surface.vertices = {
            {{-0.8F,0.0F,-0.8F},{0.0F,-1.0F,0.0F},{},{},{},{}},
            {{0.8F,0.0F,-0.8F},{0.0F,-1.0F,0.0F},{},{},{},{}},
            {{0.8F,0.0F,0.8F},{0.0F,-1.0F,0.0F},{},{},{},{}},
            {{-0.8F,0.0F,0.8F},{0.0F,-1.0F,0.0F},{},{},{},{}},
        };
        for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
            auto& vertex = surface.vertices[i];
            vertex.texcoord[0] = {(i == 1 || i == 2) ? 1.0F : 0.0F, i >= 2 ? 1.0F : 0.0F};
            vertex.tangent = {1.0F,0.0F,0.0F}; vertex.binormal = {0.0F,0.0F,1.0F};
            vertex.color = {0.5F,0.75F,0.25F,1.0F};
        }
        // Source geometry is CCW; production upload converts it to Godot winding.
        surface.indices = {0,1,2,0,2,3};
        assets::Mesh part; part.name = "invented-gate-quad"; part.submeshes.push_back(std::move(surface));
        assets::Model model; model.meshes.push_back(std::move(part));
        return model;
    }
protected:
    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("observe_render_thread"), &EawrShaderBridgeProbe::observe_render_thread);
    }
public:
    void observe_render_thread() {
        separate_thread_ = std::this_thread::get_id() != main_thread_;
        UtilityFunctions::print(separate_thread_ ? "bridge observed_render_thread=Separate" : "bridge observed_render_thread=Safe");
        thread_observed_ = true;
    }
    void _ready() override {
        if (Engine::get_singleton()->is_editor_hint()) return;
        main_thread_ = std::this_thread::get_id();
        RenderingServer::get_singleton()->call_on_render_thread(Callable(this, "observe_render_thread"));
        set_process(true);
        const String manifest_path = OS::get_singleton()->get_environment("EAWR_SHADER_BRIDGE_MANIFEST");
        output_ = utf8(OS::get_singleton()->get_environment("EAWR_SHADER_BRIDGE_OUTPUT"));
        const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(manifest_path));
        if (parsed.get_type() != Variant::DICTIONARY) { failure_ = "invalid manifest"; return; }
        const Dictionary manifest = parsed;
        const Array cases = manifest.get("cases", Array());
        for (int64_t index = 0; index < cases.size(); ++index) {
            const Dictionary item = cases[index];
            Panel panel;
            panel.name = utf8(String(item["name"]));
            panel.viewport = memnew(SubViewport);
            panel.viewport->set_size(Vector2i(256,256));
            panel.viewport->set_use_own_world_3d(true);
            panel.viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
            add_child(panel.viewport);
            auto* host = memnew(Node3D); panel.viewport->add_child(host);
            panel.renderer = std::make_unique<GodotRenderer>(*host);
            presentation::FixedCamera camera;
            camera.width = 256; camera.height = 256;
            camera.eye = {0.0F,0.0F,3.0F}; camera.target = {0.0F,0.0F,0.0F};
            camera.near_plane = 0.1F; camera.far_plane = 100.0F;
            panel.renderer->set_camera(camera);
            if (item.has("lighting")) {
                const Dictionary values = item["lighting"];
                GodotRenderer::LightingState lighting;
                const auto triple = [&values](const char* name, std::array<float,3>& target) {
                    const Array source = values[name];
                    for (int64_t i = 0; i < 3; ++i) target[static_cast<std::size_t>(i)] = static_cast<float>(static_cast<double>(source[i]));
                };
                triple("sun_diffuse", lighting.sun_diffuse);
                triple("specular", lighting.specular);
                triple("toward_light", lighting.toward_light);
                const Array fill = values["sph_fill"];
                for (int64_t channel = 0; channel < 3; ++channel) {
                    const Array matrix = fill[channel];
                    for (int64_t i = 0; i < 16; ++i) lighting.sph_fill[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)]
                        = static_cast<float>(static_cast<double>(matrix[i]));
                }
                panel.renderer->set_lighting(lighting);
            }
            presentation::MaterialDescription material;
            material.route = presentation::MaterialRoute::modern_spatial;
            material.program = utf8(String(item["shader"]));
            if (material.program == "@hand-control") {
                material.program = presentation::godot_backend::legacy::mesh_bump_spec_glow_colorize::shader(
                    presentation::RenderPass::opaque);
                const std::string authored = "CUSTOM0.x * tangent_model + CUSTOM0.y * normal_model\n        + CUSTOM0.z * cross(normal_model, tangent_model)";
                const auto at = material.program.find(authored);
                if (at == std::string::npos) { failure_ = "hand control tangent seam missing"; return; }
                material.program.replace(at, authored.size(), "BINORMAL");
                if (static_cast<bool>(item.get("flat_normal", false))) {
                    const std::string sample = "texture(NormalTexture, UV)";
                    const auto normal_at = material.program.find(sample);
                    if (normal_at == std::string::npos) { failure_ = "hand normal seam missing"; return; }
                    material.program.replace(normal_at, sample.size(), "vec4(0.5, 0.5, 1.0, 0.3764705882352941)");
                }
                const String sampler = item.get("sampler", "linear_repeat");
                const std::string hints = sampler == "nearest_clamp" ? "filter_nearest, repeat_disable" : "filter_linear, repeat_enable";
                const std::string original = "filter_linear_mipmap, repeat_enable";
                std::size_t offset = 0;
                while ((offset = material.program.find(original, offset)) != std::string::npos) {
                    material.program.replace(offset, original.size(), hints); offset += hints.size();
                }
            }
            const Dictionary bindings = item.get("bindings", Dictionary());
            const Array keys = bindings.keys();
            for (int64_t at = 0; at < keys.size(); ++at) {
                const String key = keys[at];
                const Variant value = bindings[key];
                if (value.get_type() == Variant::ARRAY) {
                    const Array vector = value;
                    if (vector.size() != 3 && vector.size() != 4) { failure_ = "invalid vector binding"; break; }
                    std::array<float,4> floats{};
                    for (int64_t c = 0; c < vector.size(); ++c) floats[static_cast<std::size_t>(c)] = static_cast<float>(static_cast<double>(vector[c]));
                    if (vector.size() == 3) material.bindings.push_back({utf8(key), assets::Vec3f{floats[0],floats[1],floats[2]}});
                    else material.bindings.push_back({utf8(key), assets::Vec4f{floats[0],floats[1],floats[2],floats[3]}});
                } else {
                    material.bindings.push_back({utf8(key), utf8(String(value))});
                }
            }
            // A hand adapter comparison uses the unchanged production selector.
            if (item.has("legacy")) {
                const Dictionary legacy = item["legacy"];
                material.route = presentation::MaterialRoute::legacy_effect;
                material.program = utf8(String(legacy["program"]));
                material.technique = utf8(String(legacy["technique"]));
                material.pass_name = utf8(String(legacy["pass"]));
            }
            std::ofstream input_receipt(output_ + "/" + panel.name + "-input.txt");
            input_receipt << "program_sha256=" << utf8(String::utf8(material.program.c_str()).sha256_text())
                          << "\nroute=" << (item.has("legacy") ? "production-selector" : "modern-spatial") << '\n';
            // Private sampler experiments use an exported hand-adapter source;
            // production family selection and its sampler defaults stay unchanged.
            const Dictionary images = item.get("textures", Dictionary());
            std::vector<GodotRenderer::BindingTexture> textures;
            for (const auto& binding : material.bindings) {
                if (std::holds_alternative<std::string>(binding.value) && binding.name != "BaseTexture") {
                    textures.push_back({binding.name, texture(images.get(String::utf8(binding.name.c_str()), Dictionary()))});
                }
            }
            auto geometry = mesh();
            if (item.has("uv_bounds")) {
                const Array bounds = item["uv_bounds"];
                for (auto& vertex : geometry.meshes[0].submeshes[0].vertices) {
                    auto& uv = vertex.texcoord[0];
                    uv.x = static_cast<float>(static_cast<double>(bounds[0])) + uv.x * static_cast<float>(static_cast<double>(bounds[2]));
                    uv.y = static_cast<float>(static_cast<double>(bounds[1])) + uv.y * static_cast<float>(static_cast<double>(bounds[3]));
                }
            }
            const auto uploaded = panel.renderer->upload(1, geometry, texture(images.get("BaseTexture", Dictionary())), material, textures);
            if (!uploaded) failure_ = panel.name + ": " + uploaded.error().message;
            using Fixed = sim::math::Fixed;
            const auto z = Fixed::from_raw(0), u = Fixed::from_raw(Fixed::scale);
            sim::math::Mat3x4 transform{};
            transform.rows[0] = {u,z,z,z}; transform.rows[1] = {z,u,z,z}; transform.rows[2] = {z,z,u,z};
            panel.renderer->submit(std::make_shared<const sim::RenderSnapshot>(1,
                std::vector<sim::RenderInstance>{{1,1,transform}}));
            panels_.push_back(std::move(panel));
        }
        if (panels_.empty()) failure_ = "no panels";
    }
    void _process(double) override {
        if (Engine::get_singleton()->is_editor_hint() || ++frames_ < 30) return;
        if (!thread_observed_) { if (frames_ < 180) return; failure_ = "render thread callback missing"; }
        if (!separate_thread_) failure_ = "Separate render thread required";
        const Ref<Image> board = Image::create_empty(static_cast<int32_t>(panels_.size()) * 256,
            256, false, Image::FORMAT_RGBA8);
        int32_t column = 0;
        for (auto& panel : panels_) {
            const auto image = panel.viewport->get_texture()->get_image();
            if (image.is_null() || image->save_png(String::utf8((output_ + "/" + panel.name + ".png").c_str())) != OK) {
                failure_ = "capture failed";
            } else if (board.is_valid()) {
                image->convert(Image::FORMAT_RGBA8);
                board->blit_rect(image, Rect2i(0,0,256,256), Vector2i(column * 256,0));
            }
            ++column;
        }
        if (board.is_null() || board->save_png(String::utf8((output_ + "/comparison.png").c_str())) != OK) {
            failure_ = "comparison capture failed";
        }
        std::ofstream report(output_ + "/probe.txt");
        report << "panels=" << panels_.size() << "\nstatus=" << (failure_.empty() ? "passed" : "failed")
               << "\n" << failure_ << '\n';
        report.close();
        const int status = failure_.empty() ? 0 : 1;
        panels_.clear();
        get_tree()->quit(status);
    }
};

namespace {
void initialize(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(EawrShaderBridgeProbe);
}
void uninitialize(const ModuleInitializationLevel) {}
}
extern "C" GDExtensionBool GDE_EXPORT eawr_shader_bridge_probe_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address, GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize); init.register_terminator(uninitialize);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}

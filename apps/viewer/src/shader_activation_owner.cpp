#include "shader_activation_owner.hpp"
#include "shader_prewarm_diagnostics.hpp"
#include "viewer_path.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/json_text.hpp"
#include <godot_cpp/classes/time.hpp>
#include "eawr/scene/scene.hpp"
#include "eawr/assets/asset_names.hpp"
#include "eawr/core/text.hpp"
#include "eawr/mods/chain.hpp"
#include "eawr/presentation/shader_activation_mods.hpp"
#include "eawr/presentation/shader_material_route.hpp"
#include "eawr/presentation/space/environment_scene.hpp"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <ostream>

namespace eawr::presentation::godot_backend {
namespace {
// ST-O1 (docs/ui/ui-settings.md): detail controls are disabled/stored-only.
// This desktop policy is the same for player and evidence runs. The UI work
// can replace it with applied settings without making stored-only fields live.
constexpr std::array<std::uint32_t, 2U> desktop_detail_policy{3U, 2U};
std::string hex(const shader_runtime::Digest& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : value) {
        const auto n = std::to_integer<unsigned>(byte); result += digits[n >> 4U]; result += digits[n & 15U];
    }
    return result;
}
std::string utf8(const godot::String& value) { return value.utf8().get_data(); }
std::optional<std::pair<std::string, assets::Texture>> resolve_shader_texture(const vfs::Vfs& mounted, std::string_view declared) {
    auto path = assets::resolve_texture_name("data/art/textures/", declared,
        [&](std::string_view name) { return bool(mounted.stat(name)); });
    if (path.empty()) return {};
    auto loaded = assets::load_texture(mounted, path);
    if (!loaded) return {};
    return std::pair{std::move(path), std::move(loaded).value()};
}
shader_cache::Identity identity(const shader_runtime::CompilerIdentity& compiler, const shader_techniques::Settings& selection) {
    shader_cache::Identity result;
    result.package_sha256 = hex(compiler.package_lock_sha256);
    result.compiler_abi = hex(compiler.public_c_abi.manifest_sha256);
    result.capability_manifest = hex(compiler.capability_manifest_sha256);
    // Outer activation policy; each stage additionally keys its exact resolved
    // options manifest and freshly prepared typed request in Compiler::stage.
    constexpr std::string_view options = "desktop330/preserve-depth/preserve-y/blocks/combine/1";
    result.options_manifest_sha256 = core::sha256_hex({reinterpret_cast<const std::uint8_t*>(options.data()), options.size()});
    result.result_schema_version = compiler.result_schema_version;
    result.options_manifest_version = compiler.options_manifest_version;
    result.resolver_policy = "effective-vfs/1";
    result.translator = "public-p2b/1"; result.emitter = "p4-material/" + std::to_string(shader_material::policy_version);
    result.binding_policy = "p3/1"; result.state_policy = "opaque-canonical/1";
    result.technique_policy = "p6-finite-declaration-order/1/desktop/" + std::to_string(selection.shader_detail)
        + "/" + std::to_string(selection.water_detail) + "/" + std::to_string(selection.vendor);
    result.admission_policy = "godot-offscreen-two-layout/1";
    const godot::Dictionary version = godot::Engine::get_singleton()->get_version_info();
    result.godot_engine = utf8(godot::String(version.get("string", "unknown")));
    result.shader_abi = "spatial-instance-registers/1"; result.language = "godot-spatial";
    auto* rendering = godot::RenderingServer::get_singleton();
    result.renderer = utf8(rendering->get_current_rendering_method());
    result.backend = utf8(rendering->get_current_rendering_driver_name());
    result.platform = utf8(godot::OS::get_singleton()->get_name());
    result.architecture = sizeof(void*) == 8U ? "64" : "32";
    for (const auto capability : compiler.capabilities) result.capabilities.push_back(static_cast<std::uint16_t>(capability));
    result.variants = {1U, 10U, 20U, 30U, 41U};
    return result;
}
} // namespace
ShaderActivationOwner::~ShaderActivationOwner() { cancel(); }
core::Result<void> ShaderActivationOwner::begin(bool enabled, const vfs::Vfs& mounted,
    std::string chain_identity, std::span<const shader_activation::LayerPolicy> layers,
    godot::Node3D& host, std::shared_ptr<GodotShaderCache> shaders) {
    cancel();
    work_ = {};
    started_us_ = godot::Time::get_singleton()->get_ticks_usec();
    // No inventory, package, user cache or renderer access on the default OFF path.
    if (!enabled) return core::Result<void>::success();
    if (!shaders || !godot::RenderingServer::get_singleton())
        return core::Result<void>::failure({.code = "EAWR-SHADER-OWNER-0001",
            .message = "shader activation requires a shared renderer cache",
            .logical_path = {}, .line = {}, .column = {}, .source_id = {}});
    const auto vendor = shader_material::routing::recognized_pci_vendor(
        utf8(godot::RenderingServer::get_singleton()->get_video_adapter_vendor()));
    if (!vendor) return core::Result<void>::failure({.code = "EAWR-SHADER-OWNER-0002",
        .message = "shader activation requires a recognized actual video adapter vendor",
        .logical_path = "apps/viewer/src/shader_activation_owner.cpp",
        .line = {}, .column = {}, .source_id = {}});
    selection_ = {desktop_detail_policy[0U], desktop_detail_policy[1U], *vendor, false};
    id_ = publisher_.begin();
    shader_runtime::Runtime runtime;
    auto* os = godot::OS::get_singleton();
    const auto executable = ViewerPath(utf8(os->get_executable_path())).native();
    // Release layout: <release>/godot/Godot.exe and <release>/shader-runtime.
    // No drive search, mod-owned DLL path or mutable package manifest is trusted.
    static_cast<void>(runtime.open(executable.parent_path().parent_path() / "shader-runtime"));
    shader_runtime::CompilerSession session(std::move(runtime));
    const auto verified = shader_runtime::compiler_identity(session);
    shader_activation::Identity key;
    if (verified) {
        const auto base = identity(*verified, selection_);
        key = [base](const shader_activation::Input&) { return std::optional(base); };
    }
    shader_activation::Cancellation cancellation;
    auto snapshot = shader_activation::capture(mounted, std::move(chain_identity), layers, {},
        [](std::string_view effect) {
            constexpr std::string_view root = "data/art/shaders/";
            if (effect.starts_with(root)) effect.remove_prefix(root.size());
            for (const auto extension : {".fx", ".fxo"}) {
                const auto candidate = std::string(effect) + extension;
                if (const auto* adapter = scene::find_legacy_selector(candidate)) return std::string(adapter->program);
                if (space::has_environment_adapter(candidate)) return candidate;
            }
            return std::string{};
        }, shader_activation::source_freezer(session, mounted, "data/art/shaders"), cancellation, {}, key);
    if (!snapshot) { publisher_.cancel(id_); return core::Result<void>::failure(snapshot.error()); }
    snapshot_bytes_ = snapshot.value().input_bytes;
    effects_ = snapshot.value().inputs.size();
    auto renderer = std::make_shared<GodotRenderer>(host, std::move(shaders), false);
    auto cache = std::make_shared<shader_cache::Cache>(ViewerPath(utf8(os->get_user_data_dir())).native() / "shader-cache",
        shader_cache::Limits{256U * 1024U * 1024U, 32U * 1024U * 1024U, 4096U, 8192U});
    compiler_ = std::make_unique<shader_material::Compiler>(std::move(session), std::move(cache),
        [this, renderer](const shader_material::Program& program) -> std::shared_ptr<const shader_activation::Artifact> {
            auto warmed = renderer->prewarm_shader_material(program.material);
            if (!warmed) {
                append_prewarm_refusal(prewarm_refusals_, warmed.error());
                return nullptr;
            }
            return std::move(warmed).value();
        }, selection_);
    job_ = std::make_unique<shader_activation::Job>(id_, std::move(snapshot.value().chain_identity),
        std::move(snapshot.value().inputs), compiler_->operations());
    return core::Result<void>::success();
}
bool ShaderActivationOwner::step(std::size_t budget) {
    if (!job_) return true;
    job_->step(budget);
    work_ = compiler_->work();
    if (job_->progress().state == shader_activation::State::running) return false;
    if (job_->progress().state == shader_activation::State::complete) {
        const auto policy = "selection_policy=desktop (ST-O1; player and evidence runs) shader_detail="
            + std::to_string(selection_.shader_detail) + " water_detail=" + std::to_string(selection_.water_detail)
            + " vendor_pci=" + std::to_string(selection_.vendor) + "\n";
        auto completion = shader_activation::complete(publisher_, *job_,
            {65536U, 262144U, 4096U, 16U, 4096U, 256U * 1024U - policy.size() - prewarm_refusals_.size()});
        if (completion) {
            report_ = std::move(completion->report);
            report_->details.insert(0U, policy);
            report_->details += prewarm_refusals_;
            receipt_ = std::make_shared<const ShaderActivationReceipt>(ShaderActivationReceipt{
                work_, snapshot_bytes_, compiler_->resident_bytes(), effects_,
                godot::Time::get_singleton()->get_ticks_usec() - started_us_, report_->details});
        }
    }
    // Drop callback borrows before the compiler, its DLLs and prewarm renderer.
    job_.reset(); compiler_.reset(); return true;
}
void ShaderActivationOwner::cancel() {
    if (job_) job_->cancel();
    if (id_) publisher_.cancel(id_);
    job_.reset(); compiler_.reset(); report_.reset(); receipt_.reset(); prewarm_refusals_.clear(); id_ = 0U;
}
std::shared_ptr<const shader_activation::Generation> ShaderActivationOwner::generation() const {
    auto active = publisher_.active();
    return active && active->id == id_ ? active : nullptr;
}
core::Result<void> begin_mod_shaders(ShaderActivationOwner& owner, const vfs::Vfs& mounted,
    std::span<const vfs::LayerRoot> roots, std::string_view profile, godot::Node3D& host,
    std::shared_ptr<GodotShaderCache> shaders) {
    std::vector<shader_activation::LayerPolicy> policies;
    mods::Catalog contributors;
    std::string chain(profile);
    for (const auto& [id, root] : roots) {
        chain += ":" + std::to_string(id.size()) + ":" + id;
        if (id == "base" || id == "expansion") { policies.push_back({id, false}); continue; }
        mods::ModRecord record; record.key = id;
        record.root = ViewerPath::utf8(core::iequals(ViewerPath::utf8(root.filename()), "Data") ? root.parent_path() : root);
        contributors.records.push_back(std::move(record));
    }
    mods::read_mod_metadata(contributors);
    for (const auto& record : contributors.records) policies.push_back(shader_activation::contributor_policy(record.key, record));
    return owner.begin(true, mounted, std::move(chain), policies, host, std::move(shaders));
}
void install_mod_shaders(GodotRenderer& renderer, std::shared_ptr<const shader_activation::Generation> generation,
    std::shared_ptr<const vfs::Vfs> mounted) {
    if (!generation || !mounted) return;
    renderer.set_shader_generation(std::move(generation),
        [mounted = std::move(mounted)](std::string_view declared) -> std::optional<std::pair<std::string, assets::Texture>> {
            return resolve_shader_texture(*mounted, declared);
        });
}
void install_mod_shaders(GodotRenderer& renderer, std::shared_ptr<const shader_activation::Generation> generation,
    const vfs::Vfs& mounted) {
    if (!generation) return;
    renderer.set_shader_generation(std::move(generation),
        [&mounted](std::string_view declared) { return resolve_shader_texture(mounted, declared); });
}
void admit_scene_shaders(scene::VfsAssetCache& cache, GodotRenderer& renderer) {
    if (renderer.shader_translation_enabled()) cache.set_admitted_shaders(
        [&renderer](const assets::Submesh& surface) { return renderer.usable_shader(surface); });
}
void write_shader_activation_report(std::ostream& output, const ShaderActivationReceipt* receipt) {
    if (!receipt) return;
    const auto& work = receipt->work;
    output << "  \"shader_activation\":{\"elapsed_us\":" << receipt->elapsed_us
        << ",\"effects\":" << receipt->effects << ",\"snapshot_bytes\":" << receipt->snapshot_bytes
        << ",\"resident_cpu_bytes\":" << receipt->resident_bytes
        << ",\"source_compilations\":" << work.source_compilations << ",\"translations\":" << work.translations
        << ",\"cache_hits\":" << work.cache_hits << ",\"admissions\":" << work.admissions
        << ",\"prewarms\":" << work.prewarms << ",\"details\":" << core::json_quote(receipt->details) << "},\n";
}
void write_shader_route_report(std::ostream& output, const GodotRenderer* renderer) {
    if (!renderer || !renderer->shader_translation_enabled()) return;
    const auto work = renderer->translation_work();
    shader_material::routing::write_route_report(output, true, work.uploaded, work.approximated, work.refused, work.missing_green);
}
} // namespace eawr::presentation::godot_backend

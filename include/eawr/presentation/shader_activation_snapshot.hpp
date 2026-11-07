#pragma once
#include "eawr/presentation/shader_activation.hpp"
#include "eawr/presentation/shader_source.hpp"

namespace eawr::presentation::shader_activation {
struct LayerPolicy {
    std::string layer;
    bool mod{};
    Policy policy{Policy::allowed};
};
struct SourceSnapshot final : Artifact {
    shader_runtime::PreprocessedSource source;
};
struct Snapshot {
    std::string chain_identity;
    std::vector<Input> inputs;
    std::size_t compiled_inventory{}, source_inventory{}, reachable{}, input_bytes{}, source_opens{};
};
struct SnapshotLimits {
    Limits job;
    std::size_t inventory_entries{262144}, source_bytes{64U * 1024U * 1024U};
};
// Invoked during capture only, before any job is dispatched. Return a frozen
// expanded source + full include/macro/compiler identities, never a live reader.
using FreezeSource = std::function<shader_runtime::SourcePreparation(const vfs::AssetRecord&, const Cancellation&)>;
using Adapter = std::function<std::string(std::string_view)>;
// Owner supplies backend/policy identities. capture verifies the selected root
// and computes P7's canonical key, including the complete source closure.
using Identity = std::function<std::optional<shader_cache::Identity>(const Input&)>;
[[nodiscard]] std::optional<shader_cache::Key> activation_key(const Input& input, shader_cache::Identity identity, Diagnostic* refusal = nullptr);
// Inventory uses effective FXO winners and mod source-only FX, independently of
// models. Reachable stems are separately counted, and absent reachable effects
// get explicit records. Every supplied layer must be unique; unlisted winners
// refuse rather than accidentally treating an unknown mod as retail.
[[nodiscard]] core::Result<Snapshot> capture(const vfs::Vfs& mounted,
    std::string chain_identity, std::span<const LayerPolicy> layers,
    std::span<const std::string> reachable, const Adapter& adapter,
    const FreezeSource& freeze_source, const Cancellation& cancellation, SnapshotLimits limits = {},
    const Identity& identity = {});
// Convenience activation-only SOURCE-A integration; all borrows expire after
// capture. No shader source, include or generated body enters ordinary reports.
[[nodiscard]] FreezeSource source_freezer(shader_runtime::CompilerSession& session,
    const vfs::Vfs& mounted, std::string logical_root,
    std::vector<shader_runtime::SourceMacro> macros = {}, shader_runtime::SourceLimits limits = {});
struct CacheCodec {
    shader_cache::Validate validate;
    std::function<Prepared(std::span<const std::uint8_t>)> decode;
    std::function<std::vector<std::uint8_t>(const Prepared&)> encode;
};
// Validated warm payloads skip translation but still pass admission/prewarm.
// No key means no cache access. Recoverable cache failure keeps fresh results.
[[nodiscard]] Operations cached_operations(Operations uncached, std::shared_ptr<const shader_cache::Cache> cache, CacheCodec codec);

// Completion/report delivery is one transaction from the owner's perspective:
// failed, cancelled, stale and duplicate publications produce no note/report.
struct Completion { std::shared_ptr<const Generation> generation; report::Report report; };
[[nodiscard]] std::optional<Completion> complete(Publisher& publisher, const Job& job, report::Limits limits);
} // namespace eawr::presentation::shader_activation

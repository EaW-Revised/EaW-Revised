#pragma once

#include "eawr/presentation/shader_source.hpp"
#include "eawr/presentation/shader_translation_cache.hpp"
#include <functional>
#include <memory>

// SFX-01..07: activation-only source contracts; no renderer activation switch.
namespace eawr::presentation::shader_source {
inline constexpr std::uint32_t schema_version = 1;
struct Limits {
    std::size_t bytes{16U * 1024U * 1024U}, tokens{262144}, nesting{64}, nodes{65536};
};
struct Location {
    std::string logical;
    std::optional<std::uint32_t> line, column;
};
enum class Failure { syntax, unsupported, budget, stage_compile, descriptor, selection, opted_out };
struct Diagnostic {
    Failure reason{};
    Location location;
    std::optional<shader_runtime::SourceDiagnostic> compiler;
};
struct Assignment {
    std::string name, expression;
    Location location;
    std::string type; // annotation declaration type; empty for state assignments
    std::optional<std::size_t> program; // exact resolved graph stage, never substring matching
};
struct Declaration {
    std::string type, name, semantic, array, expression;
    std::vector<Assignment> annotations, sampler_states;
    std::vector<std::string> qualifiers;
    bool row_major{}, constant{};
    Location location;
    std::optional<std::size_t> program;
};
struct Pass {
    std::string name;
    std::vector<Assignment> annotations, states;
    Location location;
};
struct Technique {
    std::string name;
    std::vector<Assignment> annotations;
    std::vector<Pass> passes; // includes trailing cleanup passes; no draw inference
};
struct Stage {
    std::string entry, profile;
    Location location;
};
struct Graph {
    assets::Source source;
    std::vector<Declaration> declarations;
    std::vector<Technique> techniques;
    std::vector<Stage> stages;
    std::vector<std::byte> stage_text; // parser-identified wrapper removal only
};
struct Parsed {
    std::optional<Graph> graph;
    std::optional<Diagnostic> diagnostic;
};
// Expanded input: #line coordinates are accepted only for known logical inputs.
[[nodiscard]] Parsed parse(std::span<const std::byte> expanded, assets::Source root,
    std::span<const assets::Source> inputs = {}, Limits limits = {});

// Heap-owned, immovable backing storage. Views survive movement of this handle.
class Effect final {
public:
    ~Effect();
    Effect(Effect&&) noexcept;
    Effect& operator=(Effect&&) noexcept;
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;
    [[nodiscard]] const assets::EffectView& view() const;
private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit Effect(std::unique_ptr<Storage> storage);
    friend struct Builder;
};
struct Produced {
    std::optional<Effect> effect;
    std::optional<Diagnostic> diagnostic;
};
// Every result must pass the existing P1 program reader. Injectable CPU seam.
using Compile = std::function<shader_runtime::SourceCompilation(
    std::span<const std::byte>, std::string_view, std::string_view)>;
[[nodiscard]] Produced produce(Graph graph, const Compile& compile, Limits limits = {});
[[nodiscard]] Produced compile(shader_runtime::CompilerSession& session,
    const shader_runtime::PreprocessedSource& source, Limits limits = {},
    shader_runtime::SourceLimits compiler_limits = {});

enum class Form { absent, compiled, source };
struct Selection {
    Form form{Form::absent};
    std::optional<vfs::AssetRecord> record;
    bool allowed{};
};
// Contributor-aware policy is supplied by the mod activation owner. Required,
// queried before input acquisition and before even constructing a cache key.
using Allowed = std::function<bool(const vfs::AssetRecord&)>;
[[nodiscard]] core::Result<Selection> select(const vfs::Vfs& mounted,
    std::string_view logical_stem, const Allowed& allowed, const Allowed& mod_source);
// Extends the P7 identity in place with the COMPLETE resolved source closure.
// Caller supplies backend/emitter/admission identities from their owners.
[[nodiscard]] bool source_identity(shader_cache::Identity& identity,
    const Selection& selection, const shader_runtime::PreprocessedSource& source,
    const Graph& graph);
enum class Fallback { missing_resource, hand_adapter_approximation, omit };
[[nodiscard]] Fallback fallback(const Selection& selected, bool hand_adapter) noexcept;
} // namespace eawr::presentation::shader_source

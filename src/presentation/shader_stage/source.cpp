#include "internal.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/presentation/shader_source.hpp"
#include <algorithm>
#include <map>
#include <limits>

namespace eawr::presentation::shader_runtime {
namespace {
constexpr std::string_view contract =
    "eawr-source/1;input=hlsl;target=d3d-bytecode;api=0;strip-debug=0;"
    "preprocess=1;selection=fxo-first/1;parser=source-b-pending;descriptor=1;"
    "profiles=vs_1_1,ps_1_1,vs_2_0,ps_2_0,vs_3_0,ps_3_0;"
    "capability=invented-passthrough-only;installed-ps11-not-qualified;"
    "public-hlsl-abi/1;windows-x64-cdecl;Macro(ptr64,ptr64);"
    "Preprocess(i32,ptr64,ptr64,u32,open,close,ptr64)=56;"
    "Hlsl(i32,ptr64,ptr64,Code,ptr64)=48;Code(ptr64,u64);"
    "open=int(cstr,bool1,cstr,ptr64,Code*);close=void(const-Code*,ptr64);"
    "preprocess=int(const-Compile*,Code*,char**);ownership=originating-dll;"
    "callbacks=owned-until-close;failed-code-not-owned;headers=pinned-lock;"
    "macros=ordered-once;stage-includes=refused;no-host-filesystem";
struct Refusal { SourceFailure reason; };
SourcePhase phase(SourceFailure reason, SourcePhase operation) noexcept {
    return reason == SourceFailure::unavailable || reason == SourceFailure::dependency_integrity ||
        reason == SourceFailure::dependency_abi ? SourcePhase::package_load : operation;
}
void require(bool ok, SourceFailure reason) { if (!ok) throw Refusal{reason}; }
void checkpoint(detail::Session& s) {
    require(!s.limits.cancelled || !s.limits.cancelled(s.limits.cancellation_context), SourceFailure::cancelled);
}
void ready(detail::Session& s) {
    if (s.unavailable) {
        if (s.unavailable->reason == DiagnosticReason::dependency_integrity) throw Refusal{SourceFailure::dependency_integrity};
        if (s.unavailable->reason == DiagnosticReason::dependency_abi) throw Refusal{SourceFailure::dependency_abi};
        throw Refusal{SourceFailure::unavailable};
    }
    checkpoint(s);
}
SourceFailure source_error(const core::Diagnostic& error) {
    if (error.code == vfs::diagnostic_codes::not_found) return SourceFailure::missing_include;
    if (error.code == vfs::diagnostic_codes::invalid_path) return SourceFailure::invalid_path;
    if (error.code == assets::shader_source_diagnostic_codes::budget) return SourceFailure::budget_exceeded;
    return SourceFailure::source_io;
}
// Length framing prevents ambiguous concatenations of names/macros/dependencies.
void field(std::string& out, std::string_view text) {
    out += std::to_string(text.size()); out += ':'; out += text;
}
std::string manifest(const detail::Session& s) {
    std::string out(contract);
    field(out, detail::hex(s.identity.package_lock_sha256));
    field(out, detail::hex(s.identity.public_c_abi.manifest_sha256));
    field(out, detail::hex(s.identity.capability_manifest_sha256));
    return out;
}
bool identifier(std::string_view value, std::size_t bound) {
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (value.empty() || value.size() > bound || !alpha(value.front())) return false;
    return std::all_of(value.begin(), value.end(), [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}
struct Includes {
    struct Node { assets::ShaderSource file; const void* parent{}; std::size_t depth{}; };
    detail::Session& session;
    const assets::ShaderSourceProvider* provider{};
    SourceLimits limits;
    Node root;
    std::map<const void*, Node> active;
    std::vector<SourceDependency> dependencies;
    std::optional<SourceFailure> failure;
    std::size_t total{}, opened{}, closed{}, attempts{};
    void accept(const assets::ShaderSource& file) {
        dependencies.push_back({file.source, detail::digest(file.bytes)});
    }
    const Node& parent(const void* data) const {
        if (!data || data == root.file.bytes.data()) return root;
        const auto found = active.find(data);
        require(found != active.end(), SourceFailure::callback_ownership);
        return found->second;
    }
    // Pinned vkd3d_shader.h:1832-1849: true means local/double-quoted,
    // false means system/angle-bracketed. This is not an is_system flag.
    static int open(const char* name, bool local, const char* parent_data, void* context, abi::Code* out) noexcept {
        auto& self = *static_cast<Includes*>(context);
        try {
            require(self.provider != nullptr, SourceFailure::unexpected_include);
            require(!self.failure, self.failure.value_or(SourceFailure::preprocess_failure));
            checkpoint(self.session);
            require(self.attempts < self.limits.include_opens, SourceFailure::budget_exceeded);
            ++self.attempts;
            require(name && out, SourceFailure::callback_ownership);
            std::size_t length{};
            while (length < self.limits.name_bytes && name[length]) ++length;
            require(length < self.limits.name_bytes, SourceFailure::budget_exceeded);
            const auto& ancestor = self.parent(parent_data);
            require(ancestor.depth < self.limits.depth, SourceFailure::budget_exceeded);
            auto file = self.provider->include(ancestor.file.source.logical_path, {name, length}, local,
                std::min(self.limits.file_bytes, self.limits.total_bytes - self.total));
            if (!file) throw Refusal{source_error(file.error())};
            const Node* walk = &ancestor;
            while (walk) {
                require(walk->file.source.logical_path != file.value().source.logical_path, SourceFailure::include_cycle);
                walk = walk == &self.root ? nullptr : &self.parent(walk->parent);
            }
            // Empty files still get distinct stable callback addresses.
            const auto size = file.value().bytes.size();
            self.accept(file.value());
            if (size == 0) file.value().bytes.push_back(std::byte{});
            const void* data = file.value().bytes.data();
            self.active.emplace(data, Node{std::move(file).value(), parent_data, ancestor.depth + 1});
            self.total += size;
            ++self.opened;
            *out = {data, size};
            return 0;
        } catch (const Refusal& r) { self.failure = r.reason; }
          catch (...) { self.failure = SourceFailure::budget_exceeded; }
        return -4; // pinned vkd3d_types.h:53 INVALID_SHADER; typed cause retained
    }
    static void close(const abi::Code* code, void* context) noexcept {
        auto& self = *static_cast<Includes*>(context);
        if (!code || self.active.erase(code->data) != 1) self.failure = SourceFailure::callback_ownership;
        else ++self.closed;
    }
};
struct Output {
    detail::Session& session;
    abi::Code code;
    char* messages{};
    int status{-1};
    ~Output() {
        if (status >= 0) session.api.vkd3d_shader_free_shader_code(&code);
        session.api.vkd3d_shader_free_messages(messages);
    }
};
using PreprocessFunction = int (*)(const abi::Compile*, abi::Code*, char**);
}
std::string_view source_compiler_contract() noexcept { return contract; }
SourcePreparation preprocess_source(CompilerSession& session, const assets::ShaderSourceProvider& provider,
    std::string_view logical, std::span<const SourceMacro> macros, SourceLimits limits) {
    SourcePreparation result;
    const auto fail = [&](SourceFailure reason) { result.diagnostic = SourceDiagnostic{phase(reason, SourcePhase::preprocess), reason, {std::string(logical), {}, {}, {}, {}}}; };
    try {
        auto& s = StageSessionAccess::get(session);
        ready(s);
        require(logical.size() <= limits.name_bytes && macros.size() <= limits.macros &&
            macros.size() <= std::numeric_limits<abi::U>::max(), SourceFailure::budget_exceeded);
        auto root = provider.read(logical, std::min(limits.file_bytes, limits.total_bytes));
        if (!root) throw Refusal{source_error(root.error())};
        Includes includes{s, &provider, limits, {std::move(root).value(), nullptr, 0}, {}, {}, {}, 0, 0, 0, 0};
        includes.total = includes.root.file.bytes.size();
        includes.accept(includes.root.file);
        std::vector<abi::Macro> definitions;
        for (const auto& m : macros) {
            require(identifier(m.name, limits.name_bytes) && m.value.size() <= limits.name_bytes &&
                m.value.find('\0') == m.value.npos && m.value.find_first_of("\r\n") == m.value.npos, SourceFailure::invalid_options);
            definitions.push_back({m.name.c_str(), m.value.c_str()});
        }
        abi::Preprocess prep;
        prep.macros = definitions.data(); prep.macro_count = static_cast<abi::U>(definitions.size());
        prep.open = Includes::open; prep.close = Includes::close; prep.context = &includes;
        abi::Compile info;
        // Pinned vkd3d_shader.h:1585 HLSL=2, :1632 D3D_BYTECODE=4.
        info.next = &prep; info.source_type = 2; info.target_type = 4;
        info.source = {includes.root.file.bytes.data(), includes.root.file.bytes.size()};
        info.source_name = includes.root.file.source.logical_path.c_str();
        const auto preprocess = std::bit_cast<PreprocessFunction>(s.runtime.public_export(Backend::vkd3d, "vkd3d_shader_preprocess"));
        require(preprocess != nullptr, SourceFailure::unavailable);
        Output output{s, {}, nullptr, -1};
        output.status = preprocess(&info, &output.code, &output.messages);
        result.opened = includes.opened; result.closed = includes.closed;
        if (includes.failure) throw Refusal{*includes.failure};
        require(includes.active.empty() && includes.opened == includes.closed, SourceFailure::callback_ownership);
        checkpoint(s);
        require(output.status >= 0, SourceFailure::preprocess_failure);
        require(output.code.size <= limits.expanded_bytes && output.code.data, SourceFailure::budget_exceeded);
        PreprocessedSource source;
        const auto* data = static_cast<const std::byte*>(output.code.data);
        source.expanded.assign(data, data + output.code.size);
        source.expanded_sha256 = detail::digest(source.expanded);
        source.dependencies = std::move(includes.dependencies);
        source.macros.assign(macros.begin(), macros.end());
        source.compiler = s.identity;
        auto identity = manifest(s);
        for (const auto& d : source.dependencies) {
            field(identity, d.source.logical_path); field(identity, d.source.layer_id);
            field(identity, d.source.source_id); field(identity, vfs::to_string(d.source.origin));
            field(identity, detail::hex(d.sha256));
        }
        field(identity, "macros");
        for (const auto& m : macros) { field(identity, m.name); field(identity, m.value); }
        field(identity, detail::hex(source.expanded_sha256));
        source.manifest_sha256 = detail::digest(identity);
        result.source = std::move(source);
    } catch (const Refusal& r) { fail(r.reason); }
      catch (const detail::Failure&) { fail(SourceFailure::unavailable); }
      catch (const std::bad_alloc&) { fail(SourceFailure::budget_exceeded); }
    return result;
}
SourceCompilation compile_source_stage(CompilerSession& session, const PreprocessedSource& source,
    std::span<const std::byte> stage_text, std::string_view entry, std::string_view profile, SourceLimits limits) {
    SourceCompilation result;
    const std::string logical = source.dependencies.empty() ? std::string{} : source.dependencies.front().source.logical_path;
    const auto fail = [&](SourceFailure reason) { result.diagnostic = SourceDiagnostic{phase(reason, SourcePhase::stage_compile), reason, {logical, {}, {}, {}, {}}}; };
    try {
        auto& s = StageSessionAccess::get(session);
        ready(s);
        require(!source.dependencies.empty() && source.compiler.package_lock_sha256 == s.identity.package_lock_sha256,
            SourceFailure::invalid_options);
        require(!stage_text.empty() && stage_text.size() <= limits.expanded_bytes, SourceFailure::budget_exceeded);
        require(identifier(entry, limits.name_bytes), SourceFailure::invalid_options);
        require(profile == "vs_1_1" || profile == "ps_1_1" || profile == "vs_2_0" || profile == "ps_2_0" ||
            profile == "vs_3_0" || profile == "ps_3_0", SourceFailure::unsupported_profile);
        SourceStage stage;
        stage.entry = entry; stage.profile = profile;
        Includes includes{s, nullptr, limits, {}, {}, {}, {}, 0, 0, 0, 0};
        abi::Preprocess prep;
        prep.open = Includes::open; prep.close = Includes::close; prep.context = &includes;
        abi::Hlsl hlsl;
        hlsl.next = &prep; hlsl.entry = stage.entry.c_str(); hlsl.profile = stage.profile.c_str();
        // Pinned vkd3d_shader.h:333 API_VERSION=4, :45 API_1_0=0,
        // :327 STRIP_DEBUG=1 (false/0 retains debug/constant metadata).
        const abi::Option options[]{{4, 0}, {1, 0}};
        abi::Compile info;
        info.next = &hlsl; info.source_type = 2; info.target_type = 4;
        info.options = options; info.option_count = 2;
        info.source = {stage_text.data(), stage_text.size()}; info.source_name = logical.c_str();
        Output output{s, {}, nullptr, -1};
        output.status = s.api.vkd3d_shader_compile(&info, &output.code, &output.messages);
        if (includes.failure) throw Refusal{*includes.failure};
        checkpoint(s);
        require(output.status >= 0, SourceFailure::compiler_failure);
        require(output.code.data && output.code.size <= std::min(limits.output_bytes, s.limits.input_bytes), SourceFailure::budget_exceeded);
        const auto* data = static_cast<const std::byte*>(output.code.data);
        stage.bytecode.assign(data, data + output.code.size);
        stage.source = source.dependencies.front().source;
        stage.source.stored_size = stage.bytecode.size();
        const auto program = view_source_program(stage);
        require(program && program.value().major == static_cast<unsigned>(profile[3] - '0') &&
            program.value().minor == static_cast<unsigned>(profile[5] - '0') &&
            program.value().stage == (profile.starts_with("vs") ? assets::EffectProgramStage::vertex : assets::EffectProgramStage::pixel),
            SourceFailure::invalid_output);
        auto identity = manifest(s);
        field(identity, detail::hex(source.manifest_sha256));
        field(identity, detail::hex(detail::digest(stage_text)));
        field(identity, entry); field(identity, profile);
        stage.manifest_sha256 = detail::digest(identity);
        result.stage = std::move(stage);
    } catch (const Refusal& r) { fail(r.reason); }
      catch (const detail::Failure&) { fail(SourceFailure::unavailable); }
      catch (const std::bad_alloc&) { fail(SourceFailure::budget_exceeded); }
    return result;
}
core::Result<assets::EffectProgram> view_source_program(const SourceStage& stage) {
    return assets::read_shader_program(stage.bytecode, stage.source);
}
} // namespace eawr::presentation::shader_runtime

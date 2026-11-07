#include "internal.hpp"
#include <algorithm>
#include <cstring>

namespace eawr::presentation::shader_runtime {
CompilerSession::CompilerSession(Runtime runtime, StageLimits limits)
    : impl_(std::make_unique<Impl>(std::move(runtime), limits)) {
    auto &s = *impl_;
    const auto status = s.runtime.status();
    if (status != Status::ready || s.runtime.verified_lock_sha256().empty()) {
        auto reason = DiagnosticReason::dependency_unavailable;
        if (status == Status::integrity_failure || status == Status::ready)
            reason = DiagnosticReason::dependency_integrity;
        if (status == Status::missing_export || status == Status::abi_mismatch)
            reason = DiagnosticReason::dependency_abi;
        s.unavailable = StageDiagnostic{
            DiagnosticPhase::package_load, reason, Severity::error,
            std::nullopt, "verified pinned shader runtime unavailable"};
        return;
    }
    try {
#define EAWR_STAGE_BIND_VKD3D(ret, name, args)                                 \
    s.api.name = std::bit_cast<decltype(s.api.name)>(                          \
        s.runtime.public_export(Backend::vkd3d, #name));                       \
    detail::require(s.api.name != nullptr, DiagnosticPhase::package_load,      \
                    DiagnosticReason::dependency_abi,                          \
                    "required stage compiler C export missing");
        EAWR_STAGE_VKD3D_API(EAWR_STAGE_BIND_VKD3D)
#undef EAWR_STAGE_BIND_VKD3D
#define EAWR_STAGE_BIND_CROSS(ret, name, args)                                 \
    s.api.name = std::bit_cast<decltype(s.api.name)>(                          \
        s.runtime.public_export(Backend::spirv_cross, #name));                 \
    detail::require(s.api.name != nullptr, DiagnosticPhase::package_load,      \
                    DiagnosticReason::dependency_abi,                          \
                    "required reflection C export missing");
        EAWR_STAGE_CROSS_API(EAWR_STAGE_BIND_CROSS)
#undef EAWR_STAGE_BIND_CROSS
        auto &id = s.identity;
        // Parse the generated trusted digest, never a manifest read from disk.
        const auto pin = s.runtime.verified_lock_sha256();
        const auto nibble = [](char c) {
            return c <= '9' ? c - '0' : c - 'a' + 10;
        };
        for (std::size_t i = 0; i < id.package_lock_sha256.size(); ++i)
            id.package_lock_sha256[i] = static_cast<std::byte>(
                nibble(pin[i * 2]) * 16 + nibble(pin[i * 2 + 1]));
        id.public_c_abi.vkd3d_public_api = {2, 1};
        id.public_c_abi.spirv_cross_c_abi = {0, 61, 0};
        id.public_c_abi.pointer_bits = 64;
        std::string declarations =
            "eawr-stage-public-c-abi/"
            "1;windows-x64-cdecl;U=u32;H=opaque-ptr64;B=u8;int=i32;size_t=u64;"
            "Code(ptr64,u64);Compile(i32,ptr64,Code,i32,i32,ptr64,u32,i32,"
            "ptr64);"
            "Option(i32,u32);"
            "Descriptor(i32,u32,u32,i32,i32,u32,u32);"
            "Descriptors(i32,ptr64,ptr64,u32);Pairs(i32,ptr64,ptr64,u32);Pair("
            "u32,"
            "u32,u32,u32);"
            "Signatures(i32,ptr64,Signature,Signature,Signature);Signature("
            "ptr64,"
            "u32);SignatureElement(ptr64,u32,u32,i32,i32,u32,u32,u32,i32);"
            "Binding(u32,u32,u32);ResourceBinding(i32,u32,u32,i32,u32,Binding);"
            "Interface(i32,ptr64,ptr64,u32,ptr64,u32,ptr64,u32,ptr64,u32);"
            "Resource(u32,u32,u32,ptr64);Builtin(i32,u32,Resource);Combined("
            "u32,"
            "u32,u32);ownership=originating-dll;headers=pinned-lock;";
        declarations += "Target(i32,ptr64,ptr64,i32,ptr64,u32,ptr64,u32,bool1,"
                        "ptr64,u32);Target80;";
#define EAWR_STAGE_SIGNATURE(ret, name, args)                                  \
    declarations += #ret " " #name #args ";";
        EAWR_STAGE_VKD3D_API(EAWR_STAGE_SIGNATURE)
        EAWR_STAGE_CROSS_API(EAWR_STAGE_SIGNATURE)
#undef EAWR_STAGE_SIGNATURE
        id.public_c_abi.manifest_sha256 = detail::digest(declarations);
        id.capabilities = {CapabilityToken::d3d_bytecode_to_spirv,
                           CapabilityToken::spirv_to_glsl_330,
                           CapabilityToken::typed_resource_reflection,
                           CapabilityToken::typed_interface_reflection,
                           CapabilityToken::combined_sampler_relationships};
        id.capability_manifest_sha256 = detail::digest(
            "eawr-stage-capabilities/"
            "1;tokens=1,2,3,4,5;variants=1,10|11,20|21,30,41;depth10=0,11=1;"
            "y20=0,"
            "21=1;binding-plan=1;result=1;glsl=330,es=0,vulkan=0,420pack=0;"
            "compile-"
            "api=1.0,strip-debug=0;spirv-target=vulkan1.0,main,noextensions,"
            "noparameters,noswizzles;success-only-code-ownership;reject-ray-"
            "types;"
            "no-profile-guarantee;whole-register-signature-association=1;named-uniform-members=2;member-collision=refuse");
    } catch (const detail::Failure &failure) {
        s.unavailable = detail::diagnostic(failure);
    }
}
CompilerSession::~CompilerSession() = default;
CompilerSession::CompilerSession(CompilerSession &&) noexcept = default;
CompilerSession &
CompilerSession::operator=(CompilerSession &&) noexcept = default;

std::optional<CompilerIdentity> compiler_identity(CompilerSession& session) {
    const auto& state = StageSessionAccess::get(session);
    if (state.unavailable) return {};
    return state.identity;
}

namespace detail {
std::string hex(const Digest &value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const auto byte : value) {
        const auto n = std::to_integer<unsigned>(byte);
        text += digits[n >> 4U];
        text += digits[n & 15U];
    }
    return text;
}
std::string bounded_text(const char *value, const StageLimits &limits,
                         DiagnosticPhase phase) {
    if (!value)
        return {};
    std::size_t size{};
    while (size < limits.string_bytes && value[size])
        ++size;
    require(size < limits.string_bytes, phase,
            DiagnosticReason::budget_exceeded,
            "shader metadata string budget exceeded");
    return {value, size};
}
} // namespace detail
} // namespace eawr::presentation::shader_runtime

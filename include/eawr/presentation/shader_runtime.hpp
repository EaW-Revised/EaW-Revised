#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace eawr::presentation::shader_runtime {
struct RuntimeFile {
    std::string_view path;
    std::string_view sha256;
    std::uint64_t size{};
};
enum class Status { disabled, missing_package, integrity_failure, unsupported_platform, missing_export, abi_mismatch, ready };
enum class Backend { vkd3d, spirv_cross };
using Function = void (*)();

// No constructor or renderer path loads DLLs. The future activation job must
// explicitly open a verified package. Its failure leaves hand adapters usable.
class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    [[nodiscard]] Status open(const std::filesystem::path& directory);
    // A deliberate reviewed replacement uses its own trusted identity table;
    // never populate this argument from an untrusted package manifest.
    [[nodiscard]] Status open(const std::filesystem::path& directory, std::span<const RuntimeFile> identity);
    void close() noexcept;
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] const std::string& diagnostic() const noexcept;
    // Public C exports only. Function pointers and library-owned storage are
    // valid until close/destruction. Free allocations via their originating DLL.
    [[nodiscard]] Function public_export(Backend backend, const char* name) const noexcept;
private:
    friend struct RuntimeTestAccess;
    [[nodiscard]] Status open_checked(const std::filesystem::path& directory, std::span<const RuntimeFile> identity,
                                      void (*before_load)(void*), void* context);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::presentation::shader_runtime

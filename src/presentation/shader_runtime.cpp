#include "eawr/presentation/shader_runtime.hpp"
#include "eawr/core/sha256.hpp"
#include "../../third_party/shader_runtime/identities.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eawr::presentation::shader_runtime {
#ifdef _WIN32
namespace {
struct ModuleTrust {
    std::mutex mutex;
    std::map<HMODULE, std::pair<std::string, std::size_t>> modules;
};
ModuleTrust& module_trust() {
    // Keep the tiny registry alive through static Runtime destruction. Entries
    // disappear with their module references; no function pointer is retained.
    static auto* trust = new ModuleTrust;
    return *trust;
}
}
#endif
struct Runtime::Impl {
    Status status{Status::disabled};
    std::string diagnostic;
#ifdef _WIN32
    std::vector<HANDLE> pinned_files;
    std::vector<HANDLE> pinned_directories;
    std::vector<HMODULE> libraries;
    HMODULE vkd3d{};
    HMODULE cross{};
    ~Impl() {
        for (auto it = libraries.rbegin(); it != libraries.rend(); ++it) {
            auto& trust = module_trust();
            {
                const std::lock_guard guard(trust.mutex);
                const auto found = trust.modules.find(*it);
                if (found != trust.modules.end() && --found->second.second == 0) trust.modules.erase(found);
            }
            FreeLibrary(*it);
        }
        for (const auto file : pinned_files) CloseHandle(file);
        for (auto it = pinned_directories.rbegin(); it != pinned_directories.rend(); ++it) CloseHandle(*it);
    }
#endif
};
Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;
void Runtime::close() noexcept { impl_.reset(); }
Status Runtime::status() const noexcept { return impl_ ? impl_->status : Status::disabled; }
const std::string& Runtime::diagnostic() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->diagnostic : empty;
}
Function Runtime::public_export(const Backend backend, const char* name) const noexcept {
#ifdef _WIN32
    if (!impl_ || !name) return nullptr;
    const std::string_view symbol(name);
    if (!symbol.starts_with(backend == Backend::vkd3d ? "vkd3d_shader_" : "spvc_")) return nullptr;
    const auto module = backend == Backend::vkd3d ? impl_->vkd3d : impl_->cross;
    if (!module) return nullptr;
    return std::bit_cast<Function>(GetProcAddress(module, name));
#else
    static_cast<void>(backend);
    static_cast<void>(name);
    return nullptr;
#endif
}
Status Runtime::open(const std::filesystem::path& directory) { return open(directory, pinned::files); }
Status Runtime::open(const std::filesystem::path& directory, const std::span<const RuntimeFile> identity) {
    return open_checked(directory, identity, nullptr, nullptr);
}
Status Runtime::open_checked(const std::filesystem::path& directory, const std::span<const RuntimeFile> identity,
                             void (*before_load)(void*), void* context) {
    impl_ = std::make_unique<Impl>();
    const auto fail = [&](const Status result, std::string reason) {
        // Unload every partially loaded module before reporting a refusal.
        impl_ = std::make_unique<Impl>();
        impl_->status = result;
        impl_->diagnostic = std::move(reason);
        return result;
    };
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return fail(Status::missing_package, "shader package unavailable");
#ifdef _WIN32
    const auto absolute = std::filesystem::absolute(directory, error).lexically_normal();
    if (error || identity.empty()) return fail(Status::integrity_failure, "invalid shader package identity");
    // Qualify local volumes only. Pin every directory BEFORE opening a child:
    // deny write/delete so neither junction contents nor directory names can
    // change while hashing/loading. A final-file lock alone cannot do this.
    const auto drive = absolute.root_name().wstring();
    if (drive.size() != 2 || drive[1] != L':')
        return fail(Status::integrity_failure, "shader package needs a local drive path");
    std::set<std::filesystem::path> directories;
    std::string directory_problem;
    const auto pin_directory = [&](const std::filesystem::path& path) {
        if (directories.contains(path)) return true;
        const auto utf8 = path.generic_u8string();
        const std::string display(utf8.begin(), utf8.end());
        directory_problem = "shader translation unavailable: package directory busy or inaccessible at " + display;
        // Attribute-only opens are exempt from some Windows sharing checks;
        // GENERIC_READ is required to exclude reparse-data writers as well.
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        impl_->pinned_directories.push_back(handle);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            directory_problem = "shader translation unavailable: package path contains a junction/symlink at " + display;
            return false;
        }
        directories.insert(path);
        return true;
    };
    auto ancestor = absolute.root_path();
    if (!pin_directory(ancestor)) return fail(Status::integrity_failure, directory_problem);
    for (const auto& part : absolute.relative_path()) {
        ancestor /= part;
        if (!pin_directory(ancestor)) return fail(Status::integrity_failure, directory_problem);
    }
    std::wstring final_path(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(impl_->pinned_directories.back(), final_path.data(),
                                                 static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= final_path.size()) return fail(Status::integrity_failure, "shader final directory unavailable");
    final_path.resize(length);
    // Use the final DOS spelling accepted by the Windows DLL loader. The
    // directory chain stays locked through load; loaded file IDs are checked
    // against the already-verified handles before accepting module ownership.
    const std::filesystem::path root{final_path};
    bool has_vkd3d{}, has_cross{};
    for (const auto& item : identity) {
        const std::filesystem::path relative{item.path};
        if (relative.is_absolute() || item.path.find(':') != std::string_view::npos || item.path.find('\\') != std::string_view::npos
            || std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".." || part == "."; }))
            return fail(Status::integrity_failure, "invalid shader package path");
        auto parent = root;
        for (const auto& part : relative.parent_path()) {
            parent /= part;
            if (!pin_directory(parent)) return fail(Status::integrity_failure, directory_problem);
        }
        auto path = root / relative;
        // Logical identities keep '/'. Extended Win32 paths require native
        // separators, including the separators inside a multi-part member.
        path.make_preferred();
        // Deny replacement/deletion through the hash-to-load interval.
        // LoadLibrary can still read the file. No CWD/PATH DLL lookup.
        const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const auto windows_error = GetLastError();
            return fail(Status::integrity_failure, "shader package file missing or busy: " +
                std::string(item.path) + " (Windows error " + std::to_string(windows_error) + ")");
        }
        impl_->pinned_files.push_back(file);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || item.size > 64ULL * 1024ULL * 1024ULL
            || ((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow) != item.size)
            return fail(Status::integrity_failure, "shader package file type or size mismatch");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(item.size));
        DWORD count{};
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size()
            || core::sha256_hex(bytes) != item.sha256)
            return fail(Status::integrity_failure, "shader package hash mismatch");
        has_vkd3d |= item.path == "libvkd3d-shader-1.dll";
        has_cross |= item.path == "libspirv-cross-c-shared.dll";
    }
    if (!has_vkd3d || !has_cross) return fail(Status::integrity_failure, "shader package compiler identity missing");
    if (before_load) before_load(context);
    // Dependencies precede compilers in the reviewed identity list. Search only
    // the verified package and Windows system directory, never PATH or CWD.
    for (std::size_t file_index = 0; file_index < identity.size(); ++file_index) {
        const auto& item = identity[file_index];
        if (!item.path.ends_with(".dll")) continue;
        auto path = root / std::filesystem::path{item.path};
        path.make_preferred();
        if (const auto existing = GetModuleHandleW(std::filesystem::path{item.path}.c_str())) {
            // Path equality alone cannot identify an already mapped image if
            // its old filename was renamed/replaced. Reuse only images this
            // loader previously verified, with the very same artifact hash.
            bool trusted{};
            {
                auto& trust = module_trust();
                const std::lock_guard guard(trust.mutex);
                const auto found = trust.modules.find(existing);
                trusted = found != trust.modules.end() && found->second.first == item.sha256;
            }
            if (!trusted) return fail(Status::abi_mismatch, "shader DLL already loaded outside verified package ownership");
            std::wstring loaded(32768, L'\0');
            const auto count = GetModuleFileNameW(existing, loaded.data(), static_cast<DWORD>(loaded.size()));
            if (!count || count >= loaded.size()) return fail(Status::abi_mismatch, "shader DLL module identity unavailable");
            loaded.resize(count);
            if (!std::filesystem::equivalent(path, loaded, error) || error)
                return fail(Status::abi_mismatch, "different shader DLL with same name already loaded");
        }
        const auto module = LoadLibraryExW(path.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            const auto windows_error = GetLastError();
            return fail(Status::missing_export, "shader package DLL could not load (Windows error " +
                         std::to_string(windows_error) + ")");
        }
        struct PendingModule {
            HMODULE handle;
            ~PendingModule() { if (handle) FreeLibrary(handle); }
        } pending{module};
        std::wstring loaded(32768, L'\0');
        const auto loaded_length = GetModuleFileNameW(module, loaded.data(), static_cast<DWORD>(loaded.size()));
        if (!loaded_length || loaded_length >= loaded.size())
            return fail(Status::integrity_failure, "loaded shader DLL file identity unavailable");
        loaded.resize(loaded_length);
        const auto loaded_file = CreateFileW(loaded.c_str(), FILE_READ_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (loaded_file == INVALID_HANDLE_VALUE)
            return fail(Status::integrity_failure, "loaded shader DLL file could not be identified");
        BY_HANDLE_FILE_INFORMATION expected{}, observed{};
        const bool matching_file = GetFileInformationByHandle(impl_->pinned_files[file_index], &expected)
            && GetFileInformationByHandle(loaded_file, &observed)
            && !(observed.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            && expected.dwVolumeSerialNumber == observed.dwVolumeSerialNumber
            && expected.nFileIndexHigh == observed.nFileIndexHigh
            && expected.nFileIndexLow == observed.nFileIndexLow;
        CloseHandle(loaded_file);
        if (!matching_file) return fail(Status::integrity_failure, "loaded shader DLL differs from the verified file");
        {
            auto& trust = module_trust();
            const std::lock_guard guard(trust.mutex);
            const auto [entry, inserted] = trust.modules.try_emplace(module, std::string(item.sha256), std::size_t{0});
            try { impl_->libraries.push_back(module); }
            catch (...) { if (inserted) trust.modules.erase(entry); throw; }
            ++entry->second.second;
        }
        pending.handle = nullptr;
        if (item.path == "libvkd3d-shader-1.dll") impl_->vkd3d = module;
        if (item.path == "libspirv-cross-c-shared.dll") impl_->cross = module;
    }
    // Image sections now hold the verified DLL objects. Release our pathname
    // guards immediately: keeping install ancestors locked during play would
    // unnecessarily block folder management by the player and other software.
    for (const auto file : impl_->pinned_files) CloseHandle(file);
    impl_->pinned_files.clear();
    for (auto it = impl_->pinned_directories.rbegin(); it != impl_->pinned_directories.rend(); ++it) CloseHandle(*it);
    impl_->pinned_directories.clear();
    constexpr const char* vkd3d_exports[]{"vkd3d_shader_get_version", "vkd3d_shader_compile", "vkd3d_shader_free_shader_code",
                                         "vkd3d_shader_free_messages", "vkd3d_shader_preprocess"};
    constexpr const char* cross_exports[]{"spvc_get_version", "spvc_context_create", "spvc_context_destroy",
        "spvc_context_release_allocations", "spvc_context_get_last_error_string", "spvc_context_parse_spirv",
        "spvc_context_create_compiler", "spvc_compiler_create_compiler_options", "spvc_compiler_options_set_uint",
        "spvc_compiler_install_compiler_options", "spvc_compiler_compile", "spvc_compiler_create_shader_resources",
        "spvc_resources_get_resource_list_for_type"};
    for (const auto* name : vkd3d_exports)
        if (!public_export(Backend::vkd3d, name)) return fail(Status::missing_export, "vkd3d public C export missing");
    for (const auto* name : cross_exports)
        if (!public_export(Backend::spirv_cross, name)) return fail(Status::missing_export, "SPIRV-Cross public C export missing");
    using Vkd3dVersion = const char* (*)(unsigned int*, unsigned int*);
    using CrossVersion = void (*)(unsigned int*, unsigned int*, unsigned int*);
    unsigned int major{}, minor{}, patch{};
    const auto version = std::bit_cast<Vkd3dVersion>(public_export(Backend::vkd3d, "vkd3d_shader_get_version"));
    if (!version(&major, &minor) || major != 2 || minor != 1) return fail(Status::abi_mismatch, "vkd3d version mismatch");
    std::bit_cast<CrossVersion>(public_export(Backend::spirv_cross, "spvc_get_version"))(&major, &minor, &patch);
    if (major != 0 || minor != 61 || patch != 0) return fail(Status::abi_mismatch, "SPIRV-Cross C ABI mismatch");
    impl_->status = Status::ready;
    return Status::ready;
#else
    static_cast<void>(identity);
    static_cast<void>(before_load);
    static_cast<void>(context);
    return fail(Status::unsupported_platform, "shader package platform not qualified");
#endif
}
} // namespace eawr::presentation::shader_runtime

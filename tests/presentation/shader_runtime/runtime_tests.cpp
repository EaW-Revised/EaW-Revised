#include "eawr/presentation/shader_runtime.hpp"
#include "eawr/core/sha256.hpp"
#include <array>
#include <chrono>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace eawr::presentation::shader_runtime {
struct RuntimeTestAccess {
    static Status open(Runtime& runtime, const std::filesystem::path& directory, std::span<const RuntimeFile> identity,
                       void (*before_load)(void*), void* context) {
        return runtime.open_checked(directory, identity, before_load, context);
    }
};
}

using namespace eawr::presentation::shader_runtime;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
#ifdef _WIN32
bool make_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto substitute = L"\\??\\" + std::filesystem::absolute(target).wstring();
    const auto print = std::filesystem::absolute(target).wstring();
    struct Header {
        std::uint32_t tag;
        std::uint16_t length, reserved, substitute_offset, substitute_length, print_offset, print_length;
    };
    static_assert(sizeof(Header) == 16);
    const auto path_bytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    if (path_bytes > 16000) { CloseHandle(handle); return false; }
    const Header header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<std::uint16_t>(path_bytes + 8), 0, 0,
        static_cast<std::uint16_t>(substitute.size() * sizeof(wchar_t)),
        static_cast<std::uint16_t>((substitute.size() + 1) * sizeof(wchar_t)),
        static_cast<std::uint16_t>(print.size() * sizeof(wchar_t))};
    std::vector<std::byte> buffer(sizeof(header) + path_bytes);
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.data(), substitute.size() * sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(header) + header.print_offset, print.data(), print.size() * sizeof(wchar_t));
    DWORD returned{};
    const auto result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                                       nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    return result != FALSE;
}
#endif
}
int main(int argc, char** argv) {
    try {
        Runtime runtime;
        require(runtime.status() == Status::disabled, "construction must not activate translation");
        require(!runtime.public_export(Backend::vkd3d, "vkd3d_shader_compile"), "disabled export");
        std::filesystem::path real_package;
        if (argc == 3 && std::string_view(argv[1]) == "--package") real_package = argv[2];
#ifdef _WIN32
        if (argc == 2 && std::string_view(argv[1]) == "--package-env") {
            std::wstring value(32768, L'\0');
            const auto length = GetEnvironmentVariableW(L"EAWR_SHADER_RUNTIME_TEST_PACKAGE", value.data(), static_cast<DWORD>(value.size()));
            if (!length) { std::cout << "optional offline shader package absent\n"; return 77; }
            require(length < value.size(), "offline package environment path too long");
            value.resize(length);
            real_package = value;
        }
#endif
        if (!real_package.empty()) {
            const auto loaded = runtime.open(real_package);
            require(loaded == Status::ready, runtime.diagnostic().c_str());
            Runtime second;
            require(second.open(real_package) == Status::ready, "second package owner");
#ifdef _WIN32
            const auto directory = CreateFileW(real_package.c_str(), GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            require(directory != INVALID_HANDLE_VALUE, "ancestor write/delete guards released immediately after load");
            CloseHandle(directory);
#endif
            runtime.close();
            using Version = const char* (*)(unsigned int*, unsigned int*);
            const auto version = std::bit_cast<Version>(second.public_export(Backend::vkd3d, "vkd3d_shader_get_version"));
            unsigned int major{}, minor{};
            require(version && version(&major, &minor) && major == 2 && minor == 1, "module lifetime across owners");
            require(!second.public_export(Backend::vkd3d, "private_symbol"), "only public C exports");
            std::cout << "real package load and independent module lifetimes passed\n";
            return 0;
        }
        const auto root = std::filesystem::temp_directory_path() / ("eawr-shader-runtime-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
        require(runtime.open(root / "absent") == Status::missing_package, "missing optional package");
        require(runtime.open(root) != Status::ready, "empty optional package");
#ifdef _WIN32
        if (argc == 4) {
            const std::filesystem::path fake(argv[1]);
            std::ifstream input(fake, std::ios::binary);
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
            const auto hash = eawr::core::sha256_hex(bytes);
            std::filesystem::copy_file(fake, root / "libvkd3d-shader-1.dll");
            std::filesystem::copy_file(fake, root / "libspirv-cross-c-shared.dll");
            const std::array<RuntimeFile, 2> identity{{
                {"libvkd3d-shader-1.dll", hash, bytes.size()}, {"libspirv-cross-c-shared.dll", hash, bytes.size()}}};
            std::filesystem::create_directories(root / "nested");
            std::filesystem::copy_file(fake, root / "nested" / "member.bin");
            const std::array<RuntimeFile, 3> nested_identity{{identity[0], identity[1], {"nested/member.bin", hash, bytes.size()}}};
            const auto nested_status = runtime.open(root, nested_identity);
            require(nested_status == Status::abi_mismatch, runtime.diagnostic().c_str());
            { std::ofstream corrupt(root / "nested" / "member.bin", std::ios::binary | std::ios::app); corrupt.put('x'); }
            require(runtime.open(root, nested_identity) == Status::integrity_failure, "replaced nested member refused");
            const auto mismatch_status = runtime.open(root, identity);
            require(mismatch_status == Status::abi_mismatch, runtime.diagnostic().c_str());
            require(!runtime.public_export(Backend::vkd3d, "vkd3d_shader_compile"), "refusal releases libraries");
            for (int index = 2; index <= 3; ++index) {
                std::filesystem::copy_file(argv[index], root / "libvkd3d-shader-1.dll", std::filesystem::copy_options::overwrite_existing);
                std::ifstream other(argv[index], std::ios::binary);
                const std::vector<std::uint8_t> other_bytes{std::istreambuf_iterator<char>(other), {}};
                const auto other_hash = eawr::core::sha256_hex(other_bytes);
                const std::array<RuntimeFile, 2> changed{{
                    {"libvkd3d-shader-1.dll", other_hash, other_bytes.size()},
                    {"libspirv-cross-c-shared.dll", hash, bytes.size()}}};
                require(runtime.open(root, changed) == (index == 2 ? Status::abi_mismatch : Status::missing_export),
                        "vkd3d mismatch/missing public export refused");
            }
            std::filesystem::copy_file(fake, root / "libvkd3d-shader-1.dll", std::filesystem::copy_options::overwrite_existing);
            { std::ofstream corrupt(root / "libspirv-cross-c-shared.dll", std::ios::binary | std::ios::app); corrupt.put('x'); }
            require(runtime.open(root, identity) == Status::integrity_failure, "replaced DLL refused before load");
            std::filesystem::remove(root / "libvkd3d-shader-1.dll");
            require(runtime.open(root, identity) == Status::integrity_failure, "missing dependency refused");
            const std::array<RuntimeFile, 1> traversal{{{"../outside.dll", hash, 0}}};
            require(runtime.open(root, traversal) == Status::integrity_failure, "unsafe identity refused");
            const auto chain = root / "chain";
            const auto active = chain / "active";
            const auto bad = root / "bad";
            std::filesystem::create_directories(active);
            std::filesystem::create_directories(bad / "active");
            std::filesystem::copy_file(fake, active / "libvkd3d-shader-1.dll");
            std::filesystem::copy_file(fake, active / "libspirv-cross-c-shared.dll");
            { std::ofstream inert(bad / "active" / "libvkd3d-shader-1.dll"); inert << "inert replacement, never executable"; }
            require(make_junction(root / "junction", chain), "create safe ancestor junction fixture");
            require(runtime.open(root / "junction" / "active", identity) == Status::integrity_failure,
                    "pre-existing ancestor junction refused before loading");
            require(runtime.diagnostic().find("junction/symlink") != std::string::npos, "clear linked-package fallback diagnostic");
            require(RemoveDirectoryW((root / "junction").c_str()) != FALSE, "remove own junction fixture");
            struct Swap { std::filesystem::path chain, renamed, replacement; bool attempted{}, moved{}, junction{}, writable{}; };
            Swap swap{chain, root / "chain-old", bad};
            const auto result = RuntimeTestAccess::open(runtime, active, identity, +[](void* context) {
                auto& state = *static_cast<Swap*>(context);
                state.attempted = true;
                const auto writer = CreateFileW(state.chain.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                state.writable = writer != INVALID_HANDLE_VALUE;
                if (state.writable) CloseHandle(writer);
                state.moved = MoveFileExW(state.chain.c_str(), state.renamed.c_str(), 0) != FALSE;
                if (state.moved) state.junction = make_junction(state.chain, state.replacement);
            }, &swap);
            require(swap.attempted && !swap.moved && !swap.junction && !swap.writable,
                    "ancestor cannot be renamed or opened for reparse writes at hash/load barrier");
            require(result == Status::abi_mismatch, "verified fake reached ABI check after blocked junction swap");
            const auto foreign = LoadLibraryExW((active / "libvkd3d-shader-1.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            require(foreign != nullptr, "load independent invented module owner");
            const auto refused = runtime.open(active, identity);
            FreeLibrary(foreign);
            require(refused == Status::abi_mismatch && runtime.diagnostic().find("outside verified package ownership") != std::string::npos,
                    "untracked existing image cannot be trusted by pathname equality");
        }
#endif
        static_cast<void>(argc);
        static_cast<void>(argv);
        runtime.close();
        require(runtime.status() == Status::disabled, "close disables route");
        std::cout << "optional package, ownership and refusal controls passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

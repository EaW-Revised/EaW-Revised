// Opt-in experiment. Public ABI headers remain external; no import library or
// vkd3d implementation is linked into this executable.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vkd3d_shader.h>
#include "synthetic.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Library {
    HMODULE handle;
    explicit Library(const std::filesystem::path& path)
        : handle(LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr,
              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
        if (!handle) throw std::runtime_error("LoadLibraryExW failed: " + std::to_string(GetLastError()));
    }
    ~Library() { FreeLibrary(handle); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    template<class T> T symbol(const char* name) const {
        const auto pointer = GetProcAddress(handle, name);
        if (!pointer) throw std::runtime_error(std::string("Missing export: ") + name);
        // Windows documents FARPROC conversion for dynamically loaded functions.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4191)
#endif
        return reinterpret_cast<T>(pointer);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }
};
void write(const std::filesystem::path& path, const void* data, const std::size_t size) {
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file) throw std::runtime_error("Cannot write output");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: eawr_shader_bridge_gate DLL synthetic|BYTECODE OUTPUT_DIR");
        const Library library{std::filesystem::path(argv[1])};
        const auto compile = library.symbol<PFN_vkd3d_shader_compile>("vkd3d_shader_compile");
        const auto release = library.symbol<PFN_vkd3d_shader_free_shader_code>("vkd3d_shader_free_shader_code");
        const auto messages_free = library.symbol<PFN_vkd3d_shader_free_messages>("vkd3d_shader_free_messages");
        const auto version = library.symbol<PFN_vkd3d_shader_get_version>("vkd3d_shader_get_version");
        unsigned int major{}, minor{};
        std::cout << "dynamic vkd3d-shader " << version(&major, &minor) << '\n';
        const std::filesystem::path output(argv[3]);
        std::filesystem::create_directories(output);
        const auto translate = [&](const std::string& name, const void* bytes, std::size_t size) {
            vkd3d_shader_compile_info info{};
            info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
            info.source = {bytes, size};
            info.source_type = VKD3D_SHADER_SOURCE_D3D_BYTECODE;
            info.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY;
            info.log_level = VKD3D_SHADER_LOG_INFO;
            info.source_name = name.c_str();
            vkd3d_shader_code result{};
            char* messages = nullptr;
            const int status = compile(&info, &result, &messages);
            if (messages) { std::cout << messages; messages_free(messages); }
            std::cout << name << " status=" << status << " bytes=" << result.size << '\n';
            if (status < 0) { release(&result); throw std::runtime_error("Translation failed"); }
            write(output / (name + ".spv"), result.code, result.size);
            release(&result);
        };
        if (std::string(argv[2]) == "synthetic") {
            using namespace shader_bridge_gate;
            translate("synthetic-vs", vertex.data(), sizeof(vertex));
            translate("synthetic-ps2", pixel2.data(), sizeof(pixel2));
            translate("synthetic-ps1", pixel1.data(), sizeof(pixel1));
        } else {
            std::ifstream input(argv[2], std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open input bytecode");
            const std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
            translate("program", bytes.data(), bytes.size());
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

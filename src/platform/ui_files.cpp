#include "eawr/platform/ui_files.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace eawr::platform {

std::filesystem::path path_from_utf8(const std::string_view text) {
    return Path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string utf8_from_path(const Path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::optional<std::string> read_text_file(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

bool write_text_file_atomic(const std::filesystem::path& file, const std::string& text, std::string& error) {
    std::error_code code;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), code);
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "cannot write " + path_utf8(temporary);
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = "cannot write " + path_utf8(temporary);
            return false;
        }
    }
    // rename replaces an existing file atomically on every platform; never delete the old file first.
    std::filesystem::rename(temporary, file, code);
    if (code) {
        error = "cannot replace " + path_utf8(file) + ": " + code.message();
        std::filesystem::remove(temporary, code);
        return false;
    }
    return true;
}

} // namespace eawr::platform

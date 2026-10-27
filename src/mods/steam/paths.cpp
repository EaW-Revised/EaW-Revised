#include "eawr/mods/steam/paths.hpp"

#include <exception>
#include <system_error>

namespace eawr::mods::steam {

namespace {

bool valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        if (lead >= 0x80) {
            if (lead >= 0xC2 && lead <= 0xDF) length = 2;
            else if (lead >= 0xE0 && lead <= 0xEF) length = 3;
            else if (lead >= 0xF0 && lead <= 0xF4) length = 4;
            else return false;
            if (index + length > text.size()) return false;
            for (std::size_t offset = 1; offset < length; ++offset) {
                if ((static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80) return false;
            }
            // Overlong, surrogate and out-of-range encodings.
            const auto second = static_cast<unsigned char>(text[index + 1]);
            if (lead == 0xE0 && second < 0xA0) return false;
            if (lead == 0xED && second >= 0xA0) return false;
            if (lead == 0xF0 && second < 0x90) return false;
            if (lead == 0xF4 && second >= 0x90) return false;
        }
        index += length;
    }
    return true;
}

std::string normalised_key(const fs::path& path) {
    std::error_code error;
    fs::path value = fs::weakly_canonical(path, error);
    if (error) value = path.lexically_normal();
    std::string text = path_to_utf8(value);
    while (text.size() > 1 && (text.back() == '\\' || text.back() == '/')) text.pop_back();
    for (char& character : text) {
        if (character == '/') character = '\\';
    }
    return lower_ascii(text);
}

} // namespace

fs::path path_from_utf8(std::string_view text) {
    if (!valid_utf8(text)) return {};
    try {
        const std::u8string converted(reinterpret_cast<const char8_t*>(text.data()), text.size());
        return fs::path(converted);
    } catch (const std::exception&) {
        return {};
    }
}

std::string path_to_utf8(const fs::path& path) {
    try {
        const std::u8string text = path.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    } catch (const std::exception&) {
        return {};
    }
}

std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return result;
}

std::wstring widen_ascii(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (const char character : text) result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    return result;
}

std::optional<fs::path> find_child_ci(const fs::path& directory, std::string_view name) {
    std::error_code error;
    const fs::path exact = directory / path_from_utf8(name);
    if (fs::exists(exact, error)) return exact;
    error.clear();
    const std::string wanted = lower_ascii(name);
    for (fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (lower_ascii(path_to_utf8(iterator->path().filename())) == wanted) return iterator->path();
    }
    return std::nullopt;
}

bool same_location(const fs::path& left, const fs::path& right) {
    std::error_code error;
    if (fs::exists(left, error) && fs::exists(right, error)) {
        const bool same = fs::equivalent(left, right, error);
        if (!error) return same;
    }
    return normalised_key(left) == normalised_key(right);
}

} // namespace eawr::mods::steam

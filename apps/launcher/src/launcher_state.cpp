#include "launcher_state.hpp"

#include "text_formats.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace eawr::launcher {

namespace {
constexpr const char* kSchema = "eawr-launcher/1";
}

fs::path default_user_data_dir() {
#ifdef _WIN32
    // Godot's get_data_path() honors APPDATA, including an isolated test profile.
    const DWORD size = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (size > 1) {
        std::wstring value(size, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", value.data(), size);
        if (length > 0 && length < size) {
            value.resize(length);
            return fs::path(value) / "Godot" / "app_userdata" / path_from_utf8(kViewerProjectName);
        }
    }
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &roaming)) || roaming == nullptr) {
        if (roaming != nullptr) CoTaskMemFree(roaming);
        return {};
    }
    fs::path path(roaming);
    CoTaskMemFree(roaming);
    return path / "Godot" / "app_userdata" / path_from_utf8(kViewerProjectName);
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && *data != '\0')
        return path_from_utf8(data) / "godot" / "app_userdata" / path_from_utf8(kViewerProjectName);
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return path_from_utf8(home) / ".local" / "share" / "godot" / "app_userdata" / path_from_utf8(kViewerProjectName);
    return {};
#endif
}

fs::path state_file(const fs::path& user_data) {
    return user_data / "launcher" / "launcher.json";
}

LauncherState load_state(const fs::path& user_data) {
    LauncherState state;
    std::ifstream stream(state_file(user_data), std::ios::binary);
    if (!stream) return state;
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.size() > 64 * 1024) return state;
    const std::optional<JsonValue> json = parse_json(text);
    if (!json || json->kind != JsonValue::Kind::object) return state;
    if (const std::string* root = json->string_member("game_root"); root != nullptr && !root->empty()) {
        fs::path path = path_from_utf8(*root);
        if (!path.empty()) state.game_root = std::move(path);
    }
    if (const auto* profile = json->string_member("last_profile")) state.last_profile = *profile;
    return state;
}

bool save_state(const fs::path& user_data, const LauncherState& state, std::string& error) {
    std::error_code code;
    const fs::path target = state_file(user_data);
    fs::create_directories(target.parent_path(), code);
    if (code) {
        error = "cannot create " + path_to_utf8(target.parent_path()) + ": " + code.message();
        return false;
    }
    std::string text = "{\n  \"schema\": " + json_quote(kSchema);
    if (state.game_root) text += ",\n  \"game_root\": " + json_quote(path_to_utf8(*state.game_root));
    if (state.last_profile) text += ",\n  \"last_profile\": " + json_quote(*state.last_profile);
    text += "\n}\n";
    fs::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot write " + path_to_utf8(temporary);
            return false;
        }
        stream << text;
        stream.flush();
        if (!stream) {
            error = "cannot write " + path_to_utf8(temporary);
            return false;
        }
    }
    fs::rename(temporary, target, code);   // replaces an existing file on Windows
    if (code) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        error = "cannot replace " + path_to_utf8(target) + ": " + code.message();
        return false;
    }
    return true;
}

} // namespace eawr::launcher

#include "launcher_state.hpp"

#include "text_formats.hpp"

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
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &roaming)) || roaming == nullptr) {
        if (roaming != nullptr) CoTaskMemFree(roaming);
        return {};
    }
    fs::path path(roaming);
    CoTaskMemFree(roaming);
    return path / "Godot" / "app_userdata" / path_from_utf8(kViewerProjectName);
#else
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

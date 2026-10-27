// Windows-only location adapter. The format/resolver use the injected ProfileSteamReader interface.
#include "eawr/mods/profile.hpp"
#include "profile_io.hpp"
#include "eawr/mods/steam/library_folders.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace eawr::mods {
bool detail::replace_profile_file(const steam::fs::path& from, const steam::fs::path& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
namespace {
class WindowsSteamReader final : public ProfileSteamReader {
public:
    std::vector<steam::fs::path> roots() const override {
        std::vector<steam::fs::path> result;
        const auto read = [&](HKEY hive, const wchar_t* name, DWORD flags) {
            wchar_t path[32768]{};
            DWORD bytes = sizeof(path);
            if (RegGetValueW(hive, L"Software\\Valve\\Steam", name, RRF_RT_REG_SZ | flags,
                nullptr, path, &bytes) == ERROR_SUCCESS) steam::add_library(result, steam::fs::path(path));
        };
        read(HKEY_CURRENT_USER, L"SteamPath", 0);
        read(HKEY_LOCAL_MACHINE, L"InstallPath", RRF_SUBKEY_WOW6464KEY);
        read(HKEY_LOCAL_MACHINE, L"InstallPath", RRF_SUBKEY_WOW6432KEY);
        return result;
    }
};
}
const ProfileSteamReader& native_profile_steam_reader() {
    static const WindowsSteamReader reader;
    return reader;
}
} // namespace eawr::mods

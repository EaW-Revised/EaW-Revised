#include "win_platform.hpp"

#include <windows.h>

#include <wchar.h>

namespace eawr::launcher {

std::string utf8_from_wide(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring wide_from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

namespace {

class WindowsRegistry final : public RegistryReader {
public:
    std::optional<std::string> read_string(RegistryHive hive, RegistryView view, const std::string& key,
                                           const std::string& value) const override {
        HKEY handle = nullptr;
        if (RegOpenKeyExW(root(hive), wide_from_utf8(key).c_str(), 0, KEY_READ | access(view), &handle) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        std::optional<std::string> result;
        const std::wstring name = wide_from_utf8(value);
        DWORD bytes = 0;
        const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
        if (RegGetValueW(handle, nullptr, name.c_str(), flags, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes <= 64 * 1024) {
            std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
            DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
            if (RegGetValueW(handle, nullptr, name.c_str(), flags, nullptr, buffer.data(), &size) == ERROR_SUCCESS) {
                buffer.resize(wcsnlen(buffer.c_str(), buffer.size()));
                result = utf8_from_wide(buffer);
            }
        }
        RegCloseKey(handle);
        return result;
    }

    std::vector<std::string> subkeys(RegistryHive hive, RegistryView view, const std::string& key) const override {
        std::vector<std::string> names;
        HKEY handle = nullptr;
        if (RegOpenKeyExW(root(hive), wide_from_utf8(key).c_str(), 0, KEY_READ | access(view), &handle) != ERROR_SUCCESS) {
            return names;
        }
        for (DWORD index = 0; index < 4096; ++index) {
            wchar_t buffer[256];
            DWORD length = 256;
            const LONG status = RegEnumKeyExW(handle, index, buffer, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status == ERROR_SUCCESS) names.push_back(utf8_from_wide(std::wstring(buffer, length)));
        }
        RegCloseKey(handle);
        return names;
    }

private:
    static HKEY root(RegistryHive hive) { return hive == RegistryHive::current_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
    static REGSAM access(RegistryView view) { return view == RegistryView::view32 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY; }
};

} // namespace

std::unique_ptr<RegistryReader> make_windows_registry() {
    return std::make_unique<WindowsRegistry>();
}

} // namespace eawr::launcher

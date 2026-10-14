#include "eawr/platform/saved_games.hpp"

#include "save_repository.hpp"

namespace eawr::platform {
namespace {
// The session API uses UTF-8; preserve every native path character at this seam.
std::string utf8_directory(const std::filesystem::path& directory) {
    const auto encoded = directory.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
}
} // namespace

core::Result<std::vector<LiveSaveSlot>> list_saved_games(const std::filesystem::path& directory) {
    return save_detail::list(utf8_directory(directory));
}

core::Result<void> delete_saved_game(const std::filesystem::path& directory, const std::string_view name) {
    return save_detail::erase(utf8_directory(directory), name);
}

bool valid_saved_game_name(const std::string_view name) noexcept { return save_detail::valid_name(name); }

} // namespace eawr::platform

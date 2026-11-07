#include "eawr/platform/saved_games.hpp"

#include "save_repository.hpp"

#include "eawr/checkpoint/checkpoint.hpp"

#include <algorithm>

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

core::Result<void> write_battle_save(const std::filesystem::path& directory, const std::string_view name,
    const std::span<const std::uint8_t> checkpoint, const std::span<const std::uint8_t> hints, const bool overwrite) {
    if (!save_detail::valid_name(name)) {
        core::Diagnostic error; error.code = "EAWR-SAVE-0101"; error.message = "Choose a valid save name and directory.";
        return core::Result<void>::failure(std::move(error));
    }
    auto parsed = checkpoint::parse_checkpoint(checkpoint);
    if (!parsed) return core::Result<void>::failure(parsed.error());
    auto& sections = parsed.value().sections;
    const auto hints_id = static_cast<std::uint16_t>(checkpoint::SectionId::presentation_hints);
    std::erase_if(sections, [&](const checkpoint::Section& section) { return section.id == hints_id; });
    if (!hints.empty()) sections.push_back(checkpoint::Section{hints_id, 1, false, {hints.begin(), hints.end()}});
    auto bytes = checkpoint::write_checkpoint(parsed.value());
    if (!bytes) return core::Result<void>::failure(bytes.error());
    return save_detail::write(utf8_directory(directory), name, bytes.value(), overwrite);
}

core::Result<void> delete_saved_game(const std::filesystem::path& directory, const std::string_view name) {
    return save_detail::erase(utf8_directory(directory), name);
}

bool valid_saved_game_name(const std::string_view name) noexcept { return save_detail::valid_name(name); }

} // namespace eawr::platform

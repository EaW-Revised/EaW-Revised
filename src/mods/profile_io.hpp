#pragma once
// Private platform primitive; the public write API remains save_profile(user_data, profile).
#include "eawr/mods/steam/paths.hpp"
namespace eawr::mods::detail {
[[nodiscard]] bool replace_profile_file(const steam::fs::path& from, const steam::fs::path& to);
}

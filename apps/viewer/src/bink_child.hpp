#pragma once
#include "ui/bink_movie.hpp"
#include <filesystem>
namespace eawr::presentation::godot_backend {
[[nodiscard]] std::unique_ptr<BinkDecoder> open_bink_child(const std::filesystem::path& helper,
    const std::filesystem::path& game_root, std::vector<std::byte> source,
    std::shared_ptr<std::atomic_bool> cancelled);
}

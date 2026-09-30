#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
[[nodiscard]] std::vector<std::byte> read(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const std::streamsize size = input ? static_cast<std::streamsize>(input.tellg()) : -1;
    if (size < 0) return {};
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!result.empty()) input.read(reinterpret_cast<char*>(result.data()), size);
    return input || result.empty() ? result : std::vector<std::byte>{};
}
[[nodiscard]] eawr::assets::Source source(const char* path, const std::size_t size) {
    return {.logical_path = path, .stored_size = size};
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) { std::cerr << "usage: animation_corpus_playback <model.alo> [animation.ala]\n"; return 2; }
    const auto model_bytes = read(argv[1]); const auto animation_bytes = argc == 3 ? read(argv[2]) : std::vector<std::byte>{};
    const auto model = eawr::assets::load_model(model_bytes, source(argv[1], model_bytes.size()));
    if (!model) { std::cerr << model.error().message << '\n'; return 1; }
    if (argc == 2) {
        const auto player = eawr::presentation::animation::Player::create(model.value());
        const auto pose = player ? player.value().sample({}) : eawr::core::Result<eawr::presentation::animation::Pose>::failure(player.error());
        if (!pose) { std::cerr << pose.error().message << '\n'; return 1; }
        std::cout << "model_bones=" << model.value().bones.size() << " bind_playback=passed\n";
        return 0;
    }
    const auto animation = eawr::assets::load_animation(animation_bytes, source(argv[2], animation_bytes.size()));
    if (!animation) { std::cerr << animation.error().message << '\n'; return 1; }
    const auto player = eawr::presentation::animation::Player::create(model.value(), &animation.value());
    if (!player) { std::cerr << player.error().message << '\n'; return 1; }
    for (const float time : {0.0F, animation.value().duration_seconds * 0.5F, animation.value().duration_seconds}) {
        const auto pose = player.value().sample({time, eawr::presentation::animation::PlaybackMode::loop, 0.0F});
        if (!pose) { std::cerr << pose.error().message << '\n'; return 1; }
    }
    std::cout << "model_bones=" << model.value().bones.size() << " tracks=" << animation.value().tracks.size()
              << " frames=" << animation.value().stored_frame_count << " playback=passed\n";
}

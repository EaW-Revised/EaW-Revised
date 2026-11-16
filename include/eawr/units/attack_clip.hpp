#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace eawr::units {

// WSPL-08, #6554: the attack clip a SUPER_LASER selects with Attack_Anim_Sub_Index, `<model directory><model stem>_attack_NN.ala`
// with NN the index in two digits. The one resolution the simulation (the fire gate's clip) and the viewer (the clip it
// plays) share: by number, never by scanning for the first missing file, so a gap in the numbering cannot make them read
// different clips. Empty for a model path that is not an `.alo`, or an index outside 0..99.
[[nodiscard]] inline std::string attack_clip_path(const std::string_view model_path, const std::int64_t sub_index) {
    if (!model_path.ends_with(".alo") || sub_index < 0 || sub_index >= 100) return {};
    const auto slash = model_path.rfind('/');
    const auto directory = slash == std::string_view::npos ? std::string_view{} : model_path.substr(0, slash + 1);
    const auto stem = model_path.substr(directory.size(), model_path.size() - directory.size() - 4);
    std::string path;
    path.reserve(model_path.size() + 16);
    path.append(directory).append(stem).append("_attack_");
    if (sub_index < 10) path.push_back('0');
    path.append(std::to_string(sub_index)).append(".ala");
    return path;
}

} // namespace eawr::units

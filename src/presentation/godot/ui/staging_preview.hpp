#pragma once

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace godot { class ColorRect; class TextureRect; }
namespace eawr::assets { struct MegaTextureAtlas; }
namespace eawr::skirmish { struct SetupMap; struct SetupFaction; }
namespace eawr::vfs { class Vfs; }

namespace eawr::presentation::godot_backend {
class UiTextures;

struct StagingPreviewSeat final {
    std::uint32_t team{};
    std::string faction;
    godot::Color colour;
};

// WSS-10/63: view-owned start markers, shared by local and network staging.
// Rebuild only on map changes; roster edits reuse the resolved atlas textures.
class StagingPreview final {
public:
    void clear();
    void rebuild(godot::TextureRect& preview, const skirmish::SetupMap& map,
                 const vfs::Vfs& filesystem, std::span<const skirmish::SetupFaction> factions,
                 const assets::MegaTextureAtlas* atlas, UiTextures* textures);
    void update(std::span<const StagingPreviewSeat> occupied);
    [[nodiscard]] std::size_t size() const noexcept { return markers_.size(); }
    [[nodiscard]] std::size_t emblem_count() const noexcept;

private:
    struct Marker final {
        std::uint32_t team{};
        godot::ColorRect* backing{};
        godot::TextureRect* emblem{};
    };
    std::vector<Marker> markers_;
    std::map<std::string, godot::Ref<godot::Texture2D>, std::less<>> icons_;
};
} // namespace eawr::presentation::godot_backend

#pragma once
#include "eawr/data/ui/cursors.hpp"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {
// CU-01..03: one set of decoded install art, hardware frame swaps, and diagnostics.
class BattleCursor final {
public:
    ~BattleCursor();
    void prepare(const vfs::Vfs& filesystem);
    void update(std::string_view id, double delta);
    void write_report(std::ostream& output) const;
    void composite(godot::Ref<godot::Image> image, godot::Vector2 point) const;
    void capture_overlay(godot::Node& host, godot::Vector2 point, bool enabled);
private:
    struct Pointer {
        data::ui::CursorDefinition definition;
        std::vector<godot::Ref<godot::Image>> images;
        std::vector<godot::Ref<godot::ImageTexture>> textures;
    };
    std::map<std::string, Pointer, std::less<>> pointers_;
    const Pointer* active_{};
    std::vector<std::string> problems_;
    std::string id_;
    std::size_t frame_{};
    double elapsed_{};
    bool installed_{};
    std::uint64_t system_restores_{};
    godot::CanvasLayer* capture_layer_{};
    godot::TextureRect* capture_pointer_{};
};
}

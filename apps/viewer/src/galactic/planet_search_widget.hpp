#pragma once
#include "galactic_mode.hpp"
#include "planet_search.hpp"
#include "ui/dialog_builder.hpp"
#include <godot_cpp/classes/texture2d.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace godot { class InputEvent; class ItemList; class LineEdit; class TextureRect; }
namespace eawr::presentation::godot_backend {

// The provider owns its textures, including live viewport textures. Null means
// unavailable or pending; callers request only rows currently on screen.
class PlanetSearchThumbnails {
public:
    virtual ~PlanetSearchThumbnails() = default;
    virtual godot::Ref<godot::Texture2D> thumbnail(std::uint64_t planet, std::int32_t text_height) = 0;
};

class PlanetSearchWidget final {
public:
    void open(GalacticMode::State&);
    void close();
    void service(GalacticMode::State&);
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool pointer_inside() const;
    void observe_pointer(const godot::Ref<godot::InputEvent>&);
    void pointer_left();
    [[nodiscard]] std::string report_json() const;
private:
    void rebuild();
    void refresh();
    void key(const godot::Ref<godot::InputEvent>&);
    void choose(std::int64_t);
    void sync_focus();
    GalacticMode::State* state_{};
    godot::Control* root_{};
    std::optional<BuiltDialog> dialog_;
    godot::LineEdit* edit_{};
    godot::ItemList* list_{};
    godot::Vector2 size_;
    std::optional<godot::Vector2> pointer_;
    bool pointer_was_inside_{};
    viewer::galactic::PlanetSearchIndex index_;
    std::vector<std::uint64_t> matches_;
    std::vector<godot::TextureRect*> emblems_;
    std::map<std::uint64_t, std::string> pictures_;
    godot::Ref<godot::Texture2D> placeholder_;
    // PS-04 Row_Scale: the built list's unscaled List_Box size (a rebuilt list is measured again).
    std::uint64_t row_list_{}; // instance id
    std::int32_t row_base_size_{};
    std::int32_t placeholder_height_{};
    std::string close_text_; // the Close button's resolved caption, for the report
    std::uint64_t opens_{}, picks_{}, last_pick_{};
    bool dirty_{true};
};
} // namespace eawr::presentation::godot_backend

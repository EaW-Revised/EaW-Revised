#include "tactical_hud.hpp"

#include "perf_overlay_view.hpp"

#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

[[nodiscard]] String text(const std::string_view value) {
    return String::utf8(value.data(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            else output << static_cast<char>(character);
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] std::string rect_json(const Rect2& rect) {
    std::ostringstream output;
    output << "[" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
    return output.str();
}

[[nodiscard]] Rect2 rect2(const model::PixelRect& rect) {
    return Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
                 static_cast<float>(rect.height));
}

[[nodiscard]] Color colour(const data::ui::Rgba8& value) {
    return Color(value.r / 255.0F, value.g / 255.0F, value.b / 255.0F, value.a / 255.0F);
}

} // namespace

// The options button: the component's four state textures. P2-20a only counts
// presses; the in-game menu it opens is P2-20e. The control spans the drawn art
// quad, but only its component's mesh rect takes the pointer (hover and click),
// as FoC picks the shell mesh, not the art (docs/ui/ui-layer.md 1.3).
class EawrHudButton final : public TextureButton {
    GDCLASS(EawrHudButton, TextureButton)

public:
    EawrHudButton() {
        set_ignore_texture_size(true);
        set_stretch_mode(STRETCH_SCALE);
        set_focus_mode(FOCUS_NONE);
        set_mouse_filter(MOUSE_FILTER_STOP);
    }
    void _pressed() override {
        ++presses_;
        if (action_) action_();
        else UtilityFunctions::print("[eawr-hud] options pressed (P2-20a stub: the in-game menu is P2-20e)");
    }
    // #459: what a press does (the time panel's buttons); the options button has none yet.
    void set_action(std::function<void()> action) { action_ = std::move(action); }
    [[nodiscard]] int presses() const { return presses_; }
    // The interactive rect in the button's local coordinates.
    void set_hit_rect(const Rect2& rect) { hit_rect_ = rect; }
    [[nodiscard]] const Rect2& hit_rect() const { return hit_rect_; }
    bool _has_point(const Vector2& point) const override { return hit_rect_.has_point(point); }

protected:
    static void _bind_methods() {}

private:
    int presses_{};
    Rect2 hit_rect_;
    std::function<void()> action_;
};

EawrTacticalHud::EawrTacticalHud() {
    set_mouse_filter(MOUSE_FILTER_IGNORE);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

EawrTacticalHud::~EawrTacticalHud() {
    if (RenderingServer* rendering = RenderingServer::get_singleton()) {
        for (const RID& item : items_) rendering->free_rid(item);
        if (panel_item_.is_valid()) rendering->free_rid(panel_item_);
    }
}

void EawrTacticalHud::setup(Setup setup, TextureButton* options) {
    setup_ = std::move(setup);
    RenderingServer* rendering = RenderingServer::get_singleton();
    for (const Mesh& mesh : setup_.meshes) {
        const RID item = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(item, get_canvas_item());
        // Faceplate UVs repeat (UI-L2); the shell is scaled, so sample linearly.
        rendering->canvas_item_set_default_texture_repeat(item, RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
        rendering->canvas_item_set_default_texture_filter(item, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
        if (mesh.blend == model::ShellBlend::additive) {
            if (additive_.is_null()) {
                Ref<CanvasItemMaterial> material;
                material.instantiate();
                material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
                additive_ = material;
            }
            rendering->canvas_item_set_material(item, additive_->get_rid());
        }
        items_.push_back(item);
    }
    panel_item_ = rendering->canvas_item_create();
    rendering->canvas_item_set_parent(panel_item_, get_canvas_item());
    rendering->canvas_item_set_default_texture_filter(panel_item_, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
    options_ = options;
    if (options_ != nullptr) add_child(options_);
    for (const Setup::Placed& placed : setup_.time_buttons) add_child(placed.button);
    laid_out_ = Vector2(-1.0F, -1.0F);
    relayout();
}

void EawrTacticalHud::set_probe(std::function<void()> probe) {
    probe_ = std::move(probe);
    set_process(static_cast<bool>(probe_));
}

void EawrTacticalHud::_notification(const int what) {
    if (what == NOTIFICATION_RESIZED || what == NOTIFICATION_READY || what == NOTIFICATION_ENTER_TREE) relayout();
    if (what == NOTIFICATION_PROCESS && probe_ && laid_out_.x > 0.0F) {
        const std::function<void()> probe = std::move(probe_);
        probe_ = nullptr;
        set_process(false);
        probe();
    }
}

void EawrTacticalHud::_draw() {}

model::ReferenceSpace EawrTacticalHud::space() const {
    const Vector2 size = get_size();
    return model::reference_space(
        {static_cast<std::uint32_t>(std::max(0.0F, size.x)), static_cast<std::uint32_t>(std::max(0.0F, size.y))},
        setup_.rules);
}

model::ShellPlacement EawrTacticalHud::placement() const { return setup_.view.placement(space()); }

bool EawrTacticalHud::hit(const double x, const double y) const {
    return setup_.view.hit_test_screen({x, y}, space());
}

void EawrTacticalHud::relayout() {
    const Vector2 size = get_size();
    // Nothing to lay out before setup() has made the canvas items.
    if (size.x <= 0.0F || size.y <= 0.0F || size == laid_out_ || !panel_item_.is_valid()
        || items_.size() != setup_.meshes.size()) {
        return;
    }
    laid_out_ = size;
    const model::ReferenceSpace reference = space();
    const model::ShellPlacement shell = setup_.view.placement(reference);
    RenderingServer* rendering = RenderingServer::get_singleton();
    for (std::size_t index = 0; index < setup_.meshes.size(); ++index) {
        const Mesh& mesh = setup_.meshes[index];
        const RID item = items_[index];
        rendering->canvas_item_clear(item);
        if (mesh.texture.is_null()) continue;
        PackedVector2Array points;
        PackedVector2Array uvs;
        PackedColorArray colours;
        PackedInt32Array indices;
        for (const data::ui::ShellTriangle& triangle : mesh.triangles) {
            for (const data::ui::ShellVertex& vertex : triangle.vertices) {
                const model::ReferencePoint point = model::shell_point_to_screen(vertex.position.x, vertex.position.y, shell);
                indices.push_back(static_cast<int32_t>(points.size()));
                points.push_back(Vector2(static_cast<float>(point.x), static_cast<float>(point.y)));
                uvs.push_back(Vector2(vertex.uv.x, vertex.uv.y));
                colours.push_back(Color(1, 1, 1, 1));
            }
        }
        rendering->canvas_item_add_triangle_array(item, indices, points, colours, uvs, PackedInt32Array(),
                                                  PackedFloat32Array(), mesh.texture->get_rid());
    }
    rendering->canvas_item_clear(panel_item_);
    for (const Setup::Art& art : setup_.panel) {
        if (art.texture.is_null()) continue;
        rendering->canvas_item_add_texture_rect(panel_item_, rect2(model::shell_to_screen(art.quad, shell)),
                                                art.texture->get_rid());
    }
    if (options_ != nullptr && setup_.options_quad) {
        const Rect2 rect = rect2(model::shell_to_screen(*setup_.options_quad, shell));
        options_->set_position(rect.position);
        options_->set_size(rect.size);
        if (auto* button = Object::cast_to<EawrHudButton>(options_); button != nullptr && setup_.options_hit) {
            const Rect2 hit = rect2(model::shell_to_screen(*setup_.options_hit, shell));
            button->set_hit_rect(Rect2(hit.position - rect.position, hit.size));
        }
    }
    for (const Setup::Placed& placed : setup_.time_buttons) {
        const Rect2 rect = rect2(model::shell_to_screen(placed.quad, shell));
        placed.button->set_position(rect.position);
        placed.button->set_size(rect.size);
        if (auto* button = Object::cast_to<EawrHudButton>(placed.button); button != nullptr) {
            const Rect2 hit = rect2(model::shell_to_screen(placed.hit, shell));
            button->set_hit_rect(Rect2(hit.position - rect.position, hit.size));
        }
    }
    if (setup_.planet) {
        planet_rect_ = rect2(model::shell_to_screen(setup_.planet->rect, shell));
        const model::FontPixels pixels = model::font_pixels({setup_.planet->point_size, false, 1.0},
                                                            model::font_screen_height(reference));
        planet_pixels_ = pixels.glyph_height;
        KitTextStyle style;
        style.font = setup_.planet_font;
        style.size = pixels.glyph_height;
        style.top = style.bottom = colour(setup_.planet->colour);
        style.emboss = setup_.planet->emboss;
        style.outline = setup_.planet->outline;
        if (setup_.planet_cell) {
            const model::TextCell cell = setup_.planet_cell(pixels.glyph_height);
            style.cell_ascent = cell.ascent;
            style.cell_descent = cell.descent;
        }
        planet_.draw(*this, style, setup_.planet_text, planet_rect_, HORIZONTAL_ALIGNMENT_CENTER, true, false,
                     static_cast<float>(shell.scale), 1.0F);
    }
}

struct TacticalHud::State final {
    explicit State(Options value) : options(std::move(value)) {}

    Options options;
    std::string failure;
    std::string shell_model;
    std::string atlas_path;
    model::HudShell shell;
    model::PlanetName planet;
    std::shared_ptr<FontProvider> fonts;
    model::ResolvedFont planet_face;
    std::optional<assets::MegaTextureAtlas> atlas;
    std::unique_ptr<UiTextures> textures;
    std::vector<std::string> mesh_textures;   // "name: origin" per drawn mesh
    std::vector<std::string> button_textures; // "state: origin" for the options button
    struct PanelArt final {
        std::string name;
        std::string texture;
        std::string origin;
        data::ui::ReferenceRect quad; // shell units
    };
    std::vector<PanelArt> panel;
    std::vector<core::Diagnostic> diagnostics;
    std::size_t faceplates{};
    EawrUiHitMask* mask{};
    EawrTacticalHud* hud{};
    EawrHudButton* options_button{};
    // #459: the time panel's live buttons, and #453/#459's overlay.
    EawrHudButton* pause_button{};
    EawrHudButton* fast_forward_button{};
    EawrBattleOverlay* overlay{};
    TacticalHud::TimeHandlers time_handlers;
    TacticalHud::TimeView time_view;
    bool time_view_set{};
    // #848 V-5b: an overview level is on.
    bool overview{};
    model::BattleMessageLooks message_looks;
    std::vector<std::string> time_textures; // "button state: texture (origin)"
    // #425: the unit cards, and what they look up: object types (Icon_Name, Text_ID), the text DB
    // and the command bar's textures, each decoded once.
    EawrUnitCards* cards{};
    const data::Catalog* objects{};
    std::optional<data::ui::TextDatabase> text_database;
    model::StandaloneTextures standalone;
    std::map<std::string, Ref<Texture2D>, std::less<>> card_textures;
    std::map<std::string, std::pair<std::string, String>, std::less<>> card_types; // Icon_Name, display name
    model::ResolvedFont card_face;
    const std::pair<std::string, String>& card_type(const std::string& type);
    // A command bar texture (the atlas, then files) at texel size, decoded once; null when missing.
    Ref<Texture2D> command_texture(const std::string& name, const std::string& use, bool quiet = false);
    // #454: the ability buttons.
    EawrAbilityButtons* abilities{};
    // #530: the build queue, the credits and the reinforcement pane.
    EawrProductionPanel* production{};
    // #455: the minimap and its model state.
    EawrMinimap* minimap{};
    model::MinimapSettings minimap_settings;
    model::MinimapFog minimap_fog;
    model::MinimapExtents minimap_extents;
    std::map<std::string, model::MinimapTypeLooks, std::less<>> minimap_types;
    std::function<void(double, double)> minimap_look;
    std::function<void(double, double)> minimap_move;
    int world_presses{};
    struct Probe final {
        std::string name;
        Vector2 point;
        bool hud_hit{};
        int world{};
        int button{};
    };
    std::vector<Probe> probes;
    bool probed{};

    void run_probe();

    void warn(std::string code, std::string message, std::string path = {}) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::move(code);
        diagnostic.severity = core::Severity::warning;
        diagnostic.message = std::move(message);
        diagnostic.logical_path = std::move(path);
        diagnostics.push_back(std::move(diagnostic));
    }
};

const std::pair<std::string, String>& TacticalHud::State::card_type(const std::string& type) {
    const auto found = card_types.find(type);
    if (found != card_types.end()) return found->second;
    const model::UnitCardLooks found_looks =
        model::unit_card_looks(type, objects, text_database ? &*text_database : nullptr);
    std::pair<std::string, String> looks{found_looks.icon, text(found_looks.name)};
    if (looks.first.empty()) warn("EAWR-UI-0322", type + " has no Icon_Name; its unit card shows the engine's temporary portrait", type);
    return card_types.emplace(type, std::move(looks)).first->second;
}

void TacticalHud::State::run_probe() {
    Viewport* viewport = hud->get_viewport();
    if (viewport == nullptr) return;
    const model::ShellPlacement placement = hud->placement();
    const auto screen = [&](const double x, const double y) {
        const model::ReferencePoint point = model::shell_point_to_screen(x, y, placement);
        return Vector2(static_cast<float>(point.x), static_cast<float>(point.y));
    };
    const Vector2 size = hud->get_size();
    const Vector2 sky(size.x * 0.5F, size.y * 0.25F);
    const auto move = [&](const Vector2& at) {
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(at);
        motion->set_global_position(at);
        viewport->push_input(motion, true);
    };
    std::vector<std::pair<std::string, Vector2>> points;
    if (options_button != nullptr) {
        points.emplace_back("options", options_button->get_position() + options_button->get_size() * 0.5F);
    }
    // The art overhangs the component's mesh; only the mesh takes the click.
    // Probe half a shell unit inside each side edge and 3 units into each overhang.
    if (shell.options) {
        const data::ui::ReferenceRect& rect = shell.options->rect;
        const double y = rect.y + rect.height / 2.0;
        points.emplace_back("options_inside_left", screen(rect.x + 0.5, y));
        points.emplace_back("options_inside_right", screen(rect.x + rect.width - 0.5, y));
        points.emplace_back("options_overhang_left", screen(rect.x - 3.0, y));
        points.emplace_back("options_overhang_right", screen(rect.x + rect.width + 3.0, y));
    }
    points.emplace_back("sky", sky);
    if (shell.minimap) {
        points.emplace_back("minimap", screen(shell.minimap->x + shell.minimap->width / 2.0,
                                              shell.minimap->y + shell.minimap->height / 2.0));
    }
    // Section 1.3 geometry: panel art between the tech and population texts, and
    // the transparent sky over the unit strip inside the HUD's extent.
    points.emplace_back("faceplate_art", screen(150.0, 215.0));
    points.emplace_back("faceplate_gap", screen(700.0, 250.0));
    for (const auto& [name, at] : points) {
        const int world_before = world_presses;
        const int button_before = options_button != nullptr ? options_button->presses() : 0;
        move(at);
        for (const bool pressed : {true, false}) {
            Ref<InputEventMouseButton> click;
            click.instantiate();
            click->set_button_index(MOUSE_BUTTON_LEFT);
            click->set_pressed(pressed);
            click->set_button_mask(pressed ? BitField<MouseButtonMask>(MOUSE_BUTTON_MASK_LEFT)
                                           : BitField<MouseButtonMask>(0));
            click->set_position(at);
            click->set_global_position(at);
            viewport->push_input(click, true);
        }
        const int button_after = options_button != nullptr ? options_button->presses() : 0;
        probes.push_back({name, at, hud->hit(at.x, at.y), world_presses - world_before, button_after - button_before});
    }
    // Leave the pointer over the world, so the capture shows every button unhovered.
    move(sky);
    probed = true;
}

void TacticalHud::world_input(const Ref<InputEvent>& event) {
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (button != nullptr && button->is_pressed() && button->get_button_index() == MOUSE_BUTTON_LEFT) {
        ++state_->world_presses;
    }
}

TacticalHud::TacticalHud(Options options) : state_(std::make_unique<State>(std::move(options))) {}
TacticalHud::~TacticalHud() = default;

const std::string& TacticalHud::failure() const noexcept { return state_->failure; }
Ref<Texture2D> TacticalHud::State::command_texture(const std::string& name, const std::string& use, const bool quiet) {
    const auto found = card_textures.find(name);
    if (found != card_textures.end()) return found->second;
    const model::ThemeTexture slot = model::resolve_ui_texture(name, atlas ? &atlas->directory : nullptr, standalone);
    Ref<Texture2D> result = textures ? textures->texture(slot, 1.0, 1.0) : Ref<Texture2D>();
    if (result.is_null() && !quiet) warn("EAWR-UI-0322", use + " texture " + name + " cannot be drawn", name);
    card_textures.emplace(name, result);
    return result;
}

int TacticalHud::options_presses() const noexcept {
    return state_->options_button != nullptr ? state_->options_button->presses() : 0;
}
EawrTacticalHud* TacticalHud::hud() const noexcept { return state_->hud; }

bool TacticalHud::build(const vfs::Vfs& filesystem, const data::Catalog* objects,
                        const std::optional<std::string>& context_name, Node& parent) {
    State& state = *state_;
    auto command_bar = data::ui::load_command_bar(filesystem);
    if (!command_bar) {
        state.failure = core::format_diagnostic(command_bar.error());
        return false;
    }
    const data::ui::CommandBarCatalog& catalog = command_bar.value().catalog;
    state.shell_model = model::tactical_shell_model(catalog);
    auto anchors = data::ui::load_shell_anchors(filesystem, state.shell_model);
    if (!anchors) {
        state.failure = core::format_diagnostic(anchors.error());
        return false;
    }
    for (const auto& diagnostic : anchors.value().diagnostics) state.diagnostics.push_back(diagnostic);
    state.shell = model::hud_shell(anchors.value().shell, catalog, state.options.faction);
    for (const auto& diagnostic : state.shell.diagnostics) state.diagnostics.push_back(diagnostic);

    EawrTacticalHud::Setup setup;
    setup.rules = state.options.rules;
    setup.view = model::hud_view_model(anchors.value().shell, catalog, model::alt_variant(state.options.faction),
                                       model::vfs_shell_masks(filesystem));
    for (const auto& diagnostic : setup.view.diagnostics) state.diagnostics.push_back(diagnostic);
    state.faceplates = setup.view.faceplates.size();

    // Shell art: texture files under Data/Art/Textures, decoded once per name.
    const model::StandaloneTextures standalone = model::vfs_standalone_textures(filesystem);
    std::map<std::string, Ref<Texture2D>> loaded;
    for (const model::HudShellMesh& mesh : state.shell.meshes) {
        auto [found, inserted] = loaded.try_emplace(mesh.texture);
        std::string origin = "loaded";
        if (inserted) {
            std::string why;
            if (const auto path = standalone(mesh.texture)) {
                if (auto texture = assets::load_texture(filesystem, *path)) {
                    const Ref<Image> image = texture_image(texture.value(), why);
                    if (image.is_valid()) found->second = ImageTexture::create_from_image(image);
                } else {
                    why = core::format_diagnostic(texture.error());
                }
            } else {
                why = "no texture file";
            }
            if (found->second.is_null()) {
                origin = "missing";
                state.warn("EAWR-UI-0322", mesh.name + ": " + mesh.texture + " cannot be drawn: " + why, mesh.texture);
            }
        } else if (found->second.is_null()) {
            origin = "missing";
        }
        state.mesh_textures.push_back(mesh.name + ": " + mesh.texture + " (" + origin + ")");
        setup.meshes.push_back({mesh.blend, found->second, mesh.triangles});
    }

    // The options button's state textures: the command bar's atlas, then files.
    state.atlas_path = model::command_bar_mega_texture(catalog);
    if (auto atlas = assets::load_mega_texture_atlas(filesystem, state.atlas_path)) {
        state.atlas.emplace(std::move(atlas.value()));
        state.textures = std::make_unique<UiTextures>(*state.atlas, filesystem);
        if (!state.textures->page_failure().empty()) {
            state.warn("EAWR-UI-0322", "atlas page: " + state.textures->page_failure(), state.atlas_path);
        }
    } else {
        state.warn("EAWR-UI-0322", core::format_diagnostic(atlas.error()), state.atlas_path);
    }
    // A button texture at texel size, "missing" when it cannot be drawn.
    const auto button_texture = [&](const std::string& button, const std::string& name, std::string& origin) {
        const model::ThemeTexture slot =
            model::resolve_ui_texture(name, state.atlas ? &state.atlas->directory : nullptr, standalone);
        Ref<Texture2D> result = state.textures ? state.textures->texture(slot, 1.0, 1.0) : Ref<Texture2D>();
        origin = result.is_valid() ? std::string(model::to_string(slot.origin)) : "missing";
        if (result.is_null()) state.warn("EAWR-UI-0322", button + ": " + name + " cannot be drawn", name);
        return result;
    };
    // Button art keeps its texture's size around the bone (button_quad).
    const auto quad = [](const model::HudShellButton& button, const Ref<Texture2D>& texture) {
        const Vector2 size = texture.is_valid() ? texture->get_size() : Vector2(button.rect.width, button.rect.height);
        return model::button_quad(button, size.x, size.y);
    };
    if (state.shell.options) {
        const model::HudShellButton& look = state.shell.options.value();
        state.options_button = memnew(EawrHudButton);
        state.options_button->set_name("b_option_t");
        const auto texture = [&](const std::string& state_name, const std::string& name) -> Ref<Texture2D> {
            if (name.empty()) return {};
            std::string origin;
            Ref<Texture2D> result = button_texture("b_option_t " + state_name, name, origin);
            state.button_textures.push_back(state_name + ": " + name + " (" + origin + ")");
            return result;
        };
        const Ref<Texture2D> normal = texture("normal", look.normal);
        state.options_button->set_texture_normal(normal);
        state.options_button->set_texture_hover(texture("mouse_over", look.mouse_over));
        state.options_button->set_texture_pressed(texture("pressed", look.pressed));
        state.options_button->set_texture_disabled(texture("disabled", look.disabled));
        setup.options_quad = quad(look, normal);
        setup.options_hit = look.rect;
    }
    for (const model::HudShellButton& button : state.shell.panel_buttons) {
        std::string origin = "none";
        Ref<Texture2D> texture;
        if (!button.normal.empty()) texture = button_texture(button.name, button.normal, origin);
        const data::ui::ReferenceRect rect = quad(button, texture);
        state.panel.push_back({button.name, button.normal, origin, rect});
        // #459: pause and fast forward are toggle buttons with their four state textures
        // (TM-05, TM-06); help and holocron stay inert art.
        const bool pause = button.name == "b_play_pause_t";
        if (!pause && button.name != "b_fast_forward_t") {
            setup.panel.push_back({texture, rect});
            continue;
        }
        auto* control = memnew(EawrHudButton);
        control->set_name(String(button.name.c_str()));
        control->set_toggle_mode(true);
        // TM-06: fast forward acts on the press, pause on the release.
        if (!pause) control->set_action_mode(BaseButton::ACTION_MODE_BUTTON_PRESS);
        const auto state_texture = [&](const std::string& state_name, const std::string& name) -> Ref<Texture2D> {
            if (name.empty()) return {};
            std::string texture_origin;
            Ref<Texture2D> result = button_texture(button.name + " " + state_name, name, texture_origin);
            state.time_textures.push_back(button.name + " " + state_name + ": " + name + " (" + texture_origin + ")");
            return result;
        };
        control->set_texture_normal(texture);
        control->set_texture_hover(state_texture("mouse_over", button.mouse_over));
        control->set_texture_pressed(state_texture("pressed", button.pressed));
        control->set_texture_disabled(state_texture("disabled", button.disabled));
        control->set_action([&state, pause] {
            const auto& handler = pause ? state.time_handlers.pause : state.time_handlers.fast_forward;
            if (handler) handler();
        });
        (pause ? state.pause_button : state.fast_forward_button) = control;
        setup.time_buttons.push_back({control, rect, button.rect});
    }

    // The planet name: its component font through UI-F3, the text through the map.
    std::optional<data::ui::TextDatabase>& text_database = state.text_database;
    if (auto loaded_text = data::ui::load_language_text_database(filesystem, state.options.language)) {
        text_database.emplace(std::move(loaded_text.value()));
    } else {
        state.warn("EAWR-UI-0321", core::format_diagnostic(loaded_text.error()));
    }
    state.objects = objects;
    state.standalone = standalone;
    state.planet = model::planet_name(context_name, objects, text_database ? &*text_database : nullptr);
    for (const auto& diagnostic : state.planet.diagnostics) state.diagnostics.push_back(diagnostic);
    if (state.shell.planet_name) {
        state.fonts = std::make_shared<FontProvider>(std::move(state.options.font_cache));
        state.planet_face = state.fonts->resolve(
            {state.shell.planet_name->face, state.shell.planet_name->point_size}, state.options.language);
        if (state.planet_face.substituted || state.planet_face.source == model::FaceSource::engine_default) {
            state.warn("EAWR-UI-0321", state.shell.planet_name->face + " is unavailable (font cache "
                           + state.fonts->cache().directory + "); the planet name uses "
                           + (state.planet_face.face.empty() ? std::string("the engine font") : state.planet_face.face));
        }
        setup.planet = state.shell.planet_name;
        setup.planet->point_size = state.planet_face.point_size;
        setup.planet_font = state.fonts->font(state.planet_face);
        setup.planet_cell = [fonts = state.fonts, face = state.planet_face](const std::int32_t height) {
            return model::gdi_text_cell(fonts->cache(), face, height);
        };
        setup.planet_text = text(state.planet.text);
    }

    // Canvas layer -> hit mask (UI-I2) -> drawing surface -> button.
    auto* layer = memnew(CanvasLayer);
    layer->set_name("EawrTacticalHudLayer");
    state.mask = memnew(EawrUiHitMask);
    state.mask->set_name("EawrTacticalHudMask");
    state.hud = memnew(EawrTacticalHud);
    state.hud->set_name("EawrTacticalHud");
    EawrTacticalHud* hud = state.hud;
    state.mask->set_hit_test([hud](const double x, const double y) { return hud->hit(x, y); });
    state.mask->add_child(state.hud);
    layer->add_child(state.mask);
    parent.add_child(layer);
    state.hud->setup(std::move(setup), state.options_button);

    // #425: the unit cards over the shell, in the slots' own face.
    if (!state.shell.card_slots.empty()) {
        if (!state.fonts) state.fonts = std::make_shared<FontProvider>(std::move(state.options.font_cache));
        const model::HudCardSlot& first = state.shell.card_slots.front();
        state.card_face = state.fonts->resolve({first.face, first.point_size}, state.options.language);
        if (state.card_face.substituted || state.card_face.source == model::FaceSource::engine_default) {
            state.warn("EAWR-UI-0321", first.face + " is unavailable (font cache " + state.fonts->cache().directory
                           + "); the unit card counts use "
                           + (state.card_face.face.empty() ? std::string("the engine font") : state.card_face.face));
        }
        EawrUnitCards::Setup cards;
        cards.slots = state.shell.card_slots;
        cards.borders = state.shell.card_borders;
        cards.texture = [&state](const std::string& name) { return state.command_texture(name, "unit card"); };
        cards.icon = [&state](const std::string& type) { return state.card_type(type).first; };
        cards.name = [&state](const std::string& type) { return state.card_type(type).second; };
        cards.font = state.fonts->font(state.card_face);
        cards.cell = [fonts = state.fonts, face = state.card_face](const std::int32_t height) {
            return model::gdi_text_cell(fonts->cache(), face, height);
        };
        cards.point_size = state.card_face.point_size;
        // GameConstants.xml Encyclopedia_Delay: 750 ms before a hovered card opens its encyclopedia.
        cards.hover_delay_seconds = 0.75;
        cards.placement = [hud] { return hud->placement(); };
        cards.space = [hud] { return hud->space(); };
        state.cards = memnew(EawrUnitCards);
        state.cards->set_name("EawrUnitCards");
        state.hud->add_child(state.cards);
        state.cards->setup(std::move(cards));
    }
    // #454: the ability buttons and the cards' ability marks, over the cards.
    if (!state.shell.ability_buttons.empty()) {
        EawrAbilityButtons::Setup abilities;
        abilities.buttons = state.shell.ability_buttons;
        abilities.slots = state.shell.card_slots;
        abilities.texture = [&state](const std::string& name) { return state.command_texture(name, "ability button"); };
        abilities.placement = [hud] { return hud->placement(); };
        state.abilities = memnew(EawrAbilityButtons);
        state.abilities->set_name("EawrAbilityButtons");
        state.hud->add_child(state.abilities);
        state.abilities->setup(std::move(abilities));
    }
    // #530: the build queue, the credits and the reinforcement pane (space-purchasing PU-63 to
    // PU-67), in the card slots' face.
    if (!state.shell.queue_slots.empty() || state.shell.credits || state.shell.reinforcement) {
        EawrProductionPanel::Setup production;
        production.queue = state.shell.queue_slots;
        production.credits = state.shell.credits;
        production.reinforce = state.shell.reinforcement;
        if (auto pane_anchors = data::ui::load_shell_anchors(filesystem, model::reinforce_pane_model(catalog))) {
            for (const auto& diagnostic : pane_anchors.value().diagnostics) state.diagnostics.push_back(diagnostic);
            auto pane = model::reinforce_pane(pane_anchors.value().shell, catalog, state.options.faction);
            for (const auto& diagnostic : pane.diagnostics) state.diagnostics.push_back(diagnostic);
            production.pane = std::move(pane);
        } else {
            state.diagnostics.push_back(pane_anchors.error());
        }
        production.texture = [&state](const std::string& name) { return state.command_texture(name, "production"); };
        production.optional_texture = [&state](const std::string& name) { return state.command_texture(name, "production", true); };
        production.icon = [&state](const std::string& type) { return state.card_type(type).first; };
        if (!state.fonts) state.fonts = std::make_shared<FontProvider>(std::move(state.options.font_cache));
        const model::ResolvedFont face = state.fonts->resolve({"EmpireAtWar-Medium", 6}, state.options.language);
        production.font = state.fonts->font(face);
        production.cell = [fonts = state.fonts, face](const std::int32_t height) {
            return model::gdi_text_cell(fonts->cache(), face, height);
        };
        production.placement = [hud] { return hud->placement(); };
        production.space = [hud] { return hud->space(); };
        state.production = memnew(EawrProductionPanel);
        state.production->set_name("EawrProductionPanel");
        state.hud->add_child(state.production);
        state.production->setup(std::move(production));
    }
    // #455: the minimap inside the radar mesh; its icons are command bar textures (MM-06).
    if (state.shell.minimap) {
        state.minimap_settings = model::minimap_settings(filesystem);
        for (const auto& diagnostic : state.minimap_settings.diagnostics) state.diagnostics.push_back(diagnostic);
        EawrMinimap::Setup minimap;
        minimap.rect = *state.shell.minimap;
        minimap.backdrop = state.minimap_settings.backdrop;
        minimap.background = state.minimap_settings.background;
        minimap.texture = [&state](const std::string& name) { return state.command_texture(name, "minimap"); };
        minimap.placement = [hud] { return hud->placement(); };
        state.minimap = memnew(EawrMinimap);
        state.minimap->set_name("EawrMinimap");
        state.hud->add_child(state.minimap);
        state.minimap->setup(std::move(minimap));
        state.minimap->set_look([&state](const model::MinimapPoint point) {
            const auto world = model::minimap_world(state.minimap_extents, point);
            if (state.minimap_look) state.minimap_look(world[0], world[1]);
        });
        state.minimap->set_move([&state](const model::MinimapPoint point) {
            const auto world = model::minimap_world(state.minimap_extents, point);
            if (state.minimap_move) state.minimap_move(world[0], world[1]);
        });
    }
    // #453, #459: the battle overlay above the HUD. Texts from the text DB, looks from
    // GameConstants.xml (BE-03, TM-09) and the pause banner's component fonts.
    state.message_looks = model::battle_message_looks(filesystem);
    for (const auto& diagnostic : state.message_looks.diagnostics) state.diagnostics.push_back(diagnostic);
    if (!state.fonts) state.fonts = std::make_shared<FontProvider>(std::move(state.options.font_cache));
    const auto overlay_font = [&](const std::string& face, const std::int32_t points, const std::string& use) {
        const model::ResolvedFont resolved = state.fonts->resolve({face, points}, state.options.language);
        if (resolved.substituted || resolved.source == model::FaceSource::engine_default) {
            state.warn("EAWR-UI-0321", face + " is unavailable (font cache " + state.fonts->cache().directory + "); the "
                           + use + " falls back to " + (resolved.face.empty() ? std::string("the engine font") : resolved.face));
        }
        return EawrBattleOverlay::Font{state.fonts->font(resolved), resolved.point_size};
    };
    const auto game_text = [&](const std::string_view key) {
        const data::ui::TextEntry* entry = text_database ? text_database->find(key) : nullptr;
        if (entry == nullptr) {
            state.warn("EAWR-UI-0321", std::string(key) + " is not in the text DB; the key is shown");
            return text(key);
        }
        return text(data::ui::to_utf8(entry->value));
    };
    // TM-09: the pause text and its button take the pause shell's text components' faces
    // (text_attack, attack_button: EmpireAtWar-Medium 7 in FoC).
    const auto component_font = [&](const std::string_view name) -> std::pair<std::string, std::int32_t> {
        const data::ui::CommandBarComponent* component = catalog.find(name);
        if (component == nullptr) return {"EmpireAtWar-Medium", 7};
        const auto faces = component->list(data::ui::Field::font_name);
        return {faces.empty() ? std::string("EmpireAtWar-Medium") : faces.front(),
                component->integer(data::ui::Field::font_point_size).value_or(7)};
    };
    const data::ui::CommandBarComponent* resume_component = catalog.find("attack_button");
    const data::ui::Rgba8 resume_colour = resume_component != nullptr
        ? resume_component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255, 64, 64, 255})
        : data::ui::Rgba8{255, 64, 64, 255};
    EawrBattleOverlay::Style overlay;
    overlay.message = overlay_font(state.message_looks.font, state.message_looks.point_size, "win/lose message");
    overlay.win = colour(state.message_looks.win);
    overlay.lose = colour(state.message_looks.lose);
    const auto [banner_face, banner_points] = component_font("text_attack");
    overlay.banner = overlay_font(banner_face, banner_points, "pause banner");
    overlay.paused = colour(state.message_looks.pending);
    // BEP-03 and TP-07 (project): the Resume Game and Quit Game captions at 12 points in the
    // banner button's face, so they stay readable without the banner shell.
    const auto [button_face, button_points] = component_font("attack_button");
    static_cast<void>(button_points);
    overlay.button = overlay_font(button_face, 12, "battle buttons");
    overlay.button_colour = colour(resume_colour);
    overlay.win_text = game_text(model::battle_message_key(model::BattleResult::victory));
    overlay.lose_text = game_text(model::battle_message_key(model::BattleResult::defeat));
    overlay.victory_title = game_text(model::battle_end_title_key(model::BattleResult::victory));
    overlay.defeat_title = game_text(model::battle_end_title_key(model::BattleResult::defeat));
    overlay.paused_text = game_text(model::pause_text_key);
    overlay.resume_text = game_text(model::pause_resume_key);
    overlay.quit_text = game_text(model::battle_end_quit_key);
    overlay.space = [hud] { return hud->space(); };
    state.overlay = memnew(EawrBattleOverlay);
    state.overlay->set_name("EawrBattleOverlay");
    layer->add_child(state.overlay);
    state.overlay->setup(std::move(overlay),
        [&state] { if (state.time_handlers.resume) state.time_handlers.resume(); },
        [&state] { if (state.time_handlers.quit) state.time_handlers.quit(); });
    if (state.options.probe) state.hud->set_probe([&state] { state.run_probe(); });
    return true;
}

void TacticalHud::set_time_handlers(TimeHandlers handlers) { state_->time_handlers = std::move(handlers); }

void TacticalHud::set_time_view(const TimeView& view) {
    State& state = *state_;
    state.time_view = view;
    state.time_view_set = true;
    if (state.pause_button != nullptr) {
        state.pause_button->set_pressed_no_signal(view.paused);
        state.pause_button->set_disabled(!view.pause_enabled);
    }
    if (state.fast_forward_button != nullptr) {
        state.fast_forward_button->set_pressed_no_signal(view.fast_forward);
        state.fast_forward_button->set_disabled(!view.fast_forward_enabled);
        // TM-08: tinted grey (128) while disabled.
        const float tint = view.fast_forward_enabled ? 1.0F : 128.0F / 255.0F;
        state.fast_forward_button->set_self_modulate(Color(tint, tint, tint, 1.0F));
    }
    if (state.overlay != nullptr) state.overlay->show_paused(view.paused && !state.overview);
}

void TacticalHud::set_overview(const bool on) {
    State& state = *state_;
    if (state.overview == on) return;
    state.overview = on;
    // V-5b: the hit mask carries the shell and everything placed on it; hidden, it takes no input.
    if (state.mask != nullptr) state.mask->set_visible(!on);
    if (state.overlay != nullptr) state.overlay->show_paused(state.time_view.paused && !on);
}

bool TacticalHud::shell_shown() const { return state_->mask != nullptr && state_->mask->is_visible(); }

bool TacticalHud::pause_banner_shown() const { return state_->overlay != nullptr && state_->overlay->paused_shown(); }

void TacticalHud::set_battle(const std::optional<bool> won, const bool ended) {
    if (state_->overlay == nullptr) return;
    state_->overlay->show_message(won);
    state_->overlay->show_end(ended ? won : std::nullopt);
}

std::optional<std::array<float, 2>> TacticalHud::control_point(const std::string& name) const {
    const State& state = *state_;
    const auto centre = [](const Rect2& rect) {
        const Vector2 point = rect.get_center();
        return std::array<float, 2>{point.x, point.y};
    };
    const auto button_centre = [&](const EawrHudButton* button) -> std::optional<std::array<float, 2>> {
        if (button == nullptr || !button->is_visible_in_tree()) return std::nullopt;
        return centre(Rect2(button->get_global_position() + button->hit_rect().position, button->hit_rect().size));
    };
    if (name == "pause") return button_centre(state.pause_button);
    if (state.production != nullptr) {
        if (const auto rect = state.production->control_rect(name)) return centre(*rect);
    }
    if (name == "fast_forward") return button_centre(state.fast_forward_button);
    if (state.overlay != nullptr) {
        if (const auto rect = state.overlay->control_rect(name)) return centre(*rect);
    }
    return std::nullopt;
}

EawrUnitCards* TacticalHud::unit_cards() const noexcept { return state_->cards; }

EawrProductionPanel* TacticalHud::production() const noexcept { return state_->production; }

std::optional<std::int64_t> TacticalHud::listed_build_cost(const std::string& type) const {
    return model::unit_card_looks(type, state_->objects, nullptr).build_cost;
}

EawrMinimap* TacticalHud::minimap() const noexcept { return state_->minimap; }

EawrAbilityButtons* TacticalHud::ability_buttons() const noexcept { return state_->abilities; }

void TacticalHud::set_ability_bar(const model::AbilityBar& bar) {
    if (state_->abilities != nullptr) state_->abilities->show(bar);
}

void TacticalHud::set_minimap(const MinimapView& view) {
    State& state = *state_;
    if (state.minimap == nullptr) return;
    state.minimap_extents = view.extents;
    const auto looks = [this](const std::string_view type) -> const model::MinimapTypeLooks& { return minimap_looks(type); };
    const auto [width, height] = state.minimap->pixel_size();
    state.minimap_fog.resize(width, height);
    const bool advanced = view.cells
        ? state.minimap_fog.advance(view.extents, *view.cells, state.minimap_settings.fog, view.fog)
        : state.minimap_fog.advance(view.extents, view.revealers, state.minimap_settings.fog, view.fog);
    if (advanced) {
        state.minimap->set_fog(state.minimap_fog.texels(), state.minimap_fog.width(), state.minimap_fog.height(),
                               state.minimap_fog.passes());
    }
    EawrMinimap::Frame frame;
    frame.blips = model::minimap_blips(view.units, looks, view.extents, state.minimap_settings);
    if (view.ground) frame.guide = model::minimap_guide(*view.ground, view.extents, state.minimap_settings.guide_rectangle);
    state.minimap->show(std::move(frame));
}

void TacticalHud::set_minimap_handlers(std::function<void(double, double)> look, std::function<void(double, double)> move) {
    state_->minimap_look = std::move(look);
    state_->minimap_move = std::move(move);
}

const model::MinimapTypeLooks& TacticalHud::minimap_looks(const std::string_view type) {
    State& state = *state_;
    const auto found = state.minimap_types.find(type);
    if (found != state.minimap_types.end()) return found->second;
    return state.minimap_types.emplace(std::string(type), model::minimap_type_looks(type, state.objects)).first->second;
}

std::optional<data::ui::Rgba8> TacticalHud::faction_colour(const std::string_view faction) const {
    return model::faction_colour(state_->minimap_settings, faction);
}

std::optional<std::array<float, 2>> TacticalHud::minimap_point(const double x, const double y) const {
    const EawrMinimap* minimap = state_->minimap;
    if (minimap == nullptr || !minimap->is_visible_in_tree() || !minimap->minimap_rect().has_area()) return std::nullopt;
    const Vector2 at = minimap->to_screen({x, y});
    return std::array<float, 2>{at.x, at.y};
}

void TacticalHud::set_unit_cards(const model::CardLayout& layout, const std::span<const model::CardUnit> units) {
    if (state_->cards == nullptr) return;
    std::vector<EawrUnitCards::Card> cards;
    cards.reserve(layout.cards.size());
    for (const model::UnitCard& card : layout.cards) {
        if (card.unit >= units.size()) continue;
        cards.push_back({card.slot, units[card.unit].type, card.count, card.stacked, card.health_level, card.shield});
    }
    state_->cards->show(std::move(cards), layout.borders);
}

std::string TacticalHud::report_json() const {
    const State& state = *state_;
    std::ostringstream output;
    output << "{\"mode\": \"tactical\", \"faction\": " << json(model::to_string(state.options.faction))
           << ", \"rules\": " << json(state.options.rules == model::LayoutRules::retail ? "retail" : "aspect")
           << ", \"shell_model\": " << json(state.shell_model) << ", \"atlas\": " << json(state.atlas_path)
           << ", \"faceplate_masks\": " << state.faceplates << ", \"meshes\": [";
    for (std::size_t index = 0; index < state.mesh_textures.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(state.mesh_textures[index]);
    }
    output << "], \"options_textures\": [";
    for (std::size_t index = 0; index < state.button_textures.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(state.button_textures[index]);
    }
    output << "], \"planet_name\": {\"text\": " << json(state.planet.text)
           << ", \"source\": " << json(model::to_string(state.planet.source))
           << ", \"context\": " << json(state.planet.context) << ", \"text_id\": " << json(state.planet.text_id)
           << ", \"face\": " << json(state.planet_face.face)
           << ", \"face_source\": " << json(model::to_string(state.planet_face.source)) << "}"
           << ", \"font_cache\": {\"directory\": " << json(state.fonts ? state.fonts->cache().directory : std::string())
           << ", \"source\": " << json(state.options.font_cache_source) << "}";
    if (state.hud != nullptr) {
        const Vector2 size = state.hud->get_size();
        const model::ShellPlacement shell = state.hud->placement();
        output << ", \"viewport\": [" << size.x << ", " << size.y << "]"
               << ", \"placement\": {\"left\": " << shell.left << ", \"bottom\": " << shell.bottom
               << ", \"scale\": " << shell.scale << "}";
        if (state.shell.minimap) {
            output << ", \"minimap_rect\": " << rect_json(rect2(model::shell_to_screen(*state.shell.minimap, shell)));
        }
        output << ", \"panel_buttons\": [";
        for (std::size_t index = 0; index < state.panel.size(); ++index) {
            const State::PanelArt& art = state.panel[index];
            output << (index == 0 ? "" : ", ") << "{\"name\": " << json(art.name) << ", \"texture\": "
                   << json(art.texture) << ", \"origin\": " << json(art.origin) << ", \"rect\": "
                   << rect_json(rect2(model::shell_to_screen(art.quad, shell))) << "}";
        }
        output << "]";
        if (state.options_button != nullptr) {
            output << ", \"options_rect\": "
                   << rect_json(Rect2(state.options_button->get_position(), state.options_button->get_size()))
                   << ", \"options_hit_rect\": "
                   << rect_json(Rect2(state.options_button->get_position() + state.options_button->hit_rect().position,
                                      state.options_button->hit_rect().size));
        }
        output << ", \"planet_rect\": " << rect_json(state.hud->planet_rect())
               << ", \"planet_pixels\": " << state.hud->planet_pixels();
    }
    if (state.options.probe) {
        output << ", \"probe\": {\"ran\": " << (state.probed ? "true" : "false") << ", \"clicks\": [";
        for (std::size_t index = 0; index < state.probes.size(); ++index) {
            const State::Probe& probe = state.probes[index];
            output << (index == 0 ? "" : ", ") << "{\"name\": " << json(probe.name) << ", \"point\": ["
                   << probe.point.x << ", " << probe.point.y << "], \"hud_hit\": " << (probe.hud_hit ? "true" : "false")
                   << ", \"world\": " << probe.world << ", \"button\": " << probe.button << "}";
        }
        output << "]}";
    }
    if (state.cards != nullptr) output << ", \"unit_cards\": " << state.cards->report_json();
    if (state.production != nullptr) output << ", \"production\": " << state.production->report_json();
    if (state.abilities != nullptr) output << ", \"ability_buttons\": " << state.abilities->report_json();
    if (state.minimap != nullptr) {
        output << ", \"minimap\": " << state.minimap->report_json() << ", \"minimap_fog\": {\"fogged\": "
               << state.minimap_fog.fogged() << ", \"next_row\": " << state.minimap_fog.next_row()
               << ", \"rows_per_frame\": " << model::MinimapFog::rows_per_frame << "}";
    }
    // #459: the time panel's buttons; #453/#459: the overlay.
    output << ", \"time_panel\": {\"textures\": [";
    for (std::size_t index = 0; index < state.time_textures.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(state.time_textures[index]);
    }
    output << "]";
    for (const auto& [name, button] : {std::pair<const char*, const EawrHudButton*>{"pause", state.pause_button},
                                       std::pair<const char*, const EawrHudButton*>{"fast_forward", state.fast_forward_button}}) {
        output << ", \"" << name << "\": ";
        if (button == nullptr) {
            output << "null";
            continue;
        }
        output << "{\"rect\": " << rect_json(Rect2(button->get_position(), button->get_size())) << ", \"hit_rect\": "
               << rect_json(Rect2(button->get_position() + button->hit_rect().position, button->hit_rect().size))
               << ", \"pressed\": " << (button->is_pressed() ? "true" : "false")
               << ", \"disabled\": " << (button->is_disabled() ? "true" : "false")
               << ", \"presses\": " << button->presses() << "}";
    }
    output << "}";
    if (state.overlay != nullptr) output << ", \"battle_overlay\": " << state.overlay->report_json();
    output << ", \"overview\": " << (state.overview ? "true" : "false")
           << ", \"shell_shown\": " << (shell_shown() ? "true" : "false");
    output << ", \"options_presses\": " << options_presses() << ", \"diagnostics\": [";
    for (std::size_t index = 0; index < state.diagnostics.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(core::format_diagnostic(state.diagnostics[index]));
    }
    output << "]}";
    return output.str();
}

void register_tactical_hud_classes() {
    GDREGISTER_CLASS(EawrTacticalHud);
    GDREGISTER_CLASS(EawrHudButton);
    GDREGISTER_CLASS(EawrUnitCards);
    GDREGISTER_CLASS(EawrProductionPanel);
    GDREGISTER_CLASS(EawrMinimap);
    GDREGISTER_CLASS(EawrAbilityButtons);
    GDREGISTER_CLASS(EawrBattleOverlay);
    GDREGISTER_CLASS(EawrOverlayButton);
    GDREGISTER_CLASS(EawrPerfOverlay);
}

} // namespace eawr::presentation::godot_backend

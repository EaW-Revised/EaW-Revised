// The focused planet's information card (docs/behaviour/galactic-planet-info-card.md, GPI-01..09).
// The nodes are plain engine controls under the organization shell, so the card is shown only while the shell is:
// focused camera, advanced map closed (GPI-01). Content and geometry come from planet_info_card.hpp.
#include "galactic_mode_internal.hpp"
#include "planet_info_card.hpp"

#include "eawr/presentation/ui/layout.hpp"

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/texture_rect.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

// A command-bar text component's face, ink and pixel height at the current screen height.
struct CardText final {
    Ref<Font> font;
    int pixels{};
    Color colour{1, 1, 1, 1};
    [[nodiscard]] bool valid() const { return font.is_valid() && pixels > 0; }
};

[[nodiscard]] data::ui::ReferenceRect reference_rect(const galactic_view::CardRect& rect) {
    return {rect.x, rect.y, rect.width, rect.height};
}

[[nodiscard]] Rect2 screen_rect(const galactic_view::CardRect& rect, const presentation::ui::ShellPlacement& placement) {
    const auto pixels = presentation::ui::shell_to_screen(reference_rect(rect), placement);
    return Rect2(static_cast<float>(pixels.x), static_cast<float>(pixels.y), static_cast<float>(pixels.width),
        static_cast<float>(pixels.height));
}

} // namespace

void GalacticMode::State::clear_planet_info_card() {
    if (!planet_info_card) return;
    while (planet_info_card->get_child_count() > 0) {
        auto* child = planet_info_card->get_child(0);
        planet_info_card->remove_child(child);
        child->queue_free();
    }
    planet_info_card->hide();
    planet_info_card_shown.reset();
}

std::optional<galactic_view::PlanetInfoCardStyle> GalacticMode::State::planet_info_card_style() const {
    namespace ui = data::ui;
    const auto* back = command_catalog.find("zoomed_back");
    const auto* anchor = fleet_anchors.find("planet_info");
    const auto* header_style = command_catalog.find("zoomed_header_text");
    if (!back || !anchor || !header_style) return std::nullopt;
    // GPI-03..06: the backdrop's authored values; the anchor is the button's resolved shell position.
    galactic_view::PlanetInfoCardStyle style;
    const auto origin = anchor->origin.value_or(assets::Vec2f{anchor->rect.x + anchor->rect.width * 0.5F,
        anchor->rect.y + anchor->rect.height * 0.5F});
    style.anchor_x = origin.x; style.anchor_y = origin.y;
    const auto size = back->vec2(ui::Field::size).value_or(ui::Vec2{});
    const auto default_offset = back->vec2(ui::Field::default_offset).value_or(ui::Vec2{});
    const auto inset = back->vec2(ui::Field::offset).value_or(ui::Vec2{});
    style.back_width = size.x; style.back_row_minimum = size.y;
    style.default_offset_x = default_offset.x; style.default_offset_y = default_offset.y;
    style.offset_x = inset.x; style.offset_y = inset.y;
    style.header_row_minimum = header_style->vec2(ui::Field::size).value_or(ui::Vec2{}).y;
    // GPI-06: the frame and glow grow over the backdrop by twice and sixteen times the display conversion ratio,
    // which is one shell unit per unit at this layer (unverified for non-square display ratios).
    style.frame_grow = 2.0F; style.glow_grow = 16.0F;
    if (style.back_width <= 0.0F) return std::nullopt;
    return style;
}

std::optional<float> GalacticMode::State::planet_info_card_left() const {
    if (!fleet_organize) return std::nullopt;
    const auto style = planet_info_card_style();
    if (!style) return std::nullopt;
    // The left edge does not depend on the rows: the layout of no rows gives the same frame left as the drawn card.
    const auto card = galactic_view::planet_info_card_layout(*style, {});
    return screen_rect(card.frame, fleet_organize->placement()).position.x;
}

void GalacticMode::State::update_planet_info_card(const presentation::ui::ShellPlacement& placement) {
    namespace ui = data::ui;
    const auto* back = command_catalog.find("zoomed_back");
    const auto* anchor = fleet_anchors.find("planet_info");
    const auto* header_style = command_catalog.find("zoomed_header_text");
    const auto* body_style = command_catalog.find("zoomed_text");
    const auto* centre_style = command_catalog.find("zoomed_center_text");
    const galactic_view::PlanetInfoCardGate gate{seat_flight().state() == galactic_view::CameraState::focused,
        fleet_organize && fleet_organize->is_visible(), map_input.gates.tutorial_locked, seat_flight().advanced_map()};
    const bool ready = gate.open() && back && anchor && header_style && body_style && hud && hud_fonts && fleet_organize && galaxy;
    if (!ready) { clear_planet_info_card(); return; }
    // The shown card is retained: no content copy, text or node work per render frame while its inputs are unchanged
    // (the displayed publication, the planet, the shell placement and the screen height).
    if (planet_info_card && planet_info_card_shown && planet_info_card_snapshot == displayed_snapshot
        && planet_info_card_planet == selected_planet && placement == planet_info_card_placement
        && planet_info_card_height == camera.height) {
        planet_info_card->show();
        return;
    }
    const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
        [this](const auto& row) { return row.id == selected_planet; });
    const auto facts = hud_facts.find(selected_planet);
    std::optional<galactic_view::PlanetInfoCardContent> content;
    if (planet != galaxy->planets.end() && facts != hud_facts.end())
        content = galactic_view::planet_info_card_content(&facts->second, *planet);
    if (!content) { clear_planet_info_card(); return; }
    if (!planet_info_card) {
        planet_info_card = memnew(Control);
        planet_info_card->set_name("planet_info_card");
        planet_info_card->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        fleet_organize->add_child(planet_info_card);
    }
    clear_planet_info_card();
    planet_info_card_shown = content;
    planet_info_card_snapshot = displayed_snapshot;
    planet_info_card_planet = selected_planet;
    planet_info_card_placement = placement;
    planet_info_card_height = camera.height;

    const float scale = static_cast<float>(placement.scale);
    if (scale <= 0.0F) return;
    const auto text_of = [&](const ui::CommandBarComponent* component) {
        CardText out;
        if (!component) return out;
        const auto face = hud_fonts->resolve({std::string(component->text(ui::Field::font_name)),
            component->integer(ui::Field::font_point_size).value_or(0)}, "english");
        out.font = hud_fonts->font(face);
        out.pixels = std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height));
        const auto ink = component->color(ui::Field::text_color).value_or(ui::Rgba8{255, 255, 255, 255});
        out.colour = Color(ink.r / 255.0F, ink.g / 255.0F, ink.b / 255.0F, ink.a / 255.0F);
        return out;
    };
    const CardText header = text_of(header_style);
    const CardText body = text_of(body_style);
    const CardText centre = centre_style ? text_of(centre_style) : header;
    if (!header.valid() || !body.valid()) return;

    const auto styled = planet_info_card_style();
    if (!styled) return;
    const galactic_view::PlanetInfoCardStyle& style = *styled;

    const auto texture_of = [&](const std::string& name) { return name.empty() ? Ref<Texture2D>{} : command_texture(name); };
    const auto texture_size = [](const Ref<Texture2D>& art) { return art.is_valid() ? art->get_size() : Vector2(); };
    const auto shell_height = [&](const float pixels) { return pixels / scale; }; // pixels to shell units

    // The shared header offsets (Encyclopedia_*_Offset) and the encyclopedia icon scale come from the same
    // authored constants as the hover encyclopedia.
    const auto& constants = encyclopedia_globals;
    const auto* icon_style = command_catalog.find("encyclopedia_icon");
    const float emblem_scale = icon_style ? icon_style->vec2(ui::Field::size).value_or(ui::Vec2{}).x : 0.0F;

    // The header (GPI-07/08). A nonzero contribution picks the badge band; the name and emblem move with it.
    const bool badge = content->population != 0;
    const Ref<Texture2D> emblem = content->emblem_owner ? fleet_faction_texture(*content->emblem_owner, true) : Ref<Texture2D>{};
    const Ref<Texture2D> band = texture_of(badge ? "e_topbar.tga" : "e_topbar2.tga");
    // #6756: the header's weather icon is a command-bar icon component: an entry of the mega texture or nothing.
    const Ref<Texture2D> weather = command_atlas_texture(content->weather_icon);
    const Vector2 emblem_px = texture_size(emblem) * emblem_scale * scale;
    const float population_shift = badge ? 0.0F : constants.population_offset;
    const float name_offset = emblem.is_valid() ? constants.name_offset - population_shift
        : badge ? constants.name_offset - (constants.icon_x_offset - constants.population_offset)
        : constants.population_offset;
    // The emblem hangs below the header band: its centre sits Encyclopedia_Icon_Y_Offset under the band's centre.
    const float emblem_reach = emblem.is_valid()
        ? style.header_row_minimum * 0.5F - constants.icon_y_offset + shell_height(emblem_px.y) * 0.5F : 0.0F;
    const float weather_reach = weather.is_valid() ? style.header_row_minimum + shell_height(texture_size(weather).y) : 0.0F;
    const std::array<float, 2> header_icons{emblem_reach, weather_reach};
    std::vector<float> rows;
    rows.push_back(galactic_view::planet_info_card_row_height(style.header_row_minimum, header_icons));

    // GPI-08: each Encyclopedia_Text entry in authored order, wrapped to the content box.
    const float content_pixels = std::max(0.0F, (style.back_width - 2.0F * style.offset_x) * scale);
    std::vector<String> paragraphs;
    for (const auto& line : content->description) paragraphs.push_back(String::utf8(line.c_str()));
    for (const auto& paragraph : paragraphs) {
        const float measured = shell_height(static_cast<float>(
            body.font->get_multiline_string_size(paragraph, HORIZONTAL_ALIGNMENT_LEFT, content_pixels, body.pixels).y));
        rows.push_back(galactic_view::planet_info_card_row_height(style.back_row_minimum, {}, measured));
    }
    // GPI-09: the separator, then the station row and the ground-structure row at icon scale 1.5.
    const Ref<Texture2D> separator = texture_of("e_line2.tga");
    const Ref<Texture2D> space_pip = content->station_max > 0 ? texture_of("i_bar_icons_space.tga") : Ref<Texture2D>{};
    const Ref<Texture2D> land_pip = content->land_max > 0 ? texture_of("i_bar_icons_land.tga") : Ref<Texture2D>{};
    constexpr float pip_scale = 1.5F;
    const bool pips = space_pip.is_valid() || land_pip.is_valid();
    const std::size_t separator_row = rows.size();
    if (pips && separator.is_valid()) rows.push_back(texture_size(separator).y);
    const std::size_t space_row = rows.size();
    if (space_pip.is_valid()) rows.push_back(texture_size(space_pip).y * pip_scale);
    const std::size_t land_row = rows.size();
    if (land_pip.is_valid()) rows.push_back(texture_size(land_pip).y * pip_scale);

    const auto layout = galactic_view::planet_info_card_layout(style, rows);
    const auto add_texture = [&](const Ref<Texture2D>& art, const Rect2& rect, const Color& tint, const bool additive = false) {
        if (art.is_null() || rect.size.x <= 0.0F || rect.size.y <= 0.0F) return;
        auto* node = memnew(TextureRect);
        node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        node->set_texture(art);
        node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        node->set_stretch_mode(TextureRect::STRETCH_SCALE);
        node->set_position(rect.position);
        node->set_size(rect.size);
        node->set_modulate(tint);
        if (additive) {
            Ref<CanvasItemMaterial> material;
            material.instantiate();
            material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
            node->set_material(material);
        }
        planet_info_card->add_child(node);
    };
    const auto add_text = [&](const String& value, const CardText& face, const Rect2& rect,
        const HorizontalAlignment alignment, const bool wrap, const Color* tint = nullptr) {
        if (value.is_empty() || !face.valid()) return;
        auto* label = memnew(Label);
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        label->set_text(value);
        label->add_theme_font_override("font", face.font);
        label->add_theme_font_size_override("font_size", face.pixels);
        label->add_theme_color_override("font_color", tint ? *tint : face.colour);
        label->set_horizontal_alignment(alignment);
        if (wrap) label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
        label->set_position(rect.position);
        label->set_size(rect.size);
        planet_info_card->add_child(label);
    };

    // GPI-06: frame, backdrop, glow, then the contents. The glow is the additive lower effect.
    // Two retail frames (a dark planet and a lava planet behind the card) show the same uniform interior, (9,13,20): the
    // additive glow sits under everything, the frame image is drawn opaque, and the backdrop keeps only its own image
    // alpha (stock 0.9; the authored 128 colour alpha is not multiplied in). The glow therefore shows only past the frame.
    const auto ink = back->color(ui::Field::color).value_or(ui::Rgba8{255, 255, 255, 255});
    const Color backdrop_tint(ink.r / 255.0F, ink.g / 255.0F, ink.b / 255.0F, 1.0F);
    const auto backgrounds = back->list(ui::Field::blank_texture_name);
    const auto alternates = back->list(ui::Field::icon_alternate_texture_name);
    const auto opaque_frame = [&](const std::string& name) -> Ref<Texture2D> {
        if (name == planet_card_frame_name && planet_card_frame_art.is_valid()) return planet_card_frame_art;
        const Ref<Texture2D> art = texture_of(name);
        Ref<Texture2D> result = art;
        if (art.is_valid()) {
            Ref<Image> image = art->get_image();
            if (image.is_valid() && !image->is_empty()) {
                image = image->duplicate();
                image->convert(Image::FORMAT_RGB8);
                result = ImageTexture::create_from_image(image);
            }
        }
        planet_card_frame_name = name; planet_card_frame_art = result;
        return result;
    };
    if (alternates.size() > 1) add_texture(texture_of(alternates[1]), screen_rect(layout.glow, placement), Color(0.5F, 0.5F, 0.5F, 0.5F), true);
    if (!alternates.empty()) add_texture(opaque_frame(alternates.front()), screen_rect(layout.frame, placement), Color(1, 1, 1, 1));
    if (!backgrounds.empty()) add_texture(texture_of(backgrounds.front()), screen_rect(layout.background, placement), backdrop_tint);

    const auto& header_row = layout.rows.front();
    const float band_height = style.header_row_minimum;
    // Retail draws the header band across the whole backdrop, flush with both frame edges (two retail frames); the
    // badge circle in its art then sits over the contribution digit, which is centred at the backdrop's left plus 11.
    const galactic_view::CardRect band_rect{layout.background.x, header_row.top() - band_height,
        layout.background.width, band_height};
    add_texture(band, screen_rect(band_rect, placement), Color(1, 1, 1, 1));
    // Name and contribution badge text on the header band, from the popup's x origin (the backdrop's left).
    const float popup_left = layout.background.x;
    const float header_top = header_row.top();
    if (badge) {
        const Color badge_ink = content->population > 0 ? Color(0, 1, 0, 1) : Color(1, 215.0F / 255.0F, 56.0F / 255.0F, 1);
        add_text(String::utf8(galactic_view::planet_info_card_population_text(*content).c_str()), centre,
            screen_rect({popup_left, header_top - band_height, constants.population_offset * 2.0F, band_height}, placement),
            HORIZONTAL_ALIGNMENT_CENTER, false, &badge_ink);
    }
    // The income (GPI-07) is not drawn: the retail frame of an owned, positive-income planet shows none, and the
    // cost text component authors no layer.
    const float cost_right = popup_left + constants.cost_offset;
    const float name_left = popup_left + name_offset;
    add_text(String::utf8(content->name.c_str()), header,
        screen_rect({name_left, header_top - band_height, std::max(1.0F, cost_right - name_left), band_height}, placement),
        HORIZONTAL_ALIGNMENT_LEFT, false);
    if (emblem.is_valid()) {
        const float centre_x = popup_left + constants.icon_x_offset - population_shift;
        const float centre_y = header_top - band_height * 0.5F + constants.icon_y_offset;
        const float half_w = shell_height(emblem_px.x) * 0.5F, half_h = shell_height(emblem_px.y) * 0.5F;
        add_texture(emblem, screen_rect({centre_x - half_w, centre_y - half_h, half_w * 2.0F, half_h * 2.0F}, placement), Color(1, 1, 1, 1));
    }
    if (weather.is_valid()) {
        // The weather icon hangs from the end of the header text row, against the content's right edge, at scale 1.
        const Vector2 weather_size = texture_size(weather) * scale;
        const float width = shell_height(weather_size.x), height = shell_height(weather_size.y);
        add_texture(weather, screen_rect({layout.content_left + layout.content_width - width, header_top - band_height - height, width, height},
            placement), Color(1, 1, 1, 1));
    }
    for (std::size_t index = 0; index < paragraphs.size(); ++index)
        add_text(paragraphs[index], body, screen_rect(layout.rows[1 + index], placement), HORIZONTAL_ALIGNMENT_LEFT, true);
    if (pips && separator.is_valid())
        add_texture(separator, screen_rect(layout.rows[separator_row], placement), Color(1, 1, 1, 1));
    // The pips advance by their rendered width; filled ones take the owner's colour, the rest the same colour dimmed
    // (the empty-pip look is not sourced: unverified).
    const Color owner(content->pip_colour.r, content->pip_colour.g, content->pip_colour.b, 1.0F);
    const Color empty(owner.r * 0.3F, owner.g * 0.3F, owner.b * 0.3F, 1.0F);
    const auto add_pips = [&](const Ref<Texture2D>& art, const std::int64_t maximum, const std::int64_t filled, const std::size_t row) {
        if (art.is_null()) return;
        const float width = shell_height(texture_size(art).x * pip_scale * scale);
        const auto& at = layout.rows[row];
        for (std::int64_t pip = 0; pip < maximum; ++pip)
            add_texture(art, screen_rect({at.x + static_cast<float>(pip) * width, at.y, width, at.height}, placement),
                pip < filled ? owner : empty);
    };
    add_pips(space_pip, content->station_max, content->station_filled, space_row);
    add_pips(land_pip, content->land_max, content->land_filled, land_row);
    planet_info_card->show();
}

} // namespace eawr::presentation::godot_backend

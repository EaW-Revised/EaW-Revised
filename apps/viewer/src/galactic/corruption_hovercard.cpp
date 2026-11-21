#include "galactic_mode_internal.hpp"
#include "eawr/core/text.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <pugixml.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace godot;
namespace eawr::presentation::godot_backend {
void GalacticMode::State::configure_corruption_hovercard(const pugi::xml_document& document) {
    auto& card = corruption_hovercard;
    const auto constants = document.document_element();
    const auto token = [&](const char* tag) {
        std::istringstream stream(constants.child(tag).text().as_string());
        std::string value; stream >> value; return value;
    };
    card.backdrop = token("Corruption_Encyclopedia_Backdrop");
    card.header = token("Corruption_Encyclopedia_Header");
    card.money_icon = token("Corruption_Encyclopedia_Money_Icon");
    card.left_edge = static_cast<float>(constants.child("Corruption_Encyclopedia_Left_Edge").text().as_int());
    card.spacing = static_cast<float>(constants.child("Corruption_Encyclopedia_Spacing").text().as_int());
}

void GalacticMode::State::show_corruption_hovercard(const sim::galactic::PlanetId planet) {
    namespace ui = data::ui;
    auto& card = corruption_hovercard;
    const auto publication = card.fixture ? card.fixture : displayed_snapshot;
    const auto* menu = card.fixture ? publication->corruption_menu() : publication->corruption_information(planet);
    const auto facts = hud_facts.find(planet);
    const auto* back = command_catalog.find("encyclopedia_back");
    const auto* header = command_catalog.find("encyclopedia_header_text");
    const auto* body = command_catalog.find("encyclopedia_text");
    const auto* icon_style = command_catalog.find("encyclopedia_icon");
    if (!menu || menu->planet != planet || menu->choices.empty() || facts == hud_facts.end() || facts->second.corrupted
        || !back || !header || !body || !icon_style || !hud_fonts || !command_bar || !battle_text) return;
    const auto placement = command_bar->placement();
    const float scale = static_cast<float>(placement.scale);
    if (scale <= 0) return;
    const auto point = presentation::ui::shell_to_screen({encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, placement);
    // Retain nodes across frames: the same publication, planet, viewport and anchor need no signature at all.
    const auto same_owner = [](const auto& a, const auto& b) { return !a.owner_before(b) && !b.owner_before(a); };
    const bool unchanged = card.panel && card.planet == planet && same_owner(card.source, publication)
        && card.key_width == static_cast<float>(camera.width) && card.key_height == static_cast<float>(camera.height)
        && card.key_scale == scale && card.key_x == static_cast<float>(point.x) && card.key_y == static_cast<float>(point.y);
    if (unchanged) { card.panel->set_modulate(Color(1,1,1,static_cast<float>(encyclopedia_timing.alpha))); card.panel->show(); return; }
    // Only the copied display values, placement and viewport rebuild the nodes across unrelated publications.
    std::ostringstream signature;
    signature << planet << ':' << camera.width << ':' << camera.height << ':' << scale;
    const auto append_types = [&](const auto& types) { for (const auto type : types) signature << ',' << type; signature << ';'; };
    for (const auto& row : menu->choices) {
        const auto& choice = row.prepared.choice;
        signature << ':' << row.slot << ',' << static_cast<unsigned>(choice.type) << ',' << choice.credits.raw()
            << ',' << row.available_units << ',' << row.completed_missions << ',' << row.required_unit_type
            << ',' << row.affordable << ',' << row.units_satisfied << ',' << row.missions_satisfied << ',' << row.planets_satisfied;
        if (row.live_choice) {
            const auto& live = *row.live_choice;
            signature << ',' << live.unit_count << ',' << live.mission_count << ',' << static_cast<unsigned>(live.required_mission_type);
            append_types(live.unit_types); append_types(live.planet_types);
        } else {
            const auto& required = choice.requirements;
            signature << ',' << required.unit_count << ',' << required.mission_count << ',' << static_cast<unsigned>(required.mission_type);
            append_types(required.unit_types); append_types(required.planet_types);
        }
    }
    signature << ':' << point.x << ':' << point.y;
    if (!card.panel) {
        card.panel = memnew(Control); card.panel->set_name("CorruptionInformationCard");
        card.panel->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); card.panel->set_z_index(9); hud->add_child(card.panel);
    }
    const auto value = signature.str();
    card.source = publication; card.key_width = static_cast<float>(camera.width); card.key_height = static_cast<float>(camera.height);
    card.key_scale = scale; card.key_x = static_cast<float>(point.x); card.key_y = static_cast<float>(point.y);
    if (card.signature != value) {
        while (card.panel->get_child_count()) {
            auto* child = card.panel->get_child(0); card.panel->remove_child(child); child->queue_free();
        }
        card.signature = value; card.planet = planet;
        const auto extent = back->vec2(ui::Field::size).value_or(ui::Vec2{});
        const auto padding = back->vec2(ui::Field::offset).value_or(ui::Vec2{});
        const float width = extent.x * scale;
        const float inset = padding.x * scale;
        const float content_width = std::max(0.0F, width - 2 * inset);
        if (width <= 0 || content_width <= 0) { card.signature.clear(); card.source.reset(); return; }
        struct Face { Ref<Font> font; int pixels{}; Color ink; };
        const auto face = [&](const ui::CommandBarComponent& component) {
            const auto resolved = hud_fonts->resolve({std::string(component.text(ui::Field::font_name)),
                component.integer(ui::Field::font_point_size).value_or(0)}, "english");
            const auto ink = component.color(ui::Field::text_color).value_or(ui::Rgba8{255,255,255,255});
            return Face{hud_fonts->font(resolved), std::max(1, presentation::ui::font_pixel_height(resolved.point_size, camera.height)),
                Color(ink.r / 255.0F, ink.g / 255.0F, ink.b / 255.0F, ink.a / 255.0F)};
        };
        const auto title_face = face(*header), body_face = face(*body), count_face = face(*icon_style);
        if (title_face.font.is_null() || body_face.font.is_null() || count_face.font.is_null()) { card.signature.clear(); card.source.reset(); return; }
        const auto text = [&](const std::string& name, const std::string& words, const Face& style, Rect2 rect,
            const Color ink, const HorizontalAlignment alignment = HORIZONTAL_ALIGNMENT_LEFT, bool wrap = false) {
            auto* label = memnew(Label); label->set_name(String::utf8(name.c_str()));
            label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); label->set_text(String::utf8(words.c_str()));
            label->add_theme_font_override("font", style.font); label->add_theme_font_size_override("font_size", style.pixels);
            label->add_theme_color_override("font_color", ink); label->set_horizontal_alignment(alignment);
            label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
            if (wrap) label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
            if (&style == &count_face && icon_style->flag(ui::Field::text_outline)) {
                label->add_theme_color_override("font_outline_color", Color(0,0,0,1));
                label->add_theme_constant_override("outline_size", 1);
            }
            label->set_position(rect.position); label->set_size(rect.size); card.panel->add_child(label);
            return label;
        };
        const auto texture_size = [&](const std::string& name) {
            const auto art = command_texture(name); return art.is_valid() ? art->get_size() * scale : Vector2();
        };
        const auto image = [&](const std::string& name, const std::string& art, const Rect2 rect, const Color tint = Color(1,1,1,1), bool additive = false) {
            const auto texture = command_texture(art); if (texture.is_null()) return;
            auto* node = memnew(TextureRect); node->set_name(String::utf8(name.c_str()));
            node->set_meta("art", String::utf8(art.c_str())); node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE); node->set_texture(texture);
            node->set_position(rect.position); node->set_size(rect.size); node->set_modulate(tint);
            if (additive) { Ref<CanvasItemMaterial> material; material.instantiate(); material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD); node->set_material(material); }
            card.panel->add_child(node);
        };
        struct Cell { std::string name, art, caption; float centre{}; bool satisfied{true}; bool price{}; };
        struct Row { std::vector<Cell> cells; float height{}, type_height{}; bool complete{true}; unsigned slot{}; };
        std::vector<Row> rows;
        const auto type_icon = [&](sim::galactic::corruption_actions::Type type) {
            const auto ordinal = static_cast<std::size_t>(type);
            return ordinal && ordinal <= corruption_requirement_icons.size() ? corruption_requirement_icons[ordinal - 1] : std::string{};
        };
        for (const auto& choice : menu->choices) {
            Row row; row.slot = choice.slot;
            row.complete = choice.units_satisfied && choice.missions_satisfied && choice.planets_satisfied;
            row.cells.push_back({"choice_" + std::to_string(row.slot), type_icon(choice.prepared.choice.type), {}, 0, row.complete, false});
            float column = card.left_edge;
            const auto cell = [&](std::string name, const std::string& art, const std::string& caption, bool satisfied, bool price = false) {
                if (art.empty()) return;
                row.cells.push_back({std::move(name), art, caption, column, satisfied, price}); column += card.spacing;
            };
            const auto suffix = std::to_string(row.slot);
            if (!card.money_icon.empty()) cell("cost_" + suffix, card.money_icon,
                std::to_string(choice.prepared.choice.credits.nearest_even_to_integer()), choice.affordable, true);
            const auto& required = choice.prepared.choice.requirements;
            const auto mission = choice.live_choice ? choice.live_choice->required_mission_type : required.mission_type;
            const auto missions = choice.live_choice ? choice.live_choice->mission_count : required.mission_count;
            if (mission != sim::galactic::corruption_actions::Type::none) cell("mission_" + suffix, type_icon(mission),
                std::to_string(choice.completed_missions) + "/" + std::to_string(missions), choice.missions_satisfied);
            const auto units = choice.live_choice ? choice.live_choice->unit_count : required.unit_count;
            if (choice.required_unit_type) {
                const auto name = production_names.find(choice.required_unit_type);
                const auto art = name == production_names.end() ? production_art.end() : production_art.find(name->second);
                if (art != production_art.end()) cell("units_" + suffix, art->second.icon,
                    std::to_string(std::min(choice.available_units, static_cast<std::size_t>(units))) + "/" + std::to_string(units), choice.units_satisfied);
            }
            const bool planets = choice.live_choice ? !choice.live_choice->planet_types.empty() : !required.planet_types.empty();
            // WGCI-3: the ninth requirement art entry is the generic planet, after the eight corruption types.
            if (planets && corruption_requirement_icons.size() > 8) cell("planets_" + suffix, corruption_requirement_icons[8],
                choice.planets_satisfied ? "1/1" : "0/1", choice.planets_satisfied);
            // WGCI-3: the debug build places the completion arrow at the fixed 64-unit centre.
            row.cells.push_back({"marker_" + suffix, corruption_menu.satisfaction_textures[row.complete ? 1 : 0], {}, 64, true, false});
            row.type_height = texture_size(row.cells.front().art).y;
            row.height = std::max(extent.y * scale, texture_size(card.backdrop).y);
            for (const auto& item : row.cells) {
                auto size = texture_size(item.art);
                if (item.name.starts_with("units_") && row.type_height > 0) size.y = std::min(size.y, row.type_height);
                row.height = std::max(row.height, size.y);
                // The price count sits below the credit glyph inside the row: grow the row, never push the count up.
                if (item.price) row.height = std::max(row.height, size.y + 1.5F * 2.0F * static_cast<float>(count_face.pixels));
            }
            rows.push_back(std::move(row));
        }
        const float title_height = std::max(header->vec2(ui::Field::size).value_or(ui::Vec2{}).y * scale, static_cast<float>(title_face.pixels));
        const float header_height = std::max(texture_size(card.header).y, extent.y * scale);
        const auto footer = battle_text("TEXT_TOOLTIP_CLICK_FOR_MORE_INFO");
        const float footer_height = std::max(extent.y * scale, static_cast<float>(body_face.font->get_multiline_string_size(
            String::utf8(footer.c_str()), HORIZONTAL_ALIGNMENT_LEFT, content_width, body_face.pixels).y));
        // WGCI-3 and GPI-05: shared row sizing, with natural icon sizes and the authored text-row minimum.
        const auto separator = texture_size("e_line2.tga");
        float height = title_height + header_height + separator.y + footer_height + padding.y * scale;
        for (const auto& row : rows) height += row.height;
        card.panel->set_size(Vector2(width, height));
        const auto backgrounds = back->list(ui::Field::blank_texture_name);
        const auto alternates = back->list(ui::Field::icon_alternate_texture_name);
        if (alternates.size() > 1) image("glow", alternates[1], Rect2(-8 * scale, -8 * scale, width + 16 * scale, height + 16 * scale), Color(0.5F,0.5F,0.5F,0.5F), true);
        if (!alternates.empty()) image("frame", alternates.front(), Rect2(-scale, -scale, width + 2 * scale, height + 2 * scale));
        if (!backgrounds.empty()) image("background", backgrounds.front(), Rect2(0,0,width,height));
        text("title", battle_text("TEXT_CORRUPTION_ENCYCLOPEDIA_TITLE"), title_face, Rect2(inset,0,content_width,title_height), title_face.ink);
        float y = title_height;
        const auto centered_image = [&](const std::string& name, const std::string& art, float x, float centre_y, const Color tint, float max_height) {
            auto size = texture_size(art);
            if (max_height > 0 && size.y > max_height) size *= max_height / size.y;
            const float half = std::min(size.x * 0.5F, content_width * 0.5F);
            const float centre_x = inset + std::clamp(x * scale, half, content_width - half);
            image(name, art, Rect2(Vector2(centre_x, centre_y) - size * 0.5F, size), tint);
            return Vector2(centre_x, centre_y);
        };
        // WGCI-3: header/backdrop centres are code-selected at 131 units; the requirement heading at 154.
        image("header", card.header, Rect2(Vector2(131 * scale, y + header_height * 0.5F) - texture_size(card.header) * 0.5F, texture_size(card.header)));
        Color corruption_colour = body_face.ink;
        if (route_styles && content) for (const auto& [id, faction] : content->factions) {
            static_cast<void>(id);
            if (core::iequals(faction.faction, route_styles->corrupt_faction)) corruption_colour = Color(faction.secondary.r, faction.secondary.g, faction.secondary.b);
        }
        text("requirements", battle_text("TEXT_CORRUPTION_REQUIREMENT"), body_face,
            Rect2(inset + 154 * scale - content_width * 0.5F, y, content_width, header_height), corruption_colour, HORIZONTAL_ALIGNMENT_CENTER);
        y += header_height;
        const auto caption_offset = icon_style->vec2(ui::Field::text_offset).value_or(ui::Vec2{});
        for (const auto& row : rows) {
            const auto suffix = std::to_string(row.slot);
            image("backdrop_" + suffix, card.backdrop, Rect2(Vector2(131 * scale, y + row.height * 0.5F) - texture_size(card.backdrop) * 0.5F, texture_size(card.backdrop)));
            for (const auto& cell : row.cells) {
                const auto tint = cell.satisfied || cell.price ? Color(1,1,1) : Color(148.0F/255.0F,148.0F/255.0F,148.0F/255.0F);
                const auto centre = centered_image(cell.name + "_icon", cell.art, cell.centre, y + row.height * 0.5F, tint,
                    cell.name.starts_with("units_") ? row.type_height : 0.0F);
                if (!cell.caption.empty()) {
                    const float caption_width = std::max(std::abs(card.spacing) * scale, static_cast<float>(count_face.pixels));
                    const float box_height = 2.0F * count_face.pixels;
                    float caption_top = centre.y - caption_offset.y * scale - count_face.pixels;
                    if (cell.price) {
                        // The credit glyph is wide and its count must not sit on it: start the count at the glyph's
                        // lower edge; the row was grown to hold it (WGCI-3 gives no price-specific offset).
                        caption_top = centre.y + texture_size(cell.art).y * 0.5F - 0.25F * box_height;
                    }
                    text(cell.name, cell.caption, count_face, Rect2(centre.x + caption_offset.x * scale - caption_width * 0.5F,
                        caption_top, caption_width, box_height),
                        cell.satisfied ? Color(1,1,1) : Color(1,0,0), HORIZONTAL_ALIGNMENT_CENTER);
                }
            }
            y += row.height;
        }
        image("separator", "e_line2.tga", Rect2(Vector2(width * 0.5F, y + separator.y * 0.5F) - separator * 0.5F, separator));
        y += separator.y;
        text("footer", footer, body_face, Rect2(inset,y,content_width,footer_height), corruption_colour, HORIZONTAL_ALIGNMENT_LEFT, true);
        card.panel->set_position(Vector2(std::clamp(static_cast<float>(point.x), 0.0F, std::max(0.0F, static_cast<float>(camera.width) - width)),
            std::clamp(static_cast<float>(point.y) - height, 0.0F, std::max(0.0F, static_cast<float>(camera.height) - height))));
    }
    card.panel->set_modulate(Color(1,1,1,static_cast<float>(encyclopedia_timing.alpha))); card.panel->show();
}
} // namespace eawr::presentation::godot_backend

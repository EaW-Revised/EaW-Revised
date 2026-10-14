#include "galactic_mode_internal.hpp"
#include "black_market_hud_fixture.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/input_routing.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/loading_screen.hpp"
#include "eawr/data/tag_trace.hpp"
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <algorithm>
#include <cctype>
#include <ostream>
#include <sstream>
using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::string upper(std::string value) {
    for (auto& character : value) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return value;
}
std::string first(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto values = component.list(field);
    return values.empty() ? std::string{} : values.front();
}
}
bool GalacticMode::State::build_black_market_hud() {
    if (!filesystem || !hud || !hud_fonts) return true;
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::black_market_fixture(options.hud_fixture))
        market_menu.fixture = galactic_view::black_market_fixture_snapshot(galaxy->local.player, options.hud_fixture);
    market_menu.blocker = memnew(Control);
    hud->add_child(market_menu.blocker);
    market_menu.blocker->set_name("black_market_modal");
    market_menu.blocker->add_to_group(EawrUiModalLayer::group);
    market_menu.blocker->set_z_index(10);
    market_menu.blocker->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    // WGUI-47: menus lock world selection, including empty space and disabled rows.
    market_menu.blocker->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    market_menu.blocker->hide();
    for (std::size_t kind = 0; kind < 1; ++kind) {
        auto& surface = market_menu.surfaces[kind];
        const auto shell_name = "market_frame";
        const auto* shell = command_catalog.find(shell_name);
        if (!shell) continue; // Missing authored menus stay unavailable.
        auto loaded = data::ui::load_shell_anchors(*filesystem, shell->text(data::ui::Field::model_name));
        if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
        surface.anchors = std::move(loaded).value().shell;
        surface.panel = memnew(EawrTacticalHud); market_menu.blocker->add_child(surface.panel);
        EawrTacticalHud::Setup setup;
        setup.view = presentation::ui::hud_view_model(surface.anchors, command_catalog,
            presentation::ui::alt_variant(command_faction), presentation::ui::vfs_shell_masks(*filesystem),
            presentation::ui::ShellOrigin::centre);
        const auto look = presentation::ui::hud_shell(surface.anchors, command_catalog, command_faction);
        for (const auto& mesh : look.meshes) {
            const auto path = presentation::ui::vfs_standalone_textures(*filesystem)(mesh.texture);
            if (!path) { failure = "black-market shell texture is absent: " + mesh.texture; return false; }
            auto texture = assets::load_texture(*filesystem, *path);
            if (!texture) { failure = core::format_diagnostic(texture.error()); return false; }
            std::string why;
            auto pixels = texture_image(texture.value(), why);
            if (pixels.is_null()) { failure = why; return false; }
            setup.meshes.push_back({mesh.blend, ImageTexture::create_from_image(pixels), mesh.triangles});
        }
        surface.panel->setup(std::move(setup), nullptr);
        surface.close_name = "market_close";
        for (const auto& anchor : surface.anchors.anchors()) {
            const auto* component = command_catalog.find(anchor.name);
            if (!component || !anchor.alt.shown_for(presentation::ui::alt_variant(command_faction))) continue;
            const bool choice = component->in_group("BlackMarket");
            const bool close = anchor.name == surface.close_name;
            if (choice || close) {
                auto* button = memnew(EawrHudButton);
                button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
                button->set_name(String::utf8(anchor.name.c_str()));
                button->set_texture_normal(command_texture(first(*component, data::ui::Field::icon_texture_name)));
                button->set_texture_hover(command_texture(first(*component, data::ui::Field::mouse_over_texture_name)));
                button->set_texture_pressed(command_texture(first(*component, data::ui::Field::selected_texture_name)));
                button->set_texture_disabled(command_texture(first(*component, data::ui::Field::disabled_texture_name)));
                surface.panel->add_child(button);
                if (close) {
                    surface.close = button; button->set_action([this] { close_black_market(); });
                } else {
                    button->set_order_icon({}, false);
                    surface.choices.emplace_back(anchor.name, button);
                }
            }
            if (component->text(data::ui::Field::font_name).empty()) continue;
            auto* label = memnew(Label); label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            label->set_name(String::utf8((anchor.name + "_text").c_str()));
            const auto face = hud_fonts->resolve({std::string(component->text(data::ui::Field::font_name)),
                component->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
            label->add_theme_font_override("font", hud_fonts->font(face));
            const auto tint = component->color(data::ui::Field::text_color)
                .value_or(component->color(data::ui::Field::color).value_or(data::ui::Rgba8{255,255,255,255}));
            label->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
            label->set_horizontal_alignment(component->flag(data::ui::Field::left_justified)
                ? HORIZONTAL_ALIGNMENT_LEFT : HORIZONTAL_ALIGNMENT_CENTER);
            surface.panel->add_child(label); surface.labels.emplace(anchor.name, label);
        }
        std::sort(surface.choices.begin(), surface.choices.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t index = 0; index < surface.choices.size(); ++index)
            surface.choices[index].second->set_action([this, index] { choose_black_market(index); });
        surface.panel->hide();
    }
    return build_black_market_encyclopedia();
}


bool GalacticMode::State::build_black_market_encyclopedia() {
    // WGEP-138: use the economy provider's XML traversal/ordinal identity.
    // No fixed item list; metadata follows the same VFS overlay as preparation.
    auto document = data::load_document(*filesystem, "Data/XML/BlackMarketItems.xml");
    if (!document) { failure = core::format_diagnostic(document.error()); return false; }
    const auto trim = [](std::string value) {
        const auto start = value.find_first_not_of(" \t\r\n");
        return start == std::string::npos ? std::string{} : value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
    };
    const auto field = [&](const data::XmlNode& node, std::string_view tag) {
        for (const auto& child : node.children) if (upper(child.name) == upper(std::string(tag))) {
            data::tag_trace::used(&child); return trim(child.raw_text);
        }
        return std::string{};
    };
    std::uint32_t id = 1;
    for (const auto& node : document.value().root.children) {
        if (upper(node.name) != "ITEM") continue;
        BlackMarketArt art;
        art.icon = field(node, "Icon_Name");
        art.title = battle_text(field(node, "Localized_Name"));
        art.description = battle_text(field(node, "Localized_Description"));
        auto references = field(node, "Used_By_Units");
        std::replace(references.begin(), references.end(), ',', ' ');
        std::istringstream units(references);
        // WGEP-143: unresolved/empty entries leave their authored slot blank.
        for (std::size_t slot = 0; slot < art.units.size(); ++slot) {
            std::string unit; if (!(units >> unit)) break;
            const auto found = std::find_if(production_art.begin(), production_art.end(),
                [&](const auto& entry) { return upper(entry.first) == upper(unit); });
            if (found != production_art.end()) art.units[slot] = found->second.icon;
        }
        market_menu.art.emplace(id++, std::move(art));
    }
    market_menu.bindings.resize(market_menu.surfaces[0].choices.size());
    market_menu.timing.delay = encyclopedia_timing.delay;
    market_menu.timing.minimum = encyclopedia_timing.minimum;
    market_menu.timing.fade_rate = encyclopedia_timing.fade_rate;
    auto* pane = memnew(TextureRect); market_menu.encyclopedia = pane;
    pane->set_name("BlackMarketEncyclopedia"); pane->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    pane->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    market_menu.blocker->add_child(pane); pane->hide();
    if (const auto* back = command_catalog.find("encyclopedia_back"))
        pane->set_texture(command_texture(first(*back, data::ui::Field::blank_texture_name)));
    market_menu.title = memnew(Label); market_menu.body = memnew(Label);
    market_menu.body->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    for (auto* label : {market_menu.title, market_menu.body}) {
        pane->add_child(label); label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    }
    market_menu.icon = memnew(TextureRect);
    pane->add_child(market_menu.icon);
    for (auto*& icon : market_menu.units) { icon = memnew(TextureRect); pane->add_child(icon); }
    for (auto* icon : {market_menu.icon, market_menu.units[0], market_menu.units[1], market_menu.units[2]}) {
        icon->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
    }
    return true;
}
void GalacticMode::State::close_black_market(bool cancel) {
    if (!market_menu.blocker || !market_menu.blocker->is_visible_in_tree()) return;
    if (cancel && !market_menu.fixture && game && market_menu.facts && market_menu.facts->preparation) {
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
            sim::galactic::CancelGalacticAbility{market_menu.facts->buyer, market_menu.facts->preparation}, {}});
        ++submitted_orders;
    }
    static_cast<void>(map_input.hud_event());
    market_menu.closed = true; ++market_menu.closes;
    market_menu.facts.reset();
    for (auto& binding : market_menu.bindings) binding.reset();
    market_menu.timing.candidate = market_menu.timing.shown = 0;
    market_menu.timing.hovered_seconds = market_menu.timing.remaining = market_menu.timing.alpha = 0;
    market_menu.shown_item = 0;
    market_menu.title->set_text({}); market_menu.body->set_text({}); market_menu.icon->set_texture({});
    for (auto* icon : market_menu.units) icon->set_texture({});
    for (const auto& [name, label] : market_menu.surfaces[0].labels) {
        static_cast<void>(name); label->set_text({}); label->hide();
    }
    market_menu.encyclopedia->hide(); market_menu.blocker->hide();
}
void GalacticMode::State::choose_black_market(std::size_t index) {
    if ((!market_menu.fixture && !game) || market_menu.closed || !market_menu.enabled || !market_menu.facts
        || index >= market_menu.bindings.size() || !market_menu.bindings[index]) return;
    const auto offer = *market_menu.bindings[index];
    if (offer.owned || !offer.affordability_available || !offer.affordable) return;
    const auto& menu = *market_menu.facts;
    if (!market_menu.fixture && !menu.preparation) return;
    // WGEP-142: copied index/identity/price/absence, never a debit or unlock.
    // The host checks this retained preparation; presentation never owns the price.
    market_menu.accepted = BlackMarketSubmission{menu.buyer, menu.planet, offer.index, offer.item, offer.price, menu.absence};
    if (!market_menu.fixture) {
        sim::galactic::PreparedAbility prepared_order{menu.preparation, menu.buyer, menu.planet,
            sim::galactic::AbilityKind::black_market, offer.index, offer.item, offer.price, menu.absence, offer.owned};
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::ExecutePreparedAbility{prepared_order}, {}});
        ++submitted_orders;
    }
    ++market_menu.choices_accepted;
    close_black_market(false); --market_menu.closes; // submission has the same hide-and-clear route
}
void GalacticMode::State::update_black_market_hud() {
    if (!market_menu.blocker || !displayed_snapshot) return;
    const auto publication = market_menu.fixture ? market_menu.fixture : displayed_snapshot;
    const bool changed = !market_menu.publication || publication->black_market_publication() != market_menu.publication->black_market_publication();
    const bool resized = camera.width != market_menu.width || camera.height != market_menu.height;
    const bool enabled = map_input.gates.hud_enabled && !map_input.gates.tutorial_locked;
    if (!changed && !resized && enabled == market_menu.enabled) return;
    market_menu.width = camera.width; market_menu.height = camera.height; market_menu.enabled = enabled;
    if (changed) {
        market_menu.publication = publication;
        const auto menus = publication->black_market_menus();
        const auto found = std::find_if(menus.begin(), menus.end(), [&](const auto& row) { return row.player == galaxy->local.player; });
        if (found != menus.end()) {
            // Balance refresh must not reopen a submitted/cancelled activation.
            std::uint64_t digest = 14695981039346656037ULL;
            for (const auto& offer : found->offers) for (const auto value : {
                static_cast<std::uint64_t>(offer.index), static_cast<std::uint64_t>(offer.item),
                static_cast<std::uint64_t>(offer.price.raw()), static_cast<std::uint64_t>(offer.owned)})
                digest = (digest ^ value) * 1099511628211ULL;
            digest = (digest ^ found->preparation) * 1099511628211ULL;
            const std::array<std::uint64_t, 4> identity{found->buyer, found->planet,
                static_cast<std::uint64_t>(found->absence.raw()), digest};
            if (!market_menu.activation || *market_menu.activation != identity) {
                market_menu.closed = false; market_menu.activation = std::move(identity);
            }
            if (!market_menu.closed) market_menu.facts = *found;
        } else { market_menu.facts.reset(); market_menu.activation.reset(); market_menu.closed = false; }
        ++market_menu.rebuilds;
    }
    auto& surface = market_menu.surfaces[0];
    const bool available = market_menu.facts && !market_menu.facts->offers.empty() && surface.panel && surface.close;
    market_menu.blocker->set_visible(available && enabled && !market_menu.closed);
    if (!surface.panel) return;
    surface.panel->set_visible(available);
    const auto placement = surface.panel->placement();
    const auto position = [&](Control* control, std::string_view name) {
        const auto* anchor = surface.anchors.find(name); if (!anchor) return;
        const auto rect = presentation::ui::shell_to_screen(anchor->rect, placement);
        control->set_position(Vector2(static_cast<float>(rect.x), static_cast<float>(rect.y)));
        control->set_size(Vector2(static_cast<float>(rect.width), static_cast<float>(rect.height)));
    };
    for (const auto& [name, label] : surface.labels) {
        position(label, name); label->set_text({}); label->hide();
        const auto* component = command_catalog.find(name);
        const auto offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
        label->set_position(label->get_position() + Vector2(offset.x, offset.y) * static_cast<float>(placement.scale));
        label->add_theme_font_size_override("font_size", std::max(1, presentation::ui::font_pixel_height(
            component->integer(data::ui::Field::font_point_size).value_or(0), camera.height)));
        const auto tint = component->color(data::ui::Field::text_color)
            .value_or(component->color(data::ui::Field::color).value_or(data::ui::Rgba8{255,255,255,255}));
        label->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
    }
    for (std::size_t slot = 0; slot < surface.choices.size(); ++slot) {
        auto* button = surface.choices[slot].second; position(button, surface.choices[slot].first);
        button->set_hit_rect({{}, button->get_size()}); button->hide();
        market_menu.bindings[slot].reset();
    }
    if (surface.close) {
        position(surface.close, surface.close_name); surface.close->set_hit_rect({{}, surface.close->get_size()});
        surface.close->set_disabled(!enabled);
    }
    if (!available) return;
    const auto text = [&](const std::string& name, const std::string& value) {
        if (const auto found = surface.labels.find(name); found != surface.labels.end()) {
            found->second->set_text(String::utf8(value.c_str())); found->second->set_visible(!value.empty());
        }
    };
    text("market_title00", battle_text("TEXT_BLACK_MARKET_TITLE"));
    text("market_close", battle_text("TEXT_BUTTON_CANCEL"));
    const auto planet = hud_facts.find(market_menu.facts->planet);
    if (planet != hud_facts.end()) text("market_title01", planet->second.display_name);
    // WGEP-109: header tech/faction awaits copied header publication (legacy #1130).
    const auto visited = std::min(surface.choices.size(), market_menu.facts->offers.size());
    market_menu.omitted = market_menu.facts->offers.size() - visited;
    for (std::size_t index = 0; index < visited; ++index) {
        const auto& offer = market_menu.facts->offers[index];
        // GHUD-BM01: bounded authored slots; first prepared offer wins a collision.
        // Out-of-range/colliding offers are omitted only here, never an economy gate.
        if (offer.index >= surface.choices.size() || offer.tech < 1
            || static_cast<std::size_t>(offer.tech) > surface.choices.size()) { ++market_menu.omitted; continue; }
        const auto slot = static_cast<std::size_t>(offer.tech - 1);
        if (market_menu.bindings[slot]) { ++market_menu.omitted; continue; }
        const auto art = market_menu.art.find(offer.item);
        if (art == market_menu.art.end()) { ++market_menu.omitted; continue; }
        market_menu.bindings[slot] = offer;
        auto* button = surface.choices[slot].second; button->show();
        button->set_icon_texture(command_texture(art->second.icon));
        button->set_disabled(!enabled || offer.owned || !offer.affordability_available || !offer.affordable || !market_menu.fixture);
        const auto base = 4 + slot * 2;
        text("market_title" + (base < 10 ? std::string("0") : std::string{}) + std::to_string(base), art->second.title);
        const auto price = offer.price.raw(); const auto scale = sim::math::Fixed::scale;
        text("market_title" + (base + 1 < 10 ? std::string("0") : std::string{}) + std::to_string(base + 1),
            offer.owned ? battle_text("TEXT_BLACK_MARKET_PURCHASED") : "$ " + std::to_string(price / scale + (price % scale >= scale / 2 ? 1 : 0)));
        if (offer.owned || !offer.affordability_available || !offer.affordable) {
            const auto name = "market_title" + (base + 1 < 10 ? std::string("0") : std::string{}) + std::to_string(base + 1);
            if (const auto label = surface.labels.find(name); label != surface.labels.end())
                label->second->add_theme_color_override("font_color", Color(160/255.0F,160/255.0F,160/255.0F));
        }
    }
    market_menu.shown_item = 0; // refresh text/layout on the next admitted hover service
}
void GalacticMode::State::service_black_market_encyclopedia() {
    if (!market_menu.encyclopedia) return;
    const auto now = Time::get_singleton()->get_ticks_msec();
    const auto elapsed = market_menu.wall_time ? static_cast<double>(now - market_menu.wall_time) / 1000.0 : 0.0;
    market_menu.wall_time = now;
    const bool admitted = market_menu.blocker->is_visible_in_tree() && market_menu.facts.has_value();
    std::uint32_t hovered{};
    const auto* pointer = hud->get_viewport()->gui_get_hovered_control();
    for (std::size_t slot = 0; slot < market_menu.bindings.size(); ++slot)
        if (pointer == market_menu.surfaces[0].choices[slot].second && market_menu.bindings[slot]) hovered = market_menu.bindings[slot]->item;
    // WGUI-80 / hover lifetime: expiry retains the shown content through its fade.
    market_menu.timing.service(hovered, elapsed, admitted);
    if (!admitted || !market_menu.timing.shown) { market_menu.encyclopedia->hide(); return; }
    const auto item = static_cast<std::uint32_t>(market_menu.timing.shown);
    const auto art = market_menu.art.find(item);
    if (art == market_menu.art.end()) { market_menu.encyclopedia->hide(); return; }
    if (market_menu.shown_item != item) {
        market_menu.title->set_text(String::utf8(art->second.title.c_str()));
        market_menu.body->set_text(String::utf8(art->second.description.c_str()));
        market_menu.icon->set_texture(command_texture(art->second.icon));
        for (std::size_t slot = 0; slot < 3; ++slot) market_menu.units[slot]->set_texture(command_texture(art->second.units[slot]));
        // WGEP-143: authored text and three icons; no price, owned status or abilities.
        const auto& surface = market_menu.surfaces[0];
        const auto* anchor = surface.anchors.find("i_blackmarket_encycl");
        const auto* back = command_catalog.find("encyclopedia_back");
        if (!anchor || !back) { market_menu.encyclopedia->hide(); return; }
        const auto placement = surface.panel->placement(); const auto scale = static_cast<float>(placement.scale);
        const auto extent = back->vec2(data::ui::Field::size).value_or(data::ui::Vec2{});
        const auto padding = back->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
        const Vector2 inset(padding.x * scale, padding.y * scale);
        const float width = extent.x * scale;
        for (const auto& [label, name] : {std::pair{market_menu.title, "encyclopedia_header_text"}, std::pair{market_menu.body, "encyclopedia_text"}}) {
            const auto* component = command_catalog.find(name); if (!component) continue;
            const auto face = hud_fonts->resolve({std::string(component->text(data::ui::Field::font_name)), component->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
            label->add_theme_font_override("font", hud_fonts->font(face));
            label->add_theme_font_size_override("font_size", std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height)));
            const auto tint = component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
            label->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
            label->set_size(Vector2(std::max(0.0F, width - inset.x * 2), 0));
        }
        const auto icon_size = std::max(0.0F, std::min(static_cast<float>(anchor->rect.height) * scale, (width - inset.x * 2) / 3));
        market_menu.icon->set_position(inset); market_menu.icon->set_size(Vector2(icon_size, icon_size));
        market_menu.title->set_position(inset + Vector2(icon_size, 0));
        market_menu.title->set_size(Vector2(std::max(0.0F, width - icon_size - inset.x * 2), icon_size));
        market_menu.body->set_position(inset + Vector2(0, icon_size));
        const auto unit_y = inset.y + icon_size + market_menu.body->get_minimum_size().y;
        for (std::size_t slot = 0; slot < 3; ++slot) {
            market_menu.units[slot]->set_position(Vector2(inset.x + static_cast<float>(slot) * icon_size, unit_y));
            market_menu.units[slot]->set_size(Vector2(icon_size, icon_size));
        }
        market_menu.encyclopedia->set_size(Vector2(width, unit_y + icon_size + inset.y));
        const auto rect = presentation::ui::shell_to_screen(anchor->rect, placement);
        const auto size = market_menu.encyclopedia->get_size();
        market_menu.encyclopedia->set_position(Vector2(std::clamp(static_cast<float>(rect.x), 0.0F, std::max(0.0F, static_cast<float>(camera.width) - size.x)),
            std::clamp(static_cast<float>(rect.y), 0.0F, std::max(0.0F, static_cast<float>(camera.height) - size.y))));
        market_menu.shown_item = item;
    }
    market_menu.encyclopedia->set_modulate(Color(1,1,1,static_cast<float>(market_menu.timing.alpha)));
    market_menu.encyclopedia->show();
}
void GalacticMode::State::write_black_market_report(std::ostream& output) const {
    using galactic_detail::json;
    output << "{\"source\": " << json(market_menu.fixture ? "controlled-observer-snapshot" : "campaign-snapshot")
        << ", \"activation_seam\": \"campaign black-market ability host (legacy #1130)\""
        << ", \"queue_seam\": \"copied index payload and story-before-debit provider (legacy #1130)\""
        << ", \"header_seam\": \"copied faction/displayed-tech header (legacy #1130)\""
        << ", \"layout_seam\": \"GHUD-C09 / WGEP-BM-U03\""
        << ", \"active\": " << (market_menu.blocker && market_menu.blocker->is_visible_in_tree() ? "true" : "false")
        << ", \"rebuilds\": " << market_menu.rebuilds << ", \"choices_accepted\": " << market_menu.choices_accepted
        << ", \"closes\": " << market_menu.closes << ", \"omitted\": " << market_menu.omitted
        << ", \"buyer\": " << (market_menu.facts ? market_menu.facts->buyer : 0)
        << ", \"planet\": " << (market_menu.facts ? market_menu.facts->planet : 0)
        << ", \"absence_raw\": " << (market_menu.facts ? market_menu.facts->absence.raw() : 0)
        << ", \"accepted_index\": " << (market_menu.accepted ? std::to_string(market_menu.accepted->index) : "null")
        << ", \"accepted_item\": " << (market_menu.accepted ? market_menu.accepted->item : 0)
        << ", \"accepted_buyer\": " << (market_menu.accepted ? market_menu.accepted->buyer : 0)
        << ", \"accepted_planet\": " << (market_menu.accepted ? market_menu.accepted->planet : 0)
        << ", \"accepted_price_raw\": " << (market_menu.accepted ? std::to_string(market_menu.accepted->price.raw()) : "null")
        << ", \"encyclopedia_item\": " << market_menu.shown_item
        << ", \"encyclopedia_active\": " << (market_menu.encyclopedia && market_menu.encyclopedia->is_visible_in_tree() ? "true" : "false")
        << ", \"encyclopedia_title\": " << json(market_menu.title ? std::string(market_menu.title->get_text().utf8().get_data()) : std::string{})
        << ", \"encyclopedia_body\": " << json(market_menu.body ? std::string(market_menu.body->get_text().utf8().get_data()) : std::string{})
        << ", \"encyclopedia_units\": [";
    const auto shown = market_menu.art.find(market_menu.shown_item);
    for (std::size_t slot = 0; slot < 3; ++slot)
        output << (slot ? ", " : "") << json(shown == market_menu.art.end() ? std::string{} : shown->second.units[slot]);
    output << "], \"controls\": [";
    bool comma{};
    const auto control = [&](const std::string& name, const EawrHudButton* button, const sim::galactic::ObservedBlackMarketOffer* offer) {
        if (!button || !button->is_visible()) return;
        const auto rect = button->get_global_rect();
        output << (comma ? ", " : "") << "{\"name\": " << json(name) << ", \"enabled\": " << (!button->is_disabled() ? "true" : "false")
            << ", \"index\": " << (offer ? offer->index : 0) << ", \"item\": " << (offer ? offer->item : 0)
            << ", \"owned\": " << (offer && offer->owned ? "true" : "false")
            << ", \"affordable\": " << (offer && offer->affordable ? "true" : "false")
            << ", \"affordability_available\": " << (offer && offer->affordability_available ? "true" : "false")
            << ", \"price_raw\": " << (offer ? offer->price.raw() : 0)
            << ", \"hit_rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
        comma = true;
    };
    const auto& surface = market_menu.surfaces[0];
    for (std::size_t slot = 0; slot < surface.choices.size(); ++slot)
        control(surface.choices[slot].first, surface.choices[slot].second, market_menu.bindings[slot] ? &*market_menu.bindings[slot] : nullptr);
    control(surface.close_name, surface.close, nullptr); output << "]}";
}
} // namespace eawr::presentation::godot_backend

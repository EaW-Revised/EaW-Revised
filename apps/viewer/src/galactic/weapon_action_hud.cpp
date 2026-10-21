#include "galactic_mode_internal.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/input_routing.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/loading_screen.hpp"
#include <godot_cpp/classes/image_texture.hpp>
#include <algorithm>
#include <ostream>
using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::string first(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto values = component.list(field);
    return values.empty() ? std::string{} : values.front();
}
}
bool GalacticMode::State::build_weapon_action_hud() {
    if (!filesystem || !hud || !hud_fonts) return true;
    if (options.hud_provider && !options.hud_provider->weapon_action(*this)) return false;
    weapon_menu.blocker = memnew(Control);
    hud->add_child(weapon_menu.blocker);
    weapon_menu.blocker->set_name("weapon_action_modal");
    weapon_menu.blocker->add_to_group(EawrUiModalLayer::group);
    weapon_menu.blocker->set_z_index(10);
    weapon_menu.blocker->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    // WGUI-47: menus lock world selection, including empty space and disabled rows.
    weapon_menu.blocker->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    weapon_menu.blocker->hide();
    for (std::size_t kind = 0; kind < 1; ++kind) {
        auto& surface = weapon_menu.surfaces[kind];
        const auto shell_name = "hack_shell";
        const auto* shell = command_catalog.find(shell_name);
        if (!shell) continue; // Missing authored menus stay unavailable.
        auto loaded = data::ui::load_shell_anchors(*filesystem, shell->text(data::ui::Field::model_name));
        if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
        surface.anchors = std::move(loaded).value().shell;
        surface.panel = memnew(EawrTacticalHud); weapon_menu.blocker->add_child(surface.panel);
        EawrTacticalHud::Setup setup;
        setup.view = presentation::ui::hud_view_model(surface.anchors, command_catalog,
            presentation::ui::alt_variant(command_faction), presentation::ui::vfs_shell_masks(*filesystem),
            presentation::ui::ShellOrigin::centre);
        const auto look = presentation::ui::hud_shell(surface.anchors, command_catalog, command_faction);
        for (const auto& mesh : look.meshes) {
            const auto path = presentation::ui::vfs_standalone_textures(*filesystem)(mesh.texture);
            if (!path) { failure = "weapon action shell texture is absent: " + mesh.texture; return false; }
            auto texture = assets::load_texture(*filesystem, *path);
            if (!texture) { failure = core::format_diagnostic(texture.error()); return false; }
            std::string why;
            auto pixels = texture_image(texture.value(), why);
            if (pixels.is_null()) { failure = why; return false; }
            setup.meshes.push_back({mesh.blend, ImageTexture::create_from_image(pixels), mesh.triangles});
        }
        surface.panel->setup(std::move(setup), nullptr);
        surface.close_name = "hack_close";
        for (const auto& anchor : surface.anchors.anchors()) {
            const auto* component = command_catalog.find(anchor.name);
            if (!component || !anchor.alt.shown_for(presentation::ui::alt_variant(command_faction))) continue;
            const bool choice = anchor.name == "hack_hack";
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
                    surface.close = button; button->set_action([this] { close_weapon_action(); });
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
            surface.choices[index].second->set_action([this, index] { choose_weapon_action(index); });
        surface.panel->hide();
    }
    return true;
}

void GalacticMode::State::close_weapon_action() {
    if (!weapon_menu.blocker || !weapon_menu.blocker->is_visible_in_tree()) return;
    static_cast<void>(map_input.hud_event());
    // A live menu retracts through its queued typed cancellation, never a local-only close (WGCH-148 keeps
    // the submitted command distinct: closing never withdraws one already queued).
    if (!weapon_menu.fixture && game && weapon_menu.facts && weapon_menu.facts->preparation) {
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
            sim::galactic::CancelGalacticAbility{weapon_menu.facts->actor, weapon_menu.facts->preparation}, {}});
        ++submitted_orders;
    }
    weapon_menu.closed = true; ++weapon_menu.closes; weapon_menu.blocker->hide();
}
void GalacticMode::State::choose_weapon_action(std::size_t index) {
    if (index || !weapon_menu.facts || !weapon_menu.facts->hack
        || !weapon_menu.enabled || weapon_menu.closed || !weapon_menu.facts->hack->affordable) return;
    if (!weapon_menu.fixture) {
        // Live acceptance queues the host's exact prepared capability; execution re-resolves everything.
        const auto& live = weapon_menu.facts->hack->live_prepared;
        if (!game || !live) return;
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::ExecutePreparedAbility{*live}, {}});
        ++submitted_orders;
    }
    // WGCH-66/148: controlled acceptance records prepared actor/planet/price,
    // never a weapon ID or a service call. Live queue and result are seams.
    weapon_menu.accepted = weapon_menu.facts->hack->prepared;
    static_cast<void>(map_input.hud_event()); ++weapon_menu.choices_accepted;
    weapon_menu.closed = true; weapon_menu.blocker->hide();
}
void GalacticMode::State::update_weapon_action_hud() {
    if (!weapon_menu.blocker || !displayed_snapshot) return;
    const auto publication = weapon_menu.fixture ? weapon_menu.fixture : displayed_snapshot;
    const bool changed = publication != weapon_menu.publication;
    const bool resized = camera.width != weapon_menu.width || camera.height != weapon_menu.height;
    const bool enabled = map_input.gates.hud_enabled && !map_input.gates.tutorial_locked;
    if (!changed && !resized && enabled == weapon_menu.enabled) return;
    weapon_menu.width = camera.width; weapon_menu.height = camera.height; weapon_menu.enabled = enabled;
    if (changed) {
        weapon_menu.publication = publication;
        const auto* facts = publication->hero_action_menus();
        if (facts && facts->player == galaxy->local.player) {
            // A fresh publication of the same active choice must not reopen a
            // cancelled/submitted menu. Only a changed activation may do that.
            const auto* old = weapon_menu.facts && weapon_menu.facts->hack ? &*weapon_menu.facts->hack : nullptr;
            if (!old || !facts->hack || old->prepared.actor != facts->hack->prepared.actor
                || old->prepared.planet != facts->hack->prepared.planet
                || old->prepared.required_type != facts->hack->prepared.required_type)
                weapon_menu.closed = false;
            weapon_menu.facts = *facts;
        } else { weapon_menu.facts.reset(); weapon_menu.closed = false; }
        ++weapon_menu.rebuilds;
    }
    auto& surface = weapon_menu.surfaces[0];
    const auto* choice = weapon_menu.facts && weapon_menu.facts->hack ? &*weapon_menu.facts->hack : nullptr;
    const bool available = choice && surface.panel && surface.close && surface.choices.size() == 1;
    // #4331: the controls below depend on this key alone (the anchors, labels and wording are fixed after the build), so a
    // publication that changed none of it - the common case: no menu open - writes none of them again.
    const WeaponActionHudKey key{available, enabled, weapon_menu.closed, choice != nullptr, choice && choice->affordable,
        choice && choice->live_prepared.has_value(), choice ? choice->prepared.prepared_price.raw() : 0,
        camera.width, camera.height, surface.panel ? std::optional(surface.panel->placement()) : std::nullopt};
    const bool unchanged = weapon_menu.applied == key;
    if (unchanged && !options.hud_verify) return;
    // Verification (--eawr-galactic-hud-verify): an unchanged key still writes the controls, and they must not change.
    struct Verify final {
        State& state; bool on; std::uint64_t drawn;
        ~Verify() {
            if (!on) return;
            ++state.hud_verify_checks;
            if (state.weapon_action_fingerprint() != drawn) {
                ++state.hud_verify_mismatches;
                if (state.hud_verify_detail.empty()) state.hud_verify_detail = "weapon action";
            }
        }
    } verify{*this, unchanged, unchanged ? weapon_action_fingerprint() : std::uint64_t{}};
    weapon_menu.applied = key;
    if (!unchanged) ++weapon_menu.writes;
    weapon_menu.blocker->set_visible(available && enabled && !weapon_menu.closed);
    if (!surface.panel) return;
    surface.panel->set_visible(available);
    const auto placement = surface.panel->placement();
    const auto position = [&](Control* control, std::string_view name) {
        const auto* anchor = surface.anchors.find(name); if (!anchor) return;
        const auto rect = presentation::ui::shell_to_screen(anchor->rect,placement);
        control->set_position(Vector2(static_cast<float>(rect.x),static_cast<float>(rect.y)));
        control->set_size(Vector2(static_cast<float>(rect.width),static_cast<float>(rect.height)));
    };
    for (const auto& [name,label] : surface.labels) {
        position(label,name); label->set_text({}); label->hide();
        const auto* component = command_catalog.find(name);
        const auto offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
        label->set_position(label->get_position()+Vector2(offset.x,offset.y)*static_cast<float>(placement.scale));
        label->add_theme_font_size_override("font_size",std::max(1,presentation::ui::font_pixel_height(
            component->integer(data::ui::Field::font_point_size).value_or(0),camera.height)));
        const auto tint=component->color(data::ui::Field::text_color)
            .value_or(component->color(data::ui::Field::color).value_or(data::ui::Rgba8{255,255,255,255}));
        label->add_theme_color_override("font_color",Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
    }
    for (const auto& [name,button] : surface.choices) {
        position(button,name); button->set_hit_rect({{},button->get_size()});
        button->set_disabled(!enabled || !choice || !choice->affordable || (!weapon_menu.fixture && !choice->live_prepared));
    }
    if (surface.close) {
        position(surface.close,surface.close_name); surface.close->set_hit_rect({{},surface.close->get_size()});
        surface.close->set_disabled(!enabled);
    }
    if (!available) return;
    const auto text = [&](const std::string& name,const std::string& value) {
        if (const auto found=surface.labels.find(name); found!=surface.labels.end()) {
            found->second->set_text(String::utf8(value.c_str())); found->second->set_visible(!value.empty());
        }
    };
    text("hack_title01",battle_text("TEXT_ABILITY_SLOT_HACK_SUPER_WEAPON"));
    text("hack_hack",battle_text("TEXT_BUTTON_ACCEPT"));
    text("hack_close",battle_text("TEXT_BUTTON_CANCEL"));
    const auto raw=choice->prepared.prepared_price.raw();
    const auto scale=sim::math::Fixed::scale;
    // GHUD-WH01: round only the display, without overflowing the exact price.
    text("hack_cost","$ "+std::to_string(raw/scale+(raw%scale >= scale/2 ? 1 : 0)));
    if (!choice->affordable) if (const auto found=surface.labels.find("hack_cost"); found!=surface.labels.end())
        found->second->add_theme_color_override("font_color",Color(160/255.0F,160/255.0F,160/255.0F));
    const auto first_line = surface.labels.find("hack_text_00");
    if (first_line != surface.labels.end()) {
        auto* label=first_line->second;
        const auto* component=command_catalog.find("hack_text_00");
        const auto width=component->vec2(data::ui::Field::size).value_or(data::ui::Vec2{}).y*placement.scale;
        const auto lines=presentation::ui::loading_wrap_lines(battle_text("TEXT_HACK_SUPER_WEAPON_DESCRIPTION"),width,
            [label](std::string_view value) { return label->get_theme_font("font")->get_string_size(
                String::utf8(value.data(),static_cast<int64_t>(value.size())),HORIZONTAL_ALIGNMENT_LEFT,-1,
                label->get_theme_font_size("font_size")).x; });
        // GHUD-WH01: authored description group has seven rows; surplus is omitted.
        for (std::size_t index=0;index<std::min<std::size_t>(7,lines.size());++index)
            text("hack_text_0"+std::to_string(index),lines[index]);
    }
}
std::uint64_t GalacticMode::State::weapon_action_fingerprint() const {
    galactic_view::HudDigest digest;
    const auto control = [&digest](const Control* value) {
        digest.add(value != nullptr);
        if (!value) return;
        digest.add(value->is_visible());
        if (!value->is_visible()) return; // a hidden control's place is rewritten before it is shown
        const auto position = value->get_position(), size = value->get_size();
        digest.add(position.x); digest.add(position.y); digest.add(size.x); digest.add(size.y);
    };
    control(weapon_menu.blocker);
    const auto& surface = weapon_menu.surfaces[0];
    control(surface.panel);
    for (const auto& [name, label] : surface.labels) {
        digest.add(std::string_view(name)); control(label);
        if (label) digest.add(std::string_view(galactic_detail::utf8(label->get_text())));
    }
    for (const auto& [name, button] : surface.choices) {
        digest.add(std::string_view(name)); control(button); if (button) digest.add(button->is_disabled());
    }
    control(surface.close); if (surface.close) digest.add(surface.close->is_disabled());
    return digest.value();
}
void GalacticMode::State::write_weapon_action_report(std::ostream& output) const {
    using galactic_detail::json;
    const auto* choice=weapon_menu.facts && weapon_menu.facts->hack ? &*weapon_menu.facts->hack : nullptr;
    output << "{\"source\": " << json(weapon_menu.fixture ? "controlled-observer-snapshot" : "campaign-snapshot")
        << ", \"activation_seam\": \"campaign weapon-action host (legacy #1144)\", \"queue_seam\": \"prepared weapon-action command provider (legacy #1144)\""
        << ", \"result_seam\": \"weapon-action result publication (legacy #1144)\", \"destruction_binding_seam\": \"strategic ability widget binding (legacy #2311)\""
        << ", \"active\": " << (weapon_menu.blocker && weapon_menu.blocker->is_visible_in_tree() ? "true" : "false")
        << ", \"rebuilds\": " << weapon_menu.rebuilds << ", \"choices_accepted\": " << weapon_menu.choices_accepted
        << ", \"closes\": " << weapon_menu.closes << ", \"writes\": " << weapon_menu.writes << ", \"actor\": " << (choice ? choice->prepared.actor : 0)
        << ", \"planet\": " << (choice ? choice->prepared.planet : 0) << ", \"weapon\": " << (choice ? choice->weapon : 0)
        << ", \"price_raw\": " << (choice ? choice->prepared.prepared_price.raw() : 0)
        << ", \"affordable\": " << (choice && choice->affordable ? "true" : "false")
        << ", \"accepted_price_raw\": " << (weapon_menu.accepted ? std::to_string(weapon_menu.accepted->prepared_price.raw()) : "null")
        << ", \"cost_text\": " << json(weapon_menu.surfaces[0].labels.contains("hack_cost")
            ? std::string(weapon_menu.surfaces[0].labels.at("hack_cost")->get_text().utf8().get_data()) : std::string{})
        << ", \"controls\": [";
    bool comma{};
    const auto control=[&](const std::string& name,const EawrHudButton* button) {
        if (!button || !button->is_visible()) return;
        const auto rect=button->get_global_rect();
        output << (comma ? ", " : "") << "{\"name\": " << json(name) << ", \"enabled\": " << (!button->is_disabled() ? "true" : "false")
            << ", \"hit_rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
        comma=true;
    };
    const auto& surface=weapon_menu.surfaces[0];
    control(surface.close_name,surface.close);
    for (const auto& [name,button] : surface.choices) control(name,button);
    output << "]}";
}
} // namespace eawr::presentation::godot_backend

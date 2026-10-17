#include "pending_battle_hud.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/theme_builder.hpp"
#include "ui/hud_icon_text.hpp"
#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

using namespace godot;
namespace eawr::presentation::godot_backend {
BuiltPendingBattleHud build_pending_battle_hud(const vfs::Vfs& filesystem,
    const data::ui::CommandBarCatalog& catalog, FontProvider& fonts, presentation::ui::HudFaction faction,
    Vector2 size, bool single, const std::function<Ref<Texture2D>(std::string_view)>& texture,
    const std::function<std::string(std::string_view)>& text,
    const std::function<void(const std::string&)>& sound) {
    BuiltPendingBattleHud built;
    const auto pending_colour = presentation::ui::battle_message_looks(filesystem).pending;
    built.shell = single ? "story_pending_battle_shell" : "pending_battle_shell";
    const auto* shell = catalog.find(built.shell); if (!shell) return built;
    const auto model_path = "data/art/models/" + std::string(shell->text(data::ui::Field::model_name));
    auto model = assets::load_model(filesystem, model_path);
    if (!model) return built;
    // GHUD-S75: the alert plays the first authored shell animation and holds
    // its shown pose. Bind-pose bones park the controls above the viewport.
    // Reuse the same model-derived idle binding as the shared pause shell.
    built.animation = model_path.substr(0, model_path.find_last_of('.')) + "_idle_00.ala";
    std::optional<assets::Animation> animation;
    auto loaded_animation = assets::load_animation(filesystem, built.animation);
    if (loaded_animation) animation = std::move(loaded_animation).value();
    auto player = presentation::animation::Player::create(model.value(), animation ? &*animation : nullptr);
    if (!player) return built;
    auto pose = player.value().sample_position(player.value().playable_frames(), 1); if (!pose) return built;
    built.animated_pose = animation.has_value();
    std::vector<data::ui::ShellTransform> transforms;
    transforms.reserve(pose.value().bones.size());
    for (const auto& bone : pose.value().bones) transforms.push_back(bone.model_asset);
    auto anchors = data::ui::shell_anchors(model.value(), transforms).shell;
    // Text-only bones are authored attachment points too.
    for (std::size_t index = 0; index < model.value().bones.size(); ++index) {
        const auto& name = model.value().bones[index].name;
        if (anchors.find(name) || !catalog.find(name)) continue;
        const auto& transform = pose.value().bones[index].model_asset;
        data::ui::ShellAnchor anchor; anchor.name = name; anchor.alt = data::ui::split_alt(name);
        anchor.rect = {transform[12], transform[13], 0, 0};
        anchor.origin = assets::Vec2f{transform[12], transform[13]}; anchors.add(std::move(anchor));
    }
    EawrTacticalHud::Setup setup;
    setup.view = presentation::ui::hud_view_model(anchors, catalog, presentation::ui::alt_variant(faction),
        presentation::ui::vfs_shell_masks(filesystem), presentation::ui::ShellOrigin::centre);
    auto shell_look = presentation::ui::hud_shell(anchors, catalog, faction);
    // The tactical helper excludes bound meshes because its separate button
    // builder supplies normal art. These pending buttons use their authored mesh.
    for (const auto* anchor : anchors.for_variant(presentation::ui::alt_variant(faction))) {
        if (anchor->name != "choice_button_left" && anchor->name != "choice_button_right"
            && anchor->name != "attack_button_single") continue;
        const bool additive = anchor->shader == "MeshAdditive.fx";
        if (!anchor->visible || (!additive && anchor->shader != "MeshAlpha.fx")
            || anchor->base_texture.empty() || anchor->triangles.empty()) continue;
        shell_look.meshes.push_back({anchor->name, additive ? presentation::ui::ShellBlend::additive
            : presentation::ui::ShellBlend::alpha, anchor->base_texture, anchor->z_min, anchor->triangles});
    }
    std::stable_sort(shell_look.meshes.begin(), shell_look.meshes.end(), [](const auto& left, const auto& right) { return left.z < right.z; });
    for (const auto& mesh : shell_look.meshes) {
        const auto path = presentation::ui::vfs_standalone_textures(filesystem)(mesh.texture); if (!path) return built;
        auto loaded = assets::load_texture(filesystem, *path); if (!loaded) return built;
        std::string why; auto pixels = texture_image(loaded.value(), why); if (pixels.is_null()) return built;
        setup.meshes.push_back({mesh.blend, ImageTexture::create_from_image(pixels), mesh.triangles});
    }
    const auto space = presentation::ui::reference_space({static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)});
    // GHUD-S75: the alert caller supplies half the UI camera height, moving
    // the authored centre origin to the top of the viewport. Manual_Offset
    // alone does not supply this translation; the shell's axis flags still apply.
    setup.view.shell_placement.manual_translation = {0.0, space.height / 2.0};
    const auto placement = presentation::ui::place_command_bar(space, 0, setup.view.shell_placement);
    auto* frame = memnew(Control); built.frame = frame;
    frame->set_name(String::utf8(built.shell.c_str())); frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    frame->set_size(size);
    auto* panel = memnew(EawrTacticalHud); built.surface = panel; frame->add_child(panel); panel->setup(std::move(setup), nullptr);
    panel->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    const auto add = [&](std::string id, const data::ui::ShellAnchor& anchor, Control* control, const char* kind) {
        frame->add_child(control);
        const auto rect = presentation::ui::shell_to_screen(anchor.rect, placement);
        control->set_position(Vector2(static_cast<float>(rect.x), static_cast<float>(rect.y)));
        control->set_size(Vector2(static_cast<float>(rect.width), static_cast<float>(rect.height)));
        if (auto* button = Object::cast_to<EawrHudButton>(control)) button->set_hit_rect({{}, control->get_size()});
        control->set_name(String::utf8(anchor.name.c_str()));
        built.gadgets.push_back({std::move(id), anchor.name, "", kind, "", "", control, rect});
    };
    for (const auto* anchor : anchors.for_variant(presentation::ui::alt_variant(faction))) {
        const auto* component = catalog.find(anchor->name); if (!component) continue;
        const bool begin = anchor->name == "choice_button_left" || anchor->name == "attack_button_single";
        const bool resolve = anchor->name == "choice_button_right";
        const bool title = anchor->name == "text_attack_choice" || anchor->name == "text_attack_single";
        if (!begin && !resolve && !title) continue;
        const std::string id = begin ? "fight" : resolve ? "auto" : "pending_title";
        if (begin || resolve) {
            auto* button = memnew(EawrHudButton);
            button->set_sounds(*component, sound);
            button->set_action([] {});
            button->set_focus_mode(Control::FOCUS_NONE); button->set_mouse_filter(Control::MOUSE_FILTER_STOP);
            const auto hover = component->list(data::ui::Field::mouse_over_texture_name);
            if (!hover.empty()) button->set_texture_hover(texture(hover.front()));
            button->set_meta("battle_caption", String::utf8(text(begin ? "TEXT_BUTTON_BEGIN" : "TEXT_BUTTON_RESOLVE_BATTLE").c_str()));
            add(id, *anchor, button, "button");
        }
        auto* label = memnew(EawrHudText);
        auto look = presentation::ui::hud_readout_style(catalog, anchor->name, *anchor);
        if (!look) { memdelete(label); continue; }
        // GHUD-S75: the alert writer overrides the component's text tint
        // with the shared GameConstants pending-message colour.
        if (title) look->text.colour = pending_colour;
        const auto face = fonts.resolve({look->text.face, look->text.point_size}, "english");
        const auto pixels = std::max(1, presentation::ui::font_pixel_height(face.point_size, static_cast<std::uint32_t>(size.y)));
        label->set_style(*look, fonts.command_font(face, pixels), [&fonts, face](std::int32_t height) {
            return presentation::ui::gdi_text_cell(fonts.cache(), face, height);
        }, pixels, static_cast<float>(placement.scale), 1.0F / static_cast<float>(placement.scale));
        const auto alignment = look->left_justified ? HORIZONTAL_ALIGNMENT_LEFT
            : look->right_justified ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_CENTER;
        label->set_horizontal_alignment(alignment);
        add(title ? id : id + "_caption", *anchor, label, "label");
        // GHUD-S75 / GHUD-S63: text is centred vertically on the bone plus
        // Text_Offset; justification controls which horizontal edge sits there.
        const auto point = presentation::ui::shell_point_to_screen(look->origin.x + look->text_offset.x,
            look->origin.y + look->text_offset.y, placement);
        label->set_meta("battle_text_anchor", Vector2(static_cast<float>(point.x), static_cast<float>(point.y)));
        if (!title) {
            const String caption = String::utf8(text(begin ? "TEXT_BUTTON_BEGIN" : "TEXT_BUTTON_RESOLVE_BATTLE").c_str());
            label->set_text(caption); label->set_meta("battle_caption", caption);
            const float width = label->get_minimum_size().x;
            const float shift = alignment == HORIZONTAL_ALIGNMENT_LEFT ? 0.0F
                : alignment == HORIZONTAL_ALIGNMENT_RIGHT ? width : width * 0.5F;
            label->set_size(Vector2(width, label->get_size().y));
            label->set_position(Vector2(static_cast<float>(point.x) - shift,
                static_cast<float>(point.y) - label->get_size().y * 0.5F));
        }
    }
    // WGUI-176: the centre graphs between the choice buttons (left = the local side, right = its opponent). Each is
    // a stack texture picked by unit count with the side's best ship on it; the dialog fills them from its view.
    for (const char* graph : {"graph_left", "graph_right"}) {
        const auto* anchor = anchors.find(graph);
        const auto* component = catalog.find(graph);
        if (!anchor || !component) continue;
        auto* box = memnew(Control);
        box->set_name(graph); box->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        frame->add_child(box);
        const auto rect = presentation::ui::shell_to_screen(anchor->rect, placement);
        box->set_position(Vector2(static_cast<float>(rect.x), static_cast<float>(rect.y)));
        box->set_size(Vector2(static_cast<float>(rect.width), static_cast<float>(rect.height)));
        for (const char* part : {"stack", "ship"}) {
            auto* image = memnew(TextureRect);
            image->set_name(part); image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            image->set_stretch_mode(TextureRect::STRETCH_SCALE);
            box->add_child(image);
        }
        PackedStringArray stacks;
        for (const auto& name : component->list(data::ui::Field::blank_texture_name)) stacks.push_back(String::utf8(name.c_str()));
        box->set_meta("graph_stacks", stacks);
        box->set_meta("graph_icon_scale", component->vec2(data::ui::Field::size).value_or(data::ui::Vec2{1.0F, 0.0F}).x);
        box->set_meta("graph_layer", component->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{}).x);
        box->set_meta("graph_scale", static_cast<float>(placement.scale));
        box->hide();
        built.gadgets.push_back({graph, graph, "", "graph", "", "", box, rect});
    }
    return built;
}
}

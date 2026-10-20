#include "tactical_hud_internal.hpp"
#include "eawr/data/xml.hpp"

namespace eawr::presentation::godot_backend {

void TacticalHud::State::build_team_dock(const data::ui::CommandBarCatalog& catalog) {
    using F = data::ui::Field;
    const auto* component = catalog.find("skirmish_upgrade");
    if (!component) return;
    const auto frames = component->list(F::blank_texture_name);
    for (std::size_t i = 0; i < std::min(frames.size(), team_dock_frames.size()); ++i)
        team_dock_frames[i] = command_texture(frames[i], "research dock frame");
    const auto builds = component->list(F::build_texture_name);
    if (!builds.empty()) team_dock_build = command_texture(builds.front(), "research dock progress");
    team_dock_scale = component->number(F::scale).value_or(1.0F);
    if (const auto size = component->vec2(F::size)) team_dock_icon_scale = size->y;
    if (const auto offset = component->vec2(F::text_offset)) team_dock_text_offset = {offset->x, -offset->y};
    if (!fonts) fonts = std::make_shared<FontProvider>(std::move(options.font_cache));
    team_dock_face = fonts->resolve({std::string(component->text(F::font_name)),
        component->integer(F::font_point_size).value_or(6)}, options.language);
    team_dock_text.font = fonts->font(team_dock_face);
    team_dock_text.outline = component->flag(F::text_outline);
    team_dock_text.emboss = component->flag(F::text_emboss);
    if (const auto color = component->color(F::text_color))
        team_dock_text.top = team_dock_text.bottom = tactical_hud_detail::colour(*color);
}

bool TacticalHud::team_dock_hovered() const { return state_->team_dock_hover; }

void TacticalHud::set_team_dock(const model::TeamResearchView& view,
    const std::span<const sim::tactical::UpgradeProfile> upgrades,
    const std::map<sim::tactical::TypeId, std::string>& names) {
    auto& state = *state_;
    if (!state.hud) return;
    // Cache raw, inherited presentation flags only for types seen in allied ledgers.
    const auto admit = [&](const sim::tactical::TypeId type) {
        if (std::any_of(state.team_dock_types.begin(), state.team_dock_types.end(),
            [type](const auto& item) { return item.type == type; })) return;
        const auto upgrade = std::find_if(upgrades.begin(), upgrades.end(),
            [type](const auto& item) { return item.type == type; });
        const auto name = names.find(type);
        if (upgrade == upgrades.end() || name == names.end() || !state.objects) return;
        const auto object = state.objects->resolve(name->second, data::Category::game_object);
        if (!object) return; // Retry unresolved metadata on the next snapshot.
        state.team_dock_types.push_back(model::team_research_type(type, upgrade->level_up, object.value()));
    };
    for (const auto& ledger : view.ledgers) {
        if (!sim::tactical::players_allied(view.players, view.local, ledger.player)) continue;
        for (const auto& entry : ledger.completed) admit(entry.type);
        for (const auto& queue : ledger.queues) for (const auto& entry : queue) admit(entry.type);
    }
    const bool changed = state.team_dock.refresh(view, state.team_dock_types, state.team_dock_policy);
    const auto rows = state.team_dock.rows();
    state.team_dock_hover = false;
    if (rows.empty() || state.team_dock_frames[3].is_null()) {
        if (state.team_dock_group && state.team_dock_group->is_visible()) state.team_dock_group->hide();
        state.team_dock_hover_order.reset();
        return;
    }
    const Vector2 viewport = state.hud->get_size();
    const bool relayout = state.team_dock_viewport != viewport;
    state.team_dock_viewport = viewport;
    const auto& policy = state.team_dock_policy;
    const float scale = static_cast<float>(state.hud->space().scale) * state.team_dock_scale;
    const Vector2 size = state.team_dock_frames[3]->get_size() * scale;
    if (!state.team_dock_group) {
        state.team_dock_group = memnew(Control);
        state.team_dock_group->set_name("EawrTeamDock");
        state.team_dock_group->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        state.hud->add_child(state.team_dock_group);
    }
    if (!state.team_dock_group->is_visible()) state.team_dock_group->show();
    const bool layout_changed = relayout || rows.size() != state.team_dock_drawn_rows
        || std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
            const auto index = static_cast<std::size_t>(&row - rows.data());
            const auto& previous = state.team_dock_cells[index].row;
            return !previous || previous->display_order != row.display_order || previous->in_progress != row.in_progress;
        });
    if (changed || relayout) {
        state.team_dock_group->set_position({viewport.x - policy.right_margin * scale - size.x, policy.top * scale});
        const auto pixels = model::font_pixels({state.team_dock_face.point_size, false, 1.0},
            model::font_screen_height(state.hud->space()));
        state.team_dock_text.size = pixels.glyph_height;
        const auto font_cell = model::gdi_text_cell(state.fonts->cache(), state.team_dock_face, pixels.glyph_height);
        state.team_dock_text.cell_ascent = font_cell.ascent;
        state.team_dock_text.cell_descent = font_cell.descent;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index == state.team_dock_cells.size()) {
                State::TeamDockCell cell;
                cell.button = memnew(EawrHudButton);
                cell.button->setup_art({.portrait = HudButtonArt::Portrait{{}, false, true}});
                cell.button->set_action([] {}); // WPR-76: completed-row single clicks are inert.
                cell.button->set_mouse_filter(policy.takes_pointer ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
                state.team_dock_group->add_child(cell.button);
                state.team_dock_cells.push_back(cell);
            }
            auto& cell = state.team_dock_cells[index];
            const auto& row = rows[index];
            if (!cell.button->is_visible()) cell.button->show();
            // WPR-70: one column; completed research first in observed completion order.
            const bool first = index == 0 || rows[index - 1].in_progress != row.in_progress;
            const bool last = index + 1 == rows.size() || rows[index + 1].in_progress != row.in_progress;
            const auto frame = first && last ? 3U : first ? 0U : last ? 2U : 1U;
            if (layout_changed) {
                cell.button->set_position({0, static_cast<float>(index) * policy.row_step * scale});
                cell.button->set_size(size);
                cell.button->set_hit_rect({{}, size});
                cell.button->set_cell_state_art({.normal = state.team_dock_frames[frame]}, size * 0.5F, scale);
            }
            const bool identity_changed = !cell.row || cell.row->type != row.type;
            if (identity_changed) {
                const auto name = names.find(row.type);
                const std::string type = name == names.end() ? std::to_string(row.type) : name->second;
                cell.icon = state.command_texture(state.card_type(type).first, "research dock icon");
                cell.button->set_icon_texture(cell.icon);
            }
            Rect2 portrait = cell.button->cell_portrait_box();
            if (layout_changed || identity_changed) {
                const auto portrait_size = (cell.icon.is_valid() ? cell.icon->get_size() : Vector2())
                    * state.team_dock_icon_scale * scale;
                portrait = {(size - portrait_size) * 0.5F, portrait_size};
                cell.button->set_cell_portrait_box(portrait);
            }
            if (layout_changed || !cell.row || cell.row->percent != row.percent) {
                cell.button->set_cell_progress(row.in_progress ? std::optional(PortraitCellRenderer::Progress{
                    state.team_dock_build, portrait, static_cast<double>(row.percent) / 100.0}) : std::nullopt);
                cell.button->set_cell_text({state.team_dock_text,
                    row.in_progress ? String::num_uint64(row.percent) + "%" : String(),
                    Rect2(Vector2(-25, -6) * scale + size * 0.5F + state.team_dock_text_offset * scale,
                        Vector2(50, 12) * scale), HORIZONTAL_ALIGNMENT_CENTER, scale});
            }
            cell.row = row;
        }
        state.team_dock_drawn_rows = rows.size();
        for (std::size_t index = rows.size(); index < state.team_dock_cells.size(); ++index)
            if (state.team_dock_cells[index].button->is_visible()) state.team_dock_cells[index].button->hide();
    }
    std::optional<std::uint64_t> hovered;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].in_progress || !state.team_dock_cells[i].button->is_visible_in_tree()
            || !state.team_dock_cells[i].button->is_hovered()) continue;
        hovered = rows[i].display_order;
        state.team_dock_hover = true;
        if (hovered != state.team_dock_hover_order && state.cards) {
            const auto name = names.find(rows[i].type);
            state.cards->set_external_hover((std::uint64_t{1} << 61U) + *hovered,
                name == names.end() ? std::nullopt : std::optional(state.encyclopedia_type(name->second)));
            state.team_dock_encyclopedia = true;
        }
        break;
    }
    state.team_dock_hover_order = hovered;
}

} // namespace eawr::presentation::godot_backend

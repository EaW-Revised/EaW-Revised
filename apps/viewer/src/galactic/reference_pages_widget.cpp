#include "reference_pages_widget.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/battle_overlay.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/theme_builder.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/style_box_texture.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <algorithm>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace page = viewer::galactic;
namespace {
void caption(Control* control, const std::string& text) {
    if (auto* label = Object::cast_to<EawrUiLabel>(control)) label->set_text(String::utf8(text.c_str()));
    if (auto* label = Object::cast_to<Label>(control)) label->set_text(String::utf8(text.c_str()));
}
std::string number(const std::optional<std::int64_t>& value) { return value ? std::to_string(*value) : ""; }
}
page::ReferenceFacts ReferencePages::facts(GalacticMode::State& state) {
    page::ReferenceFacts result;
    result.local_player = state.galaxy->local.player;
    // GREF seams: no archive/cadence/save, advisor archive or legend provider yet.
    // Current balances are never synthesized into chart samples.
    if (!reference_catalog_) return result;
    if (types_.empty()) for (const auto& [id, name] : state.production_names)
        if (auto type = reference_catalog_->metadata(name)) {
            type->icon.type = id; types_.emplace(id, std::move(*type));
        }
    if (planets_.empty()) for (const auto& planet : state.galaxy->planets)
        if (auto type = reference_catalog_->metadata(planet.name)) planets_.emplace(planet.id, std::move(*type));
    result.planets = page::published_summary_planets(*state.displayed_snapshot, result.local_player,
        state.hud_facts, types_, planets_);
    const auto* affiliation = state.content ? page::affiliation_for_owner(*state.content, result.local_player) : nullptr;
    const auto faction = affiliation ? affiliation->faction : state.galaxy->local.faction;
    // GREF-04: choose authored dialogs, preserving their icon positions/arrows.
    result.tree.combined = faction == "Underworld";
    const auto stem = result.tree.combined ? "IDD_TECH_TREE_BLACKMARKET" : faction == "Empire"
        ? "IDD_TECH_TREE_EMPIRE" : faction == "Rebel" ? "IDD_TECH_TREE_REBEL" : "";
    result.tree.land_dialog = stem;
    result.tree.space_dialog = result.tree.combined || std::string_view(stem).empty() ? stem : std::string(stem) + "_SPACE";
    if (state.displayed_snapshot->player_economy())
        for (const auto& player : state.displayed_snapshot->player_economy()->players)
            if (player.player == result.local_player) result.tree.technology = player.current_tech;
    const sim::galactic::ProductionPlayer* local{};
    if (state.displayed_snapshot->production()) for (const auto& player : state.displayed_snapshot->production()->players)
        if (player.player == result.local_player) local = &player;
    const auto slots = [&](const std::string& dialog_name) {
        return reference_catalog_->tree_slots(catalog_->script.find(dialog_name), faction, state.production_names);
    };
    if (!authored_tree_) {
        result.tree.land = slots(result.tree.land_dialog);
        result.tree.space = result.tree.combined ? result.tree.land : slots(result.tree.space_dialog);
        authored_tree_ = result.tree;
    } else {
        result.tree.land = authored_tree_->land; result.tree.space = authored_tree_->space;
    }
    for (auto* list : {&result.tree.land, &result.tree.space}) for (auto& slot : *list)
        slot.unlocked = local && std::find(local->unlocked.begin(), local->unlocked.end(), slot.type.type) != local->unlocked.end();
    return result;
}
void ReferencePages::service(GalacticMode::State& state) {
    if (!state.hud || !state.galaxy || !state.displayed_snapshot || !state.filesystem || !state.hud_fonts) return;
    if (!root_) {
        auto loaded = data::ui::load_dialog_catalog(*state.filesystem);
        auto database = data::ui::load_language_text_database(*state.filesystem, "english");
        if (!loaded || !database) return;
        catalog_ = std::make_shared<data::ui::DialogCatalog>(std::move(loaded).value());
        auto text = std::make_shared<data::ui::TextDatabase>(std::move(database).value());
        text_ = [text](std::string_view key) {
            const auto* value = text->find(key);
            return value ? data::ui::to_utf8(value->value) : std::string(key);
        };
        auto types = data::load_catalog_strict(*state.filesystem, state.profile == "remake" ? data::Profile::remake
            : (state.profile == "foc" ? data::Profile::foc : data::Profile::eaw));
        if (!types) return;
        reference_catalog_.emplace(std::move(types).value().catalog, text_);
        auto loaded_atlas = assets::load_mega_texture_atlas(*state.filesystem,
            "data/art/textures/" + catalog_->skin.texture_file + ".mtd");
        if (!loaded_atlas) return;
        auto atlas = std::make_shared<assets::MegaTextureAtlas>(std::move(loaded_atlas).value());
        auto textures = std::make_shared<UiTextures>(*atlas, *state.filesystem);
        build_ = [&state, this, atlas, textures, text](std::string_view name, Vector2 size) {
            const auto* definition = catalog_->script.find(name);
            if (!definition) return BuiltDialog{};
            presentation::ui::ThemeSources sources;
            sources.catalog = catalog_.get(); sources.atlas = &atlas->directory;
            sources.standalone = presentation::ui::vfs_standalone_textures(*state.filesystem);
            sources.fonts = &state.hud_fonts->cache(); sources.language = "english";
            const auto& families = state.hud_fonts->system_families();
            sources.system = [&families](std::string_view face) { return presentation::ui::match_system_face(face, families).has_value(); };
            const auto theme = presentation::ui::build_theme_model(sources,
                presentation::ui::reference_space({static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)}));
            data::ui::TextLookup lookup(*text);
            auto built = build_dialog(*definition, theme, &lookup, presentation::ui::Placement::centre, false,
                [&state](std::string_view cue) { if (state.audio) state.audio->ui_sound(cue); });
            built.frame->set_theme(build_theme(theme, *textures, *state.hud_fonts));
            return built;
        };
        root_ = memnew(EawrUiModalLayer); state.hud->add_child(root_);
        root_->set_name("galactic_reference_pages"); root_->set_z_index(20);
        root_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT); root_->hide();
        const auto* component = state.command_catalog.find("b_droid_help");
        if (component && state.command_anchors.find("b_droid_help") && state.command_bar) {
            auto* help = memnew(EawrHudButton);
            help->set_sounds(*component, [&state](const std::string& cue) { if (state.audio) state.audio->ui_sound(cue); });
            help->set_name("b_droid_help");
            const auto icon = component->list(data::ui::Field::icon_texture_name);
            if (!icon.empty()) help->set_texture_normal(state.command_texture(icon.front()));
            const auto hover = component->list(data::ui::Field::mouse_over_texture_name);
            if (!hover.empty()) help->set_texture_hover(state.command_texture(hover.front()));
            help->set_action([this] { model_.open(); dirty_ = true; });
            state.command_bar->add_child(help); state.command_buttons.emplace("b_droid_help", help);
        }
    }
    const Vector2 size(static_cast<float>(state.camera.width), static_cast<float>(state.camera.height));
    if (model_.visible && Input::get_singleton()->is_action_just_pressed("ui_cancel")) model_.visible = false;
    root_->set_visible(model_.visible && !state.tactical && !state.loading);
    if (!root_->is_visible()) return;
    state.held.clear(); state.orbiting = state.scrolling = false;
    if (snapshot_ != state.displayed_snapshot) {
        model_.publish(facts(state)); snapshot_ = state.displayed_snapshot; dirty_ = true;
    }
    if (size_ != size) { size_ = size; dirty_ = true; }
    if (dirty_) { rebuild(state); dirty_ = false; }
}
void ReferencePages::activate(GalacticMode::State& state, const std::string& id) {
    using enum page::ReferenceTab;
    if (id == "IDC_BUTTON_CLOSE" || id == "IDC_BUTTON_EXIT") model_.visible = false;
    else if (id == "IDC_DROID_LOG_RADIO") model_.choose(log);
    else if (id == "IDC_DATA_CHART_RADIO") model_.choose(history);
    else if (id == "IDC_TECH_TREE_RADIO") model_.choose(tree);
    else if (id == "IDC_SUMMARY_SCREEN_RADIO") model_.choose(summary);
    else if (id == "IDC_PRODUCTION_RADIO") model_.category(page::SummaryKind::production);
    else if (id == "IDC_ECONOMY_RADIO") model_.category(page::SummaryKind::economy);
    else if (id == "IDC_FLEETS_RADIO") model_.category(page::SummaryKind::fleets);
    else if (id == "IDC_RADIO_ECONOMY") model_.metric = page::HistoryMetric::economy;
    else if (id == "IDC_RADIO_MILITARY") model_.metric = page::HistoryMetric::military;
    else if (id == "IDC_RADIO_PLANETS") model_.metric = page::HistoryMetric::planets;
    else if (id == "IDC_TECH_TREE_LAND") model_.space = false;
    else if (id == "IDC_TECH_TREE_SPACE") model_.space = true;
    else if (id == "IDC_BUTTON_PLANETARY") model_.order(page::SummarySort::name);
    else if (id == "IDC_BUTTON_PRODUCING") model_.order(page::SummarySort::fronts);
    else if (id == "IDC_BUTTON_UPGRADES") model_.order(page::SummarySort::upgrades);
    else if (id == "IDC_BUTTON_POP_CAP") model_.order(page::SummarySort::capacity);
    else if (id == "IDC_BUTTON_TOTAL_INCOME") model_.order(page::SummarySort::total);
    else if (id == "IDC_BUTTON_BASE_CREDIT_VALUE") model_.order(page::SummarySort::basic);
    else if (id == "IDC_BUTTON_TRADE_ROUTE_INCOME") model_.order(page::SummarySort::trade);
    else if (id == "IDC_BUTTON_MINING_FACILITY_INCOME") model_.order(page::SummarySort::mining);
    else if (id.starts_with("IDC_PLANET_ICON")) {
        const auto row = static_cast<std::size_t>(std::stoul(id.substr(15)));
        if (const auto target = model_.target(row, state.map_input.gates.selection_locked)) {
            auto effect = state.map_input.select(state.input_planets(), *target, state.seat_flight().state(), false);
            if (effect.accepted) {
                // GREF-17: Summary requests a camera scroll even for the current selection.
                effect.position = true;
                state.apply_selection(effect); state.apply_camera(); model_.visible = false;
            }
        }
    }
    dirty_ = true;
}
void ReferencePages::rebuild(GalacticMode::State& state) {
    std::string name = "IDD_DROID_LOG";
    if (model_.tab == page::ReferenceTab::history) name = "IDD_DATA_CHART";
    if (model_.tab == page::ReferenceTab::tree) name = model_.space ? model_.facts.tree.space_dialog : model_.facts.tree.land_dialog;
    if (model_.tab == page::ReferenceTab::summary) name = model_.kind == page::SummaryKind::production
        ? "IDD_SUMMARY_SCREEN_PRODUCTION" : model_.kind == page::SummaryKind::economy ? "IDD_SUMMARY_SCREEN_ECONOMY" : "IDD_SUMMARY_SCREEN_FLEETS";
    if (dialog_ && dialog_->frame && dialog_name_ == name && dialog_size_ == size_) { refresh(state); return; }
    if (dialog_ && dialog_->frame) { root_->remove_child(dialog_->frame); dialog_->frame->queue_free(); }
    dialog_name_ = name; dialog_size_ = size_;
    dialog_ = build_(name, size_);
    if (!dialog_->frame) return;
    root_->add_child(dialog_->frame);
    // RC declaration order is not a paint order: group boxes are backgrounds,
    // including those declared after their headings and radio controls.
    for (const auto& gadget : dialog_->gadgets)
        if (gadget.kind == "group" && gadget.control) dialog_->frame->move_child(gadget.control, 0);
    for (auto& gadget : dialog_->gadgets) {
        const bool tab = gadget.id == "IDC_DROID_LOG_RADIO" || gadget.id == "IDC_DATA_CHART_RADIO"
            || gadget.id == "IDC_TECH_TREE_RADIO" || gadget.id == "IDC_SUMMARY_SCREEN_RADIO";
        if (gadget.control && !tab) {
            // These pages use the dark dialog background. Keep their captions
            // readable without changing the shared kit or catalog theme.
            for (const auto* role : {"Global_Default", "L_Text", "R_Text", "C_Text", "Push_Button"})
                for (const auto* edge : {"_top", "_bottom"})
                    gadget.control->add_theme_color_override(String(role) + edge, Color(1,1,1,1));
        }
        // Tabs retain the shared BS_PUSHLIKE widget's authored font, gradient,
        // skin and RC rectangle rather than a separate engine-default caption.
        if (gadget.class_name == "PETROGLYPH_DIALOG_IMAGE") {
            auto* image = memnew(TextureRect); dialog_->frame->add_child(image); gadget.control = image;
            image->set_name(String::utf8(gadget.id.c_str()));
            image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            image->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
            image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            image->set_position(Vector2(static_cast<float>(gadget.rect.x), static_cast<float>(gadget.rect.y)) - dialog_->frame->get_position());
            image->set_size(Vector2(static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)));
            if (gadget.id.starts_with("IDC_PLANET_ICON")) {
                auto* button = memnew(EawrOverlayButton); image->add_child(button);
                button->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
                button->set_action([this, &state, id = gadget.id] { activate(state, id); });
            }
        }
        if (auto* button = Object::cast_to<BaseButton>(gadget.control)) {
            auto* relay = memnew(EawrOverlayButton); dialog_->frame->add_child(relay); relay->hide();
            relay->set_action([this, &state, id = gadget.id] { activate(state, id); });
            button->connect("pressed", callable_mp(relay, &EawrOverlayButton::activate));
        }
        if (gadget.id == "IDC_SCROLLBAR_SLIDER" && gadget.control) {
            const auto position = gadget.control->get_position(), extent = gadget.control->get_size();
            gadget.control->queue_free();
            auto* bar = memnew(VScrollBar); dialog_->frame->add_child(bar); gadget.control = bar;
            bar->set_theme_type_variation(String::utf8(gadget.variation.c_str()));
            for (const auto& [slot, skin] : {std::pair{"decrement", "Scroll_Up_Button"},
                    std::pair{"decrement_highlight", "Scroll_Up_Button_Mouse_Over"},
                    std::pair{"decrement_pressed", "Scroll_Up_Button_Pressed"},
                    std::pair{"increment", "Scroll_Down_Button"},
                    std::pair{"increment_highlight", "Scroll_Down_Button_Mouse_Over"},
                    std::pair{"increment_pressed", "Scroll_Down_Button_Pressed"}})
                bar->add_theme_icon_override(slot, bar->get_theme_icon(skin, String::utf8(gadget.variation.c_str())));
            for (const auto& [slot, skin] : {std::pair{"scroll", "Scroll_Middle"},
                    std::pair{"scroll_focus", "Scroll_Middle"}, std::pair{"grabber", "Scroll_Tab"},
                    std::pair{"grabber_highlight", "Scroll_Tab"}, std::pair{"grabber_pressed", "Scroll_Tab"}}) {
                Ref<StyleBoxTexture> style; style.instantiate();
                style->set_texture(bar->get_theme_icon(skin, String::utf8(gadget.variation.c_str())));
                bar->add_theme_stylebox_override(slot, style);
            }
            bar->set_position(position); bar->set_size(extent); bar->set_step(1); bar->set_page(0);
            bar->set_max(static_cast<double>(model_.maximum_offset()));
            bar->set_value(static_cast<double>(model_.offset)); bar->set_visible(model_.maximum_offset() != 0);
            bar->connect("value_changed", callable_mp_static(+[](double value, int64_t pointer) {
                auto* self = reinterpret_cast<ReferencePages*>(pointer);
                self->model_.scroll(static_cast<std::size_t>(value)); self->dirty_ = true;
            }).bind(reinterpret_cast<int64_t>(this)));
        }
    }
    refresh(state);
}
void ReferencePages::refresh(GalacticMode::State& state) {
    if (!dialog_ || !dialog_->frame) return;
    const auto set_text = [&](const std::string& id, const std::string& value) {
        for (auto& gadget : dialog_->gadgets) if (gadget.id == id && gadget.control) caption(gadget.control, value);
    };
    const auto set_icon = [&](const std::string& id, const page::ReferenceIcon* value, bool dimmed = false) {
        for (auto& gadget : dialog_->gadgets) if (gadget.id == id) if (auto* image = Object::cast_to<TextureRect>(gadget.control)) {
            image->set_visible(value && !value->icon.empty());
            if (!value) continue;
            image->set_texture(state.command_texture(value->icon));
            image->set_modulate(dimmed ? Color(90.0F/255, 90.0F/255, 90.0F/255, 1) : Color(1,1,1,1));
            image->set_mouse_filter(Control::MOUSE_FILTER_PASS);
            image->set_tooltip_text(String::utf8((value->name + (value->description.empty() ? "" : "\n" + value->description)).c_str()));
        }
    };
    for (auto& gadget : dialog_->gadgets) {
        if (auto* button = Object::cast_to<BaseButton>(gadget.control)) {
            const std::map<std::string, bool> selected{
                {"IDC_DROID_LOG_RADIO", model_.tab == page::ReferenceTab::log},
                {"IDC_DATA_CHART_RADIO", model_.tab == page::ReferenceTab::history},
                {"IDC_TECH_TREE_RADIO", model_.tab == page::ReferenceTab::tree},
                {"IDC_SUMMARY_SCREEN_RADIO", model_.tab == page::ReferenceTab::summary},
                {"IDC_PRODUCTION_RADIO", model_.kind == page::SummaryKind::production},
                {"IDC_ECONOMY_RADIO", model_.kind == page::SummaryKind::economy},
                {"IDC_FLEETS_RADIO", model_.kind == page::SummaryKind::fleets},
                {"IDC_RADIO_ECONOMY", model_.metric == page::HistoryMetric::economy},
                {"IDC_RADIO_MILITARY", model_.metric == page::HistoryMetric::military},
                {"IDC_RADIO_PLANETS", model_.metric == page::HistoryMetric::planets},
                {"IDC_TECH_TREE_LAND", !model_.space}, {"IDC_TECH_TREE_SPACE", model_.space}};
            if (const auto found = selected.find(gadget.id); found != selected.end()) button->set_pressed_no_signal(found->second);
        }
        if (gadget.id == "IDC_SCROLLBAR_SLIDER") if (auto* bar = Object::cast_to<VScrollBar>(gadget.control)) {
            bar->set_max(static_cast<double>(model_.maximum_offset()));
            bar->set_value_no_signal(static_cast<double>(model_.offset)); bar->set_visible(model_.maximum_offset() != 0);
        }
        if (gadget.class_name == "PETROGLYPH_DIALOG_IMAGE" && gadget.control) gadget.control->hide();
        if (gadget.class_name == "PETROGLYPH_DIALOG_IMAGE" && gadget.control)
            for (int index = gadget.control->get_child_count()-1; index >= 0; --index)
                if (auto* label = Object::cast_to<Label>(gadget.control->get_child(index))) {
                    gadget.control->remove_child(label); label->queue_free();
                }
        // Clear authored placeholder text and list backgrounds before filling rows.
        if (gadget.control && (gadget.id.starts_with("IDC_PLANET_NAME") || gadget.caption == "Static"
            || gadget.caption == "xN")) caption(gadget.control, "");
        if (gadget.control && gadget.kind == "list") gadget.control->hide();
    }
    if (model_.tab == page::ReferenceTab::tree) {
        if (!model_.facts.tree.technology) set_text("IDC_HINTS", "Technology information is unavailable.");
        // GREF-04: arrows occupy authored dialog slots, never production-graph edges.
        // Each id family takes its atlas arrow, stretched to the authored rect at full alpha.
        for (auto& gadget : dialog_->gadgets) if (const auto texture = page::tree_arrow_texture(gadget.id); !texture.empty())
            if (auto* control = Object::cast_to<TextureRect>(gadget.control)) {
                control->set_stretch_mode(TextureRect::STRETCH_SCALE);
                control->set_modulate(Color(1,1,1,1));
                if (const auto skin = state.command_texture(texture); skin.is_valid()) {
                    control->set_texture(skin); control->show(); continue;
                }
                // Visible project line skin when a mod's atlas lacks the entry.
                const bool down = gadget.id.starts_with("IDC_DOWN_ARROW");
                Ref<Image> arrow = Image::create_empty(down ? 14 : 30, down ? 30 : 14, false, Image::FORMAT_RGBA8);
                arrow->fill(Color(0,0,0,0));
                for (int along = 0; along < 30; ++along) {
                    arrow->set_pixel(down ? 7 : along, down ? along : 7, Color(1,1,1,1));
                    if (along >= 23) {
                        const int width = 29-along;
                        arrow->set_pixel(down ? 7-width : along, down ? along : 7-width, Color(1,1,1,1));
                        arrow->set_pixel(down ? 7+width : along, down ? along : 7+width, Color(1,1,1,1));
                    }
                }
                control->set_texture(ImageTexture::create_from_image(arrow)); control->show();
            }
        for (const auto& slot : page::tree_view(model_.facts.tree, model_.space)) set_icon(slot.control, &slot.type, slot.dimmed);
    } else if (model_.tab == page::ReferenceTab::history) {
        const auto chart = model_.facts.history ? page::history_view(*model_.facts.history, model_.metric) : page::HistoryView{};
        if (!chart.available) { set_text("IDC_HINTS", "History samples are unavailable."); return; }
        set_text("IDC_STATIC_START", "0"); set_text("IDC_STATIC_MIDDLE", std::to_string(chart.middle_cycle));
        set_text("IDC_STATIC_END", std::to_string(chart.end_cycle)); set_text("IDC_Y_MIN", "0");
        set_text("IDC_Y_MAX", model_.metric == page::HistoryMetric::military ? "100%" : std::to_string(static_cast<std::int64_t>(chart.maximum)));
        set_text("IDC_Y_CAPTION", text_(model_.metric == page::HistoryMetric::economy ? "TEXT_CHECK_ECONOMY"
            : model_.metric == page::HistoryMetric::military ? "TEXT_CHECK_MILITARY" : "TEXT_CHECK_PLANETS"));
        Ref<Image> image = Image::create_empty(640, 266, false, Image::FORMAT_RGBA8); image->fill(Color(0,0,0,0));
        for (const auto& line : chart.lines) for (std::size_t index = 1; index < line.points.size(); ++index) {
            auto a = line.points[index-1], b = line.points[index];
            const auto steps = std::max(1, static_cast<int>(std::max(std::abs(b[0]-a[0])*639, std::abs(b[1]-a[1])*265)));
            for (int step = 0; step <= steps; ++step) {
                const auto alpha = static_cast<double>(step)/steps;
                const int x = std::clamp(static_cast<int>((a[0]+(b[0]-a[0])*alpha)*639), 0, 639);
                const int y = std::clamp(static_cast<int>((1-a[1]-(b[1]-a[1])*alpha)*265), 0, 265);
                image->set_pixel(x,y,Color(line.colour.r,line.colour.g,line.colour.b,1));
            }
        }
        for (auto& gadget : dialog_->gadgets) if (gadget.id == "IDC_DATA_CHART_IMAGE")
            if (auto* control = Object::cast_to<TextureRect>(gadget.control)) { control->set_texture(ImageTexture::create_from_image(image)); control->show(); }
    } else if (model_.tab == page::ReferenceTab::log) {
        set_text("IDC_HINTS", "Advisor log and reference legend are unavailable.");
        for (auto& gadget : dialog_->gadgets) if (gadget.control && gadget.kind == "check")
            if (auto* button = Object::cast_to<BaseButton>(gadget.control)) button->set_disabled(true);
    } else {
        const bool fleets = model_.kind == page::SummaryKind::fleets;
        if (fleets) set_text("IDC_HINTS", "Orbital fleet details are unavailable.");
        for (std::size_t row = 0; row < (fleets ? 10U : 6U); ++row) {
            const auto index = model_.offset + row;
            if (index >= (fleets ? model_.summary.fleet_rows.size() : model_.summary.planets.size())) continue;
            const auto& planet = model_.summary.planets[fleets ? model_.summary.fleet_rows[index].planet_index : index];
            const auto suffix = std::to_string(row);
            page::ReferenceIcon planet_icon{planet.icon, planet.name, {}, planet.planet, 1};
            if (!fleets || model_.summary.fleet_rows[index].first) {
                set_text("IDC_PLANET_NAME"+suffix, planet.name); set_icon("IDC_PLANET_ICON"+suffix, &planet_icon);
            }
            if (model_.kind == page::SummaryKind::economy) {
                const auto economy = page::economy_row(planet);
                set_text("IDC_TOTAL_INCOME_"+suffix, number(economy.total)); set_text("IDC_BASE_CREDIT_VALUE_"+suffix, number(economy.basic));
                set_text("IDC_TRADE_ROUTE_INCOME_"+suffix, number(economy.trade)); set_text("IDC_MINING_FACILITY_INCOME_"+suffix, number(economy.mining));
                if (economy.mines) { set_icon("IDC_MINING_FACILITY_ICON_"+suffix, &economy.mine);
                    set_text("IDC_NUM_MINING_FACILITIES_STATIC_"+suffix, "x"+std::to_string(economy.mines)); }
            } else if (fleets) {
                const auto& icons = model_.summary.fleet_rows[index].icons;
                for (std::size_t icon = 0; icon < icons.size(); ++icon) {
                    const auto id = "IDC_ROW"+suffix+"_FLEET"+std::to_string(icon);
                    set_icon(id, &icons[icon]);
                    if (icons[icon].count > 1) {
                        for (const auto& gadget : dialog_->gadgets) if (gadget.id == id && gadget.control) {
                            auto* count = memnew(Label); gadget.control->add_child(count);
                            count->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                            count->set_text(String::utf8(("x"+std::to_string(icons[icon].count)).c_str()));
                            count->set_position(Vector2(0, gadget.control->get_size().y*.6F));
                        }
                    }
                }
            } else {
                if (planet.capacity && *planet.capacity > 0) set_text("IDC_POP_CAP_"+suffix, std::to_string(static_cast<int>(*planet.capacity)));
                for (std::size_t domain = 0; domain < 2; ++domain) if (planet.fronts[domain]) {
                    const auto& front = *planet.fronts[domain]; page::ReferenceIcon icon{front.icon, front.type, {}, front.type_id, 1};
                    const auto id = std::string(domain == 0 ? "IDC_SP_ICON" : "IDC_GP_ICON")+suffix;
                    set_icon(id, &icon);
                    for (const auto& gadget : dialog_->gadgets) if (gadget.id == id && gadget.control) {
                        auto* percent = memnew(Label); gadget.control->add_child(percent);
                        percent->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                        percent->set_text(String::utf8((std::to_string(front.progress_percent)+"%").c_str()));
                        percent->set_position(Vector2(0, gadget.control->get_size().y*.7F));
                    }
                }
                if (planet.upgrades) for (std::size_t icon = 0; icon < std::min<std::size_t>(8, planet.upgrades->size()); ++icon)
                    set_icon("IDC_HERO"+suffix+"_ICON"+std::to_string(icon), &(*planet.upgrades)[icon]);
            }
        }
        const auto& totals = model_.summary.totals;
        set_text("IDC_TOTAL_INCOME_TOTAL", number(totals.total)); set_text("IDC_BASE_CREDIT_VALUE_TOTAL", number(totals.basic));
        set_text("IDC_TRADE_ROUTE_INCOME_TOTAL", number(totals.trade)); set_text("IDC_MINING_FACILITIES_INCOME_TOTAL", number(totals.mining));
        if (model_.summary.capacity) set_text("IDC_POP_TOTAL_STATIC", std::to_string(static_cast<int>(*model_.summary.capacity)));
    }
}
void service_reference_pages(GalacticMode::State& state) {
    if (!state.reference_pages) state.reference_pages = std::make_unique<ReferencePages>();
    state.reference_pages->service(state);
}
} // namespace eawr::presentation::godot_backend

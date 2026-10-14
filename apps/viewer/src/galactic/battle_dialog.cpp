#include "battle_dialog.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/hud_icon_text.hpp"
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/style_box_texture.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <sstream>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
using namespace viewer::galactic;
void scroll_changed(double value, int64_t address, int64_t side) {
    reinterpret_cast<BattleDialog*>(address)->scroll(static_cast<std::size_t>(side), value);
}
std::string phase_name(BattleDialogPhase phase) {
    switch (phase) {
    case BattleDialogPhase::hidden: return "hidden";
    case BattleDialogPhase::choice: return "choice";
    case BattleDialogPhase::progress: return "progress";
    case BattleDialogPhase::summary: return "summary";
    }
    return "hidden";
}
}

BattleDialog::BattleDialog(Control& parent, Style style,
    std::function<bool(std::uint64_t, viewer::galactic::BattleChoice)> choice)
    : style_(std::move(style)), choice_(std::move(choice)) {
    root_ = memnew(Control);
    parent.add_child(root_);
    root_->set_name("CampaignBattleDialog");
    root_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    // GHUD-S75: the pending choice owns the foreground, including a fleet
    // drag preview (the drag receiver uses absolute Z 100).
    root_->set_z_as_relative(false);
    root_->set_z_index(110);
    root_->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    advisor_ = memnew(Label); root_->add_child(advisor_);
    advisor_->set_z_index(1);
    advisor_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    advisor_->add_theme_color_override("font_color", Color(1, 24.0F / 255, 39.0F / 255)); // WAR-64
    root_->hide();
}
BattleDialog::~BattleDialog() {
    // The owning State tears down before its parent node.
    if (root_) memdelete(root_);
}
bool BattleDialog::visible() const { return view_.phase != viewer::galactic::BattleDialogPhase::hidden; }
bool BattleDialog::blocks_world(Vector2 point) const {
    if (!visible() || !root_->is_visible_in_tree()) return false;
    if (view_.phase != viewer::galactic::BattleDialogPhase::choice) return true;
    if (!pending_) return false;
    const auto local = point - root_->get_global_position();
    return pending_->surface && pending_->surface->hit(local.x, local.y);
}
std::vector<BuiltGadget>& BattleDialog::gadgets() { return pending_ ? pending_->gadgets : dialog_->gadgets; }
const std::vector<BuiltGadget>& BattleDialog::gadgets() const { return pending_ ? pending_->gadgets : dialog_->gadgets; }
void BattleDialog::present(viewer::galactic::BattleDialogView view) {
    if (view == view_ && size_ == root_->get_size()) return;
    summary_rows_dirty_ = true;
    const bool changed = view.conflict != view_.conflict || view.phase != view_.phase || view.single_button != view_.single_button;
    if (changed) { submitted_ = false; scroll_ = {}; listed_at_ = -1; }
    if (view.choice_ready && (view.response_revision != view_.response_revision
        || (!view_.choice_ready && view.choice_ready) || view.refusal != view_.refusal)) submitted_ = false;
    view_ = std::move(view);
    std::string advisor_text;
    for (const auto& line : view_.advisor) { if (!advisor_text.empty()) advisor_text += '\n'; advisor_text += line; }
    if (!view_.refusal.empty()) { if (!advisor_text.empty()) advisor_text += '\n'; advisor_text += view_.refusal; }
    advisor_->set_text(String(advisor_text.c_str()));
    advisor_->set_position(Vector2(16, 4));
    advisor_->set_size(Vector2(root_->get_size().x - 32, 30));
    root_->set_visible(visible() && (view_.phase != viewer::galactic::BattleDialogPhase::choice || view_.pending_visible));
    root_->set_mouse_filter(view_.phase == viewer::galactic::BattleDialogPhase::choice ? Control::MOUSE_FILTER_IGNORE : Control::MOUSE_FILTER_STOP);
    if (!visible()) return;
    if (changed || (!dialog_ && !pending_) || size_ != root_->get_size()) rebuild();
    refresh();
}
void BattleDialog::service_layout() {
    if (!visible()) return;
    // GUI picking uses sibling order. Later fleet publications add controls
    // after the dialog; keep its choices in front for input as well as art.
    if (auto* parent = root_->get_parent(); parent && root_->get_index() + 1 != parent->get_child_count())
        parent->move_child(root_, parent->get_child_count() - 1);
    if (size_ == root_->get_size()) return;
    advisor_->set_size(Vector2(root_->get_size().x - 32, 30));
    rebuild(); refresh();
}
void BattleDialog::caption(BuiltGadget& gadget, std::string_view value) {
    const String text(std::string(value).c_str());
    if (auto* label = Object::cast_to<EawrHudText>(gadget.control)) {
        label->set_text(text); label->set_meta("battle_caption", text);
        const Vector2 anchor = label->get_meta("battle_text_anchor");
        const auto alignment = label->get_horizontal_alignment();
        const float width = label->get_minimum_size().x;
        const float shift = alignment == HORIZONTAL_ALIGNMENT_LEFT ? 0.0F
            : alignment == HORIZONTAL_ALIGNMENT_RIGHT ? width : width * 0.5F;
        label->set_size(Vector2(width, label->get_size().y));
        label->set_position(anchor - Vector2(shift, label->get_size().y * 0.5F));
    }
    if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control)) {
        label->set_text(text); label->set_meta("battle_caption", text);
        if (pending_ && gadget.rect.width == 0) {
            const auto font = label->get_theme_font("Global_Default");
            const auto pixels = label->get_theme_font_size("Global_Default");
            const auto extent = font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, pixels);
            label->set_position(Vector2(static_cast<float>(gadget.rect.x) - extent.x / 2, static_cast<float>(gadget.rect.y)));
            label->set_size(extent);
        }
    }
    if (auto* button = Object::cast_to<EawrUiButton>(gadget.control)) {
        button->set_text(text); button->set_meta("battle_caption",text);
    }
}
void BattleDialog::activate(viewer::galactic::BattleChoice choice) {
    using namespace viewer::galactic;
    const bool allowed = view_.involved && ((view_.phase == BattleDialogPhase::choice && view_.choice_ready &&
        ((choice == BattleChoice::fight && view_.fight_allowed && view_.tactical_available) ||
         (choice == BattleChoice::auto_resolve && view_.auto_allowed))) ||
        (view_.phase == BattleDialogPhase::progress && choice == BattleChoice::retreat && view_.retreat_allowed) ||
        (view_.phase == BattleDialogPhase::summary && (choice == BattleChoice::continue_galaxy
            || (choice == BattleChoice::invade && !view_.auto_resolved && view_.won && view_.invasion_fleet && view_.invasion_planet))));
    if (!allowed || submitted_ || (!dialog_ && !pending_)) return;
    ++choices_;
    for (auto& gadget : gadgets()) if (auto* button = Object::cast_to<BaseButton>(gadget.control))
        if (gadget.id != "IDC_ADVANCED_RESOLVE_INFO") button->set_disabled(true);
    // Copy identity before dispatch; the owner can immediately publish a new view.
    const auto id = view_.conflict;
    const auto dispatch = choice_;
    if (!dispatch_battle_choice(submitted_, id, choice, dispatch)) refresh();
}
void BattleDialog::toggle_advanced() {
    advanced_ = !advanced_; listed_at_ = -1;
    rebuild(); refresh(); // WAR-66: switching creates the other resource.
}
void BattleDialog::scroll(std::size_t side, double value) {
    if (side >= 2) return;
    const auto next = static_cast<std::size_t>(std::max(0.0, value));
    if (scroll_[side] == next) return;
    scroll_[side] = next; summary_rows_dirty_ = true;
    refresh();
}
void BattleDialog::rebuild() {
    summary_rows_dirty_ = true;
    using namespace viewer::galactic;
    if (dialog_ && dialog_->frame) {
        dialog_->frame->hide(); dialog_->frame->queue_free();
    }
    if (pending_ && pending_->frame) { pending_->frame->hide(); pending_->frame->queue_free(); }
    // A rebuild can run inside this relay's callback; leave its function alive
    // until deferred deletion instead of destroying the executing closure.
    for (auto* relay : relays_) relay->queue_free();

    relays_.clear(); dialog_.reset(); pending_.reset(); hero_counts_ = {}; size_ = root_->get_size();
    if (view_.phase == BattleDialogPhase::choice) {
        auto built = style_.pending ? style_.pending(size_, view_.single_button) : BuiltPendingBattleHud{};
        if (!built.frame) return;
        pending_ = std::move(built); root_->add_child(pending_->frame);
    } else {
        const auto resource = view_.phase == BattleDialogPhase::summary ? "IDD_BATTLE_END_DIALOG"
            : advanced_ ? "IDD_AUTO_RESOLVE_DIALOG" : "IDD_AUTO_RESOLVE_DIALOG_LITE";
        auto built = style_.dialog(resource, size_);
        if (!built.frame) return;
        dialog_ = std::move(built); root_->add_child(dialog_->frame);
    }
    listed_at_ = -1; // WAR-122: newly created bars/lists sample even at unchanged logical time.
    ++rebuilds_;

    if (view_.phase == BattleDialogPhase::summary) {
        // WGP-34's two complementary figures. The RC has no named efficiency
        // controls, so these use a project placement between headings/hero slots.
        const auto efficiency = campaign_efficiency(view_.results);
        for (std::size_t side = 0; side < 2; ++side) {
            auto* label = memnew(EawrUiLabel); dialog_->frame->add_child(label);
            label->set_position(Vector2(size_.x * (side == 0 ? .055F : .545F), size_.y * .25F));
            label->set_size(Vector2(size_.x * .35F, size_.y * .025F));
            label->set_name(side == 0 ? "campaign_efficiency_own" : "campaign_efficiency_enemy");
            label->set_text(String((std::to_string(efficiency[side]) + "%").c_str()));
        }
        if (!view_.picture.empty()) {
            auto* picture = memnew(TextureRect); dialog_->frame->add_child(picture);
            picture->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            picture->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            picture->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
            picture->set_position(Vector2(size_.x * .78F, size_.y * .02F));
            picture->set_size(Vector2(size_.x * .15F, size_.y * .17F));
            picture->set_texture(style_.texture(view_.picture));
        }
    }
    auto action = [&](BuiltGadget& gadget, std::function<void()> callback) {
        auto* button = Object::cast_to<BaseButton>(gadget.control);
        if (!button) return;
        auto* relay = memnew(EawrOverlayButton); root_->add_child(relay); relay->hide();
        relay->set_action(std::move(callback)); relays_.push_back(relay);
        button->connect("pressed", callable_mp(relay, &EawrOverlayButton::activate));
    };
    for (auto& gadget : gadgets()) {
        if (gadget.class_name == "PETROGLYPH_DIALOG_IMAGE") {
            auto* image = memnew(TextureRect);
            image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            image->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
            image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            image->set_position(Vector2(static_cast<float>(gadget.rect.x), static_cast<float>(gadget.rect.y))
                - dialog_->frame->get_position());
            image->set_size(Vector2(static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)));
            dialog_->frame->add_child(image); gadget.control = image;
            for (std::size_t side = 0; side < 2; ++side) for (std::size_t index = 0; index < 5; ++index) {
                const auto prefix = side == 0 ? "IDC_YOUR_HERO" : "IDC_ENEMY_HERO";
                if (gadget.id != prefix + std::to_string(index)) continue;
                // WGP-116: the authored hero image has no separate count
                // gadget. Keep its caption within that image's rectangle.
                auto* count = memnew(EawrUiLabel); image->add_child(count);
                count->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                count->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
                hero_counts_[side][index] = count;
            }
        }
        if (!gadget.control) continue;
        if (gadget.id == "IDC_ADVANCED_RESOLVE_INFO") {
            auto* check = Object::cast_to<BaseButton>(gadget.control);
            if (check) check->set_pressed_no_signal(advanced_);
            action(gadget, [this] { toggle_advanced(); });
        } else if (gadget.id == "IDC_LAND_INVADE_BUTTON") action(gadget, [this] { activate(BattleChoice::invade); });
        else if (gadget.id == "IDC_WITHDRAW_BUTTON" || gadget.id == "retreat")
            action(gadget, [this] { activate(BattleChoice::retreat); });
        else if (gadget.id == "IDC_CONTINUE_BUTTON") {
            caption(gadget, style_.text("TEXT_BUTTON_RETURN_TO_GALACTIC"));
            action(gadget, [this] { activate(BattleChoice::continue_galaxy); });
        } else if (gadget.id == "fight") action(gadget, [this] { activate(BattleChoice::fight); });
        else if (gadget.id == "auto") action(gadget, [this] { activate(BattleChoice::auto_resolve); });
        else if (gadget.id == "IDC_YOUR_SCROLLBAR" || gadget.id == "IDC_ENEMY_SCROLLBAR") {
            const std::size_t side = gadget.id == "IDC_YOUR_SCROLLBAR" ? 0 : 1;
            const auto position = gadget.control->get_position(); const auto size = gadget.control->get_size();
            gadget.control->queue_free();
            auto* bar = memnew(VScrollBar); dialog_->frame->add_child(bar); gadget.control = bar;
            bar->set_theme_type_variation(String(gadget.variation.c_str()));
            for (const auto& [name, slot] : {std::pair{"decrement", "Scroll_Up_Button"},
                std::pair{"decrement_highlight", "Scroll_Up_Button_Mouse_Over"},
                std::pair{"decrement_pressed", "Scroll_Up_Button_Pressed"},
                std::pair{"increment", "Scroll_Down_Button"},
                std::pair{"increment_highlight", "Scroll_Down_Button_Mouse_Over"},
                std::pair{"increment_pressed", "Scroll_Down_Button_Pressed"}})
                bar->add_theme_icon_override(name, bar->get_theme_icon(slot, String(gadget.variation.c_str())));
            for (const auto& [name, slot] : {std::pair{"scroll", "Scroll_Middle"},
                std::pair{"scroll_focus", "Scroll_Middle"}, std::pair{"grabber", "Scroll_Tab"},
                std::pair{"grabber_highlight", "Scroll_Tab"}, std::pair{"grabber_pressed", "Scroll_Tab"}}) {
                Ref<StyleBoxTexture> skin; skin.instantiate();
                skin->set_texture(bar->get_theme_icon(slot, String(gadget.variation.c_str())));
                bar->add_theme_stylebox_override(name, skin);
            }
            bar->set_position(position); bar->set_size(size); bar->set_step(1);
            bar->set_max(static_cast<double>(presentation::ui::loss_scroll_max(view_.results.losses[side].size())));
            bar->set_page(0);
            bar->connect("value_changed", callable_mp_static(&scroll_changed)
                .bind(reinterpret_cast<int64_t>(this), static_cast<int64_t>(side)));
            bar->set_visible(presentation::ui::loss_scroll_max(view_.results.losses[side].size()) != 0);
        }
    }
}
void BattleDialog::refresh() {
    using namespace viewer::galactic;
    if (!dialog_ && !pending_) return;
    if (view_.phase == BattleDialogPhase::summary) {
        if (!summary_rows_dirty_) return;
        ++summary_refreshes_;
    }
    const bool relist = listed_at_ < 0 || view_.elapsed_seconds < listed_at_ || view_.elapsed_seconds - listed_at_ >= 1.0;
    for (auto& gadget : gadgets()) {
        auto* control = gadget.control;
        if (!control) continue;
        const auto& id = gadget.id;
        if (id == "IDC_CLOSE_BUTTON" || id == "IDC_SAVE_REPLAY_BUTTON"
            || id == "IDC_COUNTDOWN_TIMER") { control->hide(); continue; }
        if (id == "IDC_STATIC_BOUNTY") {
            const bool shown=view_.phase == BattleDialogPhase::summary && campaign_bounty_visible(view_.bounty);
            control->set_visible(shown); caption(gadget,shown ? view_.bounty->caption : ""); continue;
        }
        if (id == "IDC_LAND_INVADE_BUTTON") control->set_visible(!view_.auto_resolved && view_.won && view_.invasion_fleet && view_.invasion_planet);
        if (id == "IDC_WITHDRAW_BUTTON" || id == "retreat") control->set_visible(view_.involved);
        if (id == "fight" || id == "fight_caption") control->show();
        if (id == "auto" || id == "auto_caption") control->set_visible(!view_.single_button);
        if (auto* button = Object::cast_to<BaseButton>(control)) {
            if (id == "fight") button->set_disabled(submitted_ || !view_.choice_ready || !view_.fight_allowed || !view_.tactical_available);
            if (id == "auto") button->set_disabled(!battle_auto_enabled(view_, submitted_));
            if (id == "retreat" || id == "IDC_WITHDRAW_BUTTON")
                button->set_disabled(submitted_ || !view_.retreat_allowed);
        }
        if (relist && (id == "IDC_AUTO_RESOLVE_PROGRESS_DEFENDER" || id == "IDC_AUTO_RESOLVE_PROGRESS_INVADER")) {
            if (auto* bar = Object::cast_to<Range>(control)) {
                bar->set_min(0); bar->set_max(1); bar->set_step(0);
                bar->set_value(auto_resolve_progress(view_.elapsed_seconds, view_.display_seconds));
            }
        }
        if (id == "IDC_DEFENDING_FORCES_LIST" || id == "IDC_INVADING_FORCES_LIST") {
            const std::size_t side = id == "IDC_DEFENDING_FORCES_LIST" ? 0 : 1;
            if (relist) if (auto* list = Object::cast_to<ItemList>(control)) {
                list->clear();
                if (view_.involved) for (const auto& name : view_.sides[side].combatants) {
                    const auto index = list->add_item(String(name.c_str()));
                    const auto& colour = view_.sides[side].colour;
                    list->set_item_custom_fg_color(index, Color(colour[0], colour[1], colour[2]));
                }
            }
        }
        if (id == "IDC_STATIC_MEDIUM") caption(gadget, view_.involved ? view_.planet : "");
        if (id == "pending_title") caption(gadget, viewer::galactic::pending_battle_title(view_, style_.text));
        if (view_.phase == BattleDialogPhase::progress && id == "IDC_STATIC") {
            // Built captions already passed through the mounted text database.
            const std::size_t side = gadget.caption == style_.text("TEXT_DEFENDING_FORCES") ? 0 : 1;
            caption(gadget, view_.involved ? view_.sides[side].name : "");
            const auto& colour = view_.sides[side].colour;
            control->add_theme_color_override("L_Text_top", Color(colour[0], colour[1], colour[2]));
            control->add_theme_color_override("L_Text_bottom", Color(colour[0], colour[1], colour[2]));
        }
        if (auto* image = Object::cast_to<TextureRect>(control)) {
            std::string texture;
            if (id == "IDC_DEFENDER_ICON" && view_.involved) texture = view_.sides[0].icon;
            if (id == "IDC_INVADER_ICON" && view_.involved) texture = view_.sides[1].icon;
            // WAR-66: the full resource's misspelled invader slot stays blank.
            if (id == "IDC_INVADERL_ICON") texture.clear();
            for (std::size_t side = 0; side < 2; ++side) {
                const std::string prefix = side == 0 ? "IDC_YOUR_" : "IDC_ENEMY_";
                for (std::size_t index = 0; index < 12; ++index)
                    if (id == prefix + "UNIT" + std::to_string(index) && index + scroll_[side] < view_.results.losses[side].size())
                        texture = view_.results.losses[side][index + scroll_[side]].type;
                for (std::size_t index = 0; index < 5; ++index)
                    if (id == prefix + "HERO" + std::to_string(index)) {
                        const auto& heroes = view_.results.heroes[side];
                        if (view_.phase == BattleDialogPhase::summary) {
                            const auto count = index < heroes.size() ? campaign_loss_count(heroes[index].count) : std::string{};
                            if (hero_counts_[side][index]) hero_counts_[side][index]->set_text(String(count.c_str()));
                            control->set_meta("battle_caption", String(count.c_str()));
                        }
                        if (index < heroes.size()) texture = heroes[index].type;
                    }
            }
            image->set_texture(texture.empty() ? Ref<Texture2D>{} : style_.texture(texture));
            image->set_visible(!texture.empty());
            if (id == "IDC_DEFENDER_ICON" || id == "IDC_INVADER_ICON") {
                const auto& colour = view_.sides[id == "IDC_DEFENDER_ICON" ? 0 : 1].colour;
                image->set_modulate(Color(colour[0], colour[1], colour[2]));
            }
        }
        if (view_.phase == BattleDialogPhase::summary) {
            if (id == "IDC_CONTINUE_BUTTON") caption(gadget,style_.text("TEXT_BUTTON_RETURN_TO_GALACTIC"));
            if (id == "IDC_TITLE_STATIC") caption(gadget, style_.text(view_.won ? "TEXT_WIN_BATTLE" : "TEXT_LOST_BATTLE"));
            if (id == "IDC_TIME_STATIC") caption(gadget, style_.text("TEXT_BATTLE_TIME") + " "
                + presentation::ui::battle_time_text(view_.results.elapsed_milliseconds));
            if (id == "IDC_BATTLE_END_PLANET_STATIC") caption(gadget, view_.planet);
            for (std::size_t side = 0; side < 2; ++side) for (std::size_t index = 0; index < 12; ++index) {
                const auto prefix = side == 0 ? "IDC_YOUR_UNIT_NAME" : "IDC_ENEMY_UNIT_NAME";
                if (id != prefix + std::to_string(index)) continue;
                const auto row = index + scroll_[side];
                const auto& losses = view_.results.losses[side];
                caption(gadget, row < losses.size() ? campaign_loss_count(losses[row].count) : "");
            }
        }
    }
    if (relist) listed_at_ = view_.elapsed_seconds;
    summary_rows_dirty_ = false;
}
std::string BattleDialog::report_json() const {
    using galactic_detail::json;
    std::ostringstream out;
    out << "{\"phase\":" << json(phase_name(view_.phase)) << ",\"conflict\":" << view_.conflict
        << ",\"advanced\":" << (advanced_ ? "true" : "false") << ",\"mounted\":" << (dialog_ || pending_ ? "true" : "false")
        << ",\"pending_shell\":" << json(pending_ ? pending_->shell : "")
        << ",\"show_animation\":" << json(pending_ ? pending_->animation : "")
        << ",\"animated_pose\":" << (pending_ && pending_->animated_pose ? "true" : "false")
        << ",\"placement_seam\":" << json(pending_ ? "GHUD-S75" : "")
        << ",\"overlay_z\":" << root_->get_z_index()
        << ",\"tactical_admission\":" << viewer::galactic::battle_tactical_report_json(view_, json)
        << ",\"progress\":" << viewer::galactic::auto_resolve_progress(view_.elapsed_seconds, view_.display_seconds)
        << ",\"rebuilds\":" << rebuilds_ << ",\"choices\":" << choices_
        << ",\"summary_refreshes\":" << summary_refreshes_ << ",\"controls\":[";
    std::size_t count{};
    if (dialog_ || pending_) for (const auto& gadget : gadgets()) if (gadget.control) {
        const auto rect = gadget.control->get_global_rect();
        const auto* button = Object::cast_to<BaseButton>(gadget.control);
        const String label_text = gadget.control->get_meta("battle_caption", String());
        out << (count++ ? "," : "") << "{\"name\":" << json(gadget.id)
            << ",\"text\":" << json(std::string(label_text.utf8().get_data()))
            << ",\"visible\":" << (gadget.control->is_visible_in_tree() ? "true" : "false")
            << ",\"disabled\":" << (button && button->is_disabled() ? "true" : "false")
            << ",\"rect\":[" << rect.position.x << ',' << rect.position.y << ',' << rect.size.x << ',' << rect.size.y << "]";
        if (auto* list = Object::cast_to<ItemList>(gadget.control)) {
            out << ",\"items\":[";
            for (int item = 0; item < list->get_item_count(); ++item)
                out << (item ? "," : "") << json(std::string(list->get_item_text(item).utf8().get_data()));
            out << ']';
        }
        if (auto* bar = Object::cast_to<Range>(gadget.control)) out << ",\"value\":" << bar->get_value();
        if (const auto* label = Object::cast_to<EawrHudText>(gadget.control)) {
            const Vector2 anchor = label->get_meta("battle_text_anchor");
            out << ",\"text_anchor\":[" << anchor.x << ',' << anchor.y << "]"
                << ",\"alignment\":" << static_cast<int>(label->get_horizontal_alignment());
        }
        out << '}';
    }
    out << "],\"loss_totals\":[" << view_.results.totals[0] << ',' << view_.results.totals[1]
        << "],\"time\":" << json(presentation::ui::battle_time_text(view_.results.elapsed_milliseconds))
        << ",\"auto_resolved\":" << (view_.auto_resolved ? "true" : "false")
        << ",\"bounty_provider_seam\":\"observer local collected tally and faction benefit\""
        << ",\"scoring_provider_seam\":\"scoring-script death projection, scale and score-control outputs\""
        << ",\"efficiency\":[" << campaign_efficiency(view_.results)[0] << ',' << campaign_efficiency(view_.results)[1]
        << "],\"advisor\":[";
    for (std::size_t index = 0; index < view_.advisor.size(); ++index) out << (index ? "," : "") << json(view_.advisor[index]);
    out << "]}";
    return out.str();
}
}

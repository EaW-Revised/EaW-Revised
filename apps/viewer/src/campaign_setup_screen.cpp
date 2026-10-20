#include "campaign_setup_screen.hpp"
#include "eawr/core/text.hpp"

#include "ui/kit.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <string_view>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;
namespace {

String text(const std::string_view value) { return String::utf8(value.data(), static_cast<int64_t>(value.size())); }

std::string trim(std::string_view value) {
    return core::trim(value, core::xml_whitespace);
}

// WGP-73: the preview's backdrop is the shared galactic backdrop texture; a texture reference is
// looked up as a .tga or a .dds of its stem (as the menu overlay's is).
constexpr std::string_view preview_backdrop = "galactic_back";
// WGP-73: the radar's world window the blips are normalised against (x by 440, y by 520).
constexpr double blip_x_origin = 220.0, blip_x_span = 440.0, blip_y_origin = 260.0, blip_y_span = 520.0;
constexpr float blip_size = 4.0F;

// The text database writes a line break inside a description as the two characters backslash and n.
std::string with_line_breaks(std::string value) {
    for (std::size_t at = 0; (at = value.find("\\n", at)) != std::string::npos; ++at) value.replace(at, 2, "\n");
    return value;
}

std::string localized_key(const data::Catalog& catalog, const std::string_view name, const data::Category category,
                          const std::string_view tag) {
    auto resolved = catalog.resolve(name, category);
    if (!resolved) return {};
    const auto* field = resolved.value().value(tag);
    return field != nullptr ? trim(field->value.raw_text) : std::string{};
}

// "r, g, b, a" as the Color tag writes it, 0..255.
std::optional<Color> parse_colour(const std::string_view raw) {
    std::array<float, 4> parts{0.0F, 0.0F, 0.0F, 255.0F};
    std::size_t count = 0;
    std::size_t at = 0;
    while (at <= raw.size() && count < parts.size()) {
        const auto comma = raw.find(',', at);
        const std::string token = trim(raw.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
        float value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (token.empty() || parsed.ec != std::errc{}) return std::nullopt;
        parts[count++] = value;
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    if (count < 3) return std::nullopt;
    return Color(parts[0] / 255.0F, parts[1] / 255.0F, parts[2] / 255.0F, parts[3] / 255.0F);
}

} // namespace

CampaignSetupScreen::CampaignSetupScreen() = default;
CampaignSetupScreen::~CampaignSetupScreen() = default;

std::string CampaignSetupScreen::caption(const std::string_view key) const {
    if (key.empty()) return {};
    return resources_.text != nullptr ? data::ui::to_utf8(resources_.text->text(key)) : std::string(key);
}

bool CampaignSetupScreen::build(Control& root, const Resources& resources, const model::CampaignHostSupport support) {
    resources_ = resources;
    if (resources_.filesystem == nullptr || resources_.catalog == nullptr || resources_.dialogs == nullptr
        || resources_.theme_model == nullptr) {
        failure_ = "the campaign staging screen needs the front end's data load";
        return false;
    }
    auto loaded = data::galactic::load_galactic_data(*resources_.filesystem, *resources_.catalog);
    if (!loaded) {
        failure_ = core::format_diagnostic(loaded.error());
        return false;
    }
    tables_.emplace(std::move(loaded).value());

    // WGP-65/66/40: the facts of every authored entry and playable faction; titles are localized
    // here so the model's sort sees the display names.
    std::vector<model::CampaignFacts> campaigns;
    for (const data::galactic::Campaign& campaign : tables_->campaigns) {
        model::CampaignFacts facts;
        facts.name = campaign.name;
        facts.campaign_set = campaign.campaign_set;
        facts.faction = campaign.starting_active_player;
        facts.title = campaign.text_id.empty() ? campaign.name : caption(trim(campaign.text_id));
        facts.description = campaign.description_text.empty()
            ? std::string{} : with_line_breaks(caption(trim(campaign.description_text)));
        facts.sort_order = campaign.sort_order.value_or(1000);
        facts.listed = campaign.is_listed.value_or(true);
        facts.multiplayer = campaign.is_multiplayer.value_or(false);
        facts.tutorial = campaign.tutorial.value_or(false);
        facts.supports_custom_settings = campaign.supports_custom_settings.value_or(true);
        const auto authored = [&](const auto& records, const std::int64_t fallback) {
            for (const auto& row : records)
                if (row.faction.size() == campaign.starting_active_player.size()
                    && std::equal(row.faction.begin(), row.faction.end(), campaign.starting_active_player.begin(),
                        [](const char a, const char b) { return std::tolower(static_cast<unsigned char>(a))
                            == std::tolower(static_cast<unsigned char>(b)); })) return row.value;
            return fallback;
        };
        facts.starting_credits = authored(campaign.starting_credits, 0);
        facts.starting_tech = authored(campaign.starting_tech_levels, 0);
        facts.maximum_tech = authored(campaign.max_tech_levels, 5);
        facts.minimum_custom_tech = campaign.minimum_custom_tech.value_or(1);
        facts.maximum_custom_tech = campaign.maximum_custom_tech.value_or(5);
        campaigns.push_back(std::move(facts));
    }
    std::vector<model::FactionFacts> factions;
    for (const data::galactic::Faction& faction : tables_->factions) {
        model::FactionFacts facts;
        facts.name = faction.name;
        facts.playable = faction.is_playable.value_or(false);
        facts.displayed_tech_adjustment = faction.displayed_tech_level_adjustment.value_or(0);
        const std::string key = localized_key(*resources_.catalog, faction.name, data::Category::faction, "Text_ID");
        facts.title = key.empty() ? faction.name : caption(key);
        factions.push_back(std::move(facts));
    }
    model_ = std::make_unique<model::CampaignSetupModel>(std::move(campaigns), std::move(factions), support);

    const data::ui::Dialog* found = resources_.dialogs->script.find(std::string(model::campaign_staging_dialog));
    if (found == nullptr) {
        failure_ = std::string(model::campaign_staging_dialog) + " is not in the catalogue (rejected or removed by the data)";
        return false;
    }
    built_.emplace(build_dialog(*found, *resources_.theme_model, resources_.text, model::Placement::centre));
    // The skin's tooltips go through the database like the captions.
    for (std::size_t i = 0; i < found->controls.size() && i < built_->gadgets.size(); ++i) {
        const auto* tip = resources_.dialogs->tooltip(found->controls[i]);
        if (tip != nullptr && built_->gadgets[i].control != nullptr)
            built_->gadgets[i].control->set_tooltip_text(text(caption(tip->text_id)));
    }
    // A group box draws its border only: the builder's frame has an interior fill, so it goes behind
    // the controls it surrounds (the resource lists it last).
    for (const BuiltGadget& item : built_->gadgets) {
        if (item.kind == "group" && item.control != nullptr) built_->frame->move_child(item.control, 0);
    }
    root.add_child(built_->frame);
    built_->frame->hide();

    // WGP-190: reuse the retail resource, controls and localized captions.
    if (const auto* advanced = resources_.dialogs->script.find("IDD_CAMPAIGN_ADVANCED_OPTIONS_DIALOG")) {
        advanced_.emplace(build_dialog(*advanced, *resources_.theme_model, resources_.text, model::Placement::centre));
        for (auto& item : advanced_->gadgets) {
            if (item.kind == "group" && item.control) advanced_->frame->move_child(item.control, 0);
            if (auto* button = Object::cast_to<BaseButton>(item.control)) button->set_toggle_mode(true);
            if (item.id == "IDC_ALLOW_RANDOM_EVENTS" && item.control) item.control->hide();
            if (item.id == "IDC_ALLOW_AUTORESOLVE_COMBO") {
                if (auto* combo = Object::cast_to<OptionButton>(item.control)) {
                    combo->add_item(text(caption("TEXT_ALLOW_AUTORESOLVE_ASK")));
                    combo->set_disabled(true);
                    combo->set_tooltip_text("This campaign uses the authored auto-resolve policy.");
                }
            }
            if (item.id == "IDC_SKIRMISH_CREDITS_SLIDER" && item.control) {
                item.control->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                item.control->set_tooltip_text("Starting credits use this faction's campaign default.");
                if (auto* slider = Object::cast_to<EawrUiSlider>(item.control))
                    slider->set_forced_states(KitState::disabled, KitState::disabled);
            }
        }
        root.add_child(advanced_->frame);
        advanced_->frame->hide();
    }

    list_ = gadget("IDC_STAGING_MAP_LIST");
    faction_combo_ = gadget("IDC_FACTION_COMBO");
    description_ = gadget("IDC_CAMPAIGN_DESCRIPTION");
    start_ = gadget("IDC_PLAY_BUTTON");
    options_ = gadget("IDC_SELECT_OPTIONS");
    cancel_ = gadget("IDC_CANCEL_BUTTON");
    official_ = gadget("IDC_SELECT_OFFICIAL_MAPS");
    custom_ = gadget("IDC_SELECT_CUSTOM_MAPS");
    radios_[0] = gadget("IDC_RADIO_CUSTOM_EASY");
    radios_[1] = gadget("IDC_RADIO_CUSTOM_MEDIUM");
    radios_[2] = gadget("IDC_RADIO_CUSTOM_HARD");
    for (const char* needed : {"IDC_STAGING_MAP_LIST", "IDC_FACTION_COMBO", "IDC_PLAY_BUTTON", "IDC_CANCEL_BUTTON"}) {
        if (gadget(needed) == nullptr) {
            failure_ = std::string("the campaign staging dialog has no ") + needed;
            return false;
        }
    }
    if (Object::cast_to<ItemList>(list_) == nullptr || Object::cast_to<OptionButton>(faction_combo_) == nullptr
        || Object::cast_to<BaseButton>(start_) == nullptr || Object::cast_to<BaseButton>(cancel_) == nullptr) {
        failure_ = "the campaign staging dialog's list, faction drop-down or buttons are not the kinds the screen drives";
        return false;
    }
    // Polled each frame: a click presses and releases within one input flush.
    for (Control* button : {start_, options_, cancel_, official_, custom_}) {
        if (auto* base = Object::cast_to<BaseButton>(button)) base->set_toggle_mode(true);
    }
    for (Control* radio : radios_) {
        if (auto* base = Object::cast_to<BaseButton>(radio)) base->set_toggle_mode(true);
    }
    // WGP-40: the official maps button is the shown choice and the custom maps button is off.
    if (auto* base = Object::cast_to<BaseButton>(official_)) base->set_pressed_no_signal(true);
    if (auto* base = Object::cast_to<BaseButton>(custom_)) {
        base->set_disabled(true);
        base->set_tooltip_text("Custom maps do not apply to galactic conquest campaigns.");
    }

    // The preview fills the dialog's image slot (the builder leaves the slot's rectangle).
    if (const BuiltGadget* image = [&]() -> const BuiltGadget* {
            for (const BuiltGadget& item : built_->gadgets) if (item.id == "IDC_IMAGE") return &item;
            return nullptr; }()) {
        const float x = static_cast<float>(image->rect.x), y = static_cast<float>(image->rect.y);
        const float w = static_cast<float>(image->rect.width), h = static_cast<float>(image->rect.height);
        auto* frame = memnew(EawrUiFrame);
        frame->set_name("EawrCampaignPreviewFrame");
        frame->set_small(true);
        frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        frame->set_position(Vector2(x, y));
        frame->set_size(Vector2(w, h));
        frame->hide();
        root.add_child(frame);
        preview_ = frame;
        preview_image_ = memnew(TextureRect);
        preview_image_->set_name("EawrCampaignPreview");
        preview_image_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        preview_image_->set_stretch_mode(TextureRect::STRETCH_SCALE);
        preview_image_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        preview_image_->set_position(Vector2(2, 2));
        preview_image_->set_size(Vector2(std::max(0.0F, w - 4.0F), std::max(0.0F, h - 4.0F)));
        frame->add_child(preview_image_);
        preview_blips_ = memnew(Control);
        preview_blips_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        preview_blips_->set_position(preview_image_->get_position());
        preview_blips_->set_size(preview_image_->get_size());
        frame->add_child(preview_blips_);
        std::string stem(preview_backdrop);
        for (const char* extension : {".tga", ".dds"}) {
            auto texture = assets::load_texture(*resources_.filesystem, "data/art/textures/" + stem + extension);
            if (!texture) continue;
            std::string problem;
            Ref<Image> picture = texture_image(texture.value(), problem);
            if (picture.is_valid()) {
                preview_image_->set_texture(ImageTexture::create_from_image(picture));
                break;
            }
        }
    }

    rebuild_model_rows();
    return true;
}

Control* CampaignSetupScreen::gadget(const std::string_view id) const {
    if (model_ && model_->options_open() && advanced_) {
        for (const auto& item : advanced_->gadgets) if (item.id == id) return item.control;
        return nullptr;
    }
    if (!built_) return nullptr;
    for (const BuiltGadget& item : built_->gadgets) {
        if (item.id == id) return item.control;
    }
    return nullptr;
}

std::size_t CampaignSetupScreen::listed_rows() const noexcept { return model_ ? model_->rows().size() : 0; }

// The list and the drop-down hold the model's rows.
void CampaignSetupScreen::rebuild_model_rows() {
    auto* list = Object::cast_to<ItemList>(list_);
    list->clear();
    for (const model::CampaignRow& row : model_->rows()) list->add_item(text(row.title));
    auto* combo = Object::cast_to<OptionButton>(faction_combo_);
    combo->clear();
    for (const model::FactionChoice& faction : model_->factions()) combo->add_item(text(faction.title));
    shown_row_ = -2;
    shown_faction_ = -2;
}

void CampaignSetupScreen::open() {
    if (!model_) return;
    // WGP-40/66: nothing selected, normal difficulty, the last faction.
    model_->reset();
    pending_.reset();
    preview_set_.clear();
    rebuild_model_rows();
    opened_ = true;
    if (built_) built_->frame->show();
    if (preview_ != nullptr) preview_->show();
    refresh();
}

void CampaignSetupScreen::close() {
    opened_ = false;
    if (built_) built_->frame->hide();
    if (preview_ != nullptr) preview_->hide();
    if (advanced_) advanced_->frame->hide();
    if (model_) model_->close_options(false);
}

bool CampaignSetupScreen::visible() const noexcept { return opened_ && built_; }

void CampaignSetupScreen::refresh() {
    auto* list = Object::cast_to<ItemList>(list_);
    auto* combo = Object::cast_to<OptionButton>(faction_combo_);
    if (model_->selected_row()) list->select(static_cast<int32_t>(*model_->selected_row()));
    else list->deselect_all();
    if (model_->selected_faction()) combo->select(static_cast<int32_t>(*model_->selected_faction()));
    shown_row_ = model_->selected_row() ? static_cast<int>(*model_->selected_row()) : -1;
    shown_faction_ = model_->selected_faction() ? static_cast<int>(*model_->selected_faction()) : -1;
    if (auto* label = Object::cast_to<EawrUiLabel>(description_)) label->set_text(text(std::string(model_->description())));

    // WGP-67: the difficulty radios, the start and options buttons follow the choices.
    constexpr std::array<model::CampaignDifficulty, 3> levels{model::CampaignDifficulty::easy,
        model::CampaignDifficulty::normal, model::CampaignDifficulty::hard};
    for (std::size_t i = 0; i < levels.size(); ++i) {
        auto* radio = Object::cast_to<BaseButton>(radios_[i]);
        if (radio == nullptr) continue;
        const bool available = model_->difficulty_available(levels[i]);
        radio->set_disabled(!available);
        radio->set_tooltip_text(text(model_->difficulty_reason(levels[i])));
        radio_shown_[i] = model_->difficulty() == levels[i];
        radio->set_pressed_no_signal(radio_shown_[i]);
    }
    if (auto* start = Object::cast_to<BaseButton>(start_)) {
        start->set_disabled(!model_->can_start());
        start->set_tooltip_text(text(model_->start_disabled_reason()));
        start_shown_ = model_->can_start();
    }
    if (auto* options = Object::cast_to<BaseButton>(options_)) {
        options->set_disabled(!model_->options_available() || !advanced_);
        options->set_tooltip_text(text(model_->options_disabled_reason()));
    }
    refresh_preview();
}

// WGP-73: the backdrop with a blip at each planet of the selected campaign's authored locations.
void CampaignSetupScreen::refresh_preview() {
    if (preview_blips_ == nullptr || !tables_) return;
    std::string wanted;
    if (const auto* row = model_->selected()) wanted = model_->campaigns()[row->campaign].name;
    if (wanted == preview_set_ && !wanted.empty()) return;
    preview_set_ = wanted;
    while (preview_blips_->get_child_count() > 0) {
        Node* child = preview_blips_->get_child(0);
        preview_blips_->remove_child(child);
        memdelete(child);
    }
    if (wanted.empty()) return;
    const data::galactic::Campaign* campaign = tables_->campaign(wanted);
    if (campaign == nullptr) return;
    // The neutral faction's colour; without one the blips are a plain grey.
    Color colour(0.7F, 0.7F, 0.7F, 1.0F);
    for (const data::galactic::Faction& faction : tables_->factions) {
        if (!faction.is_neutral.value_or(false)) continue;
        const std::string raw = localized_key(*resources_.catalog, faction.name, data::Category::faction, "Color");
        if (const auto parsed = parse_colour(raw)) colour = *parsed;
        break;
    }
    const Vector2 size = preview_blips_->get_size();
    const auto number = [](const sim::math::Fixed value) {
        return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
    };
    for (const std::string& name : campaign->planets) {
        const data::galactic::Planet* planet = tables_->planet(name);
        if (planet == nullptr || !planet->galactic_position) continue;
        const double nx = std::clamp((number(planet->galactic_position->x) + blip_x_origin) / blip_x_span, 0.0, 1.0);
        const double ny = std::clamp((number(planet->galactic_position->y) + blip_y_origin) / blip_y_span, 0.0, 1.0);
        auto* blip = memnew(ColorRect);
        blip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        blip->set_color(colour);
        blip->set_size(Vector2(blip_size, blip_size));
        // The radar's Y runs the other way, and each icon is centred on its point.
        blip->set_position(Vector2(static_cast<float>(nx) * size.x - blip_size * 0.5F,
                                   static_cast<float>(1.0 - ny) * size.y - blip_size * 0.5F));
        preview_blips_->add_child(blip);
    }
}

CampaignSetupScreen::Action CampaignSetupScreen::poll() {
    if (!visible() || !model_) return Action::none;
    if (model_->options_open()) {
        auto* starting = Object::cast_to<EawrUiSlider>(gadget("IDC_SKIRMISH_STARTING_TECH_LEVEL_SLIDER"));
        auto* maximum = Object::cast_to<EawrUiSlider>(gadget("IDC_SKIRMISH_MAX_TECH_LEVEL_SLIDER"));
        bool changed = false;
        if (starting && (starting->edit_revision() != shown_starting_revision_ || starting->get_value() != shown_starting_tech_)) {
            changed = model_->select_starting_tech(static_cast<std::int64_t>(std::llround(starting->get_value())));
        }
        if (maximum && (maximum->edit_revision() != shown_maximum_revision_ || maximum->get_value() != shown_maximum_tech_)) {
            changed = model_->select_maximum_tech(static_cast<std::int64_t>(std::llround(maximum->get_value()))) || changed;
        }
        const auto pressed = [&](const char* id) {
            auto* button = Object::cast_to<BaseButton>(gadget(id));
            if (!button || !button->is_pressed()) return false;
            button->set_pressed_no_signal(false); return true;
        };
        if (pressed("IDC_DEFAULTS_BUTTON")) { model_->default_options(); changed = true; }
        if (changed) refresh_options();
        if (pressed("IDC_BUTTON_ACCEPT")) finish_options(true);
        else if (pressed("IDC_CANCEL_BUTTON")) finish_options(false);
        return Action::none;
    }
    auto* list = Object::cast_to<ItemList>(list_);
    auto* combo = Object::cast_to<OptionButton>(faction_combo_);
    bool changed = false;

    const PackedInt32Array selected = list->get_selected_items();
    const int row = selected.is_empty() ? -1 : static_cast<int>(selected[0]);
    if (row != shown_row_) {
        model_->select_row(row >= 0 ? std::optional<std::size_t>(static_cast<std::size_t>(row)) : std::nullopt);
        changed = true;
    }
    const int faction = static_cast<int>(combo->get_selected());
    if (faction != shown_faction_ && faction >= 0) {
        model_->select_faction(static_cast<std::size_t>(faction));
        changed = true;
    }
    constexpr std::array<model::CampaignDifficulty, 3> levels{model::CampaignDifficulty::easy,
        model::CampaignDifficulty::normal, model::CampaignDifficulty::hard};
    for (std::size_t i = 0; i < levels.size(); ++i) {
        auto* radio = Object::cast_to<BaseButton>(radios_[i]);
        if (radio == nullptr) continue;
        // A press on an unpressed radio picks it; one on the pressed radio keeps it on.
        if (radio->is_pressed() != radio_shown_[i]) {
            if (!radio_shown_[i]) static_cast<void>(model_->select_difficulty(levels[i]));
            changed = true;
        }
    }
    if (changed) refresh();

    if (auto* options = Object::cast_to<BaseButton>(options_); options && options->is_pressed()) {
        options->set_pressed_no_signal(false);
        if (advanced_ && model_->open_options()) {
            built_->frame->hide();
            if (preview_) preview_->hide();
            advanced_->frame->show(); refresh_options();
            return Action::none;
        }
    }
    for (Control* button : {official_, custom_}) {
        if (auto* base = Object::cast_to<BaseButton>(button); base != nullptr && button == official_)
            base->set_pressed_no_signal(true);
    }
    if (auto* cancel = Object::cast_to<BaseButton>(cancel_); cancel != nullptr && cancel->is_pressed()) {
        cancel->set_pressed_no_signal(false);
        return Action::back;
    }
    if (auto* start = Object::cast_to<BaseButton>(start_); start != nullptr && start->is_pressed()) {
        start->set_pressed_no_signal(false);
        if (!start->is_disabled()) {
            pending_ = model_->start_request();
            if (pending_) return Action::start;
        }
    }
    return Action::none;
}

std::optional<model::CampaignStartRequest> CampaignSetupScreen::take_start() {
    auto request = std::move(pending_);
    pending_.reset();
    return request;
}

std::string CampaignSetupScreen::summary() const {
    if (!model_) return "unavailable";
    std::string out = "rows " + std::to_string(model_->rows().size()) + "; factions "
        + std::to_string(model_->factions().size());
    out += "; row " + (model_->selected_row() ? std::to_string(*model_->selected_row()) : std::string("none"));
    out += "; faction " + (model_->selected_faction() ? model_->factions()[*model_->selected_faction()].name
                                                       : std::string("none"));
    out += "; difficulty " + std::string(model::to_string(model_->difficulty()));
    out += std::string("; start ") + (model_->can_start() ? "enabled" : "disabled");
    out += std::string("; options ") + (model_->options_available() ? "enabled" : "disabled");
    return out;
}

void CampaignSetupScreen::refresh_options() {
    const auto values = model_->tech_options();
    const auto set_slider = [&](const char* id, const std::int64_t value) {
        auto* slider = Object::cast_to<EawrUiSlider>(gadget(id));
        if (!slider) return 0.0;
        slider->set_min(static_cast<double>(model_->minimum_tech()));
        slider->set_max(static_cast<double>(model_->maximum_tech()));
        slider->set_step(1.0);
        slider->set_value_no_signal(static_cast<double>(value));
        return slider->get_value();
    };
    shown_maximum_tech_ = set_slider("IDC_SKIRMISH_MAX_TECH_LEVEL_SLIDER", values.maximum);
    shown_starting_tech_ = set_slider("IDC_SKIRMISH_STARTING_TECH_LEVEL_SLIDER",
        std::min(values.starting, static_cast<std::int64_t>(shown_maximum_tech_)));
    if (const auto* slider = Object::cast_to<EawrUiSlider>(gadget("IDC_SKIRMISH_STARTING_TECH_LEVEL_SLIDER")))
        shown_starting_revision_ = slider->edit_revision();
    if (const auto* slider = Object::cast_to<EawrUiSlider>(gadget("IDC_SKIRMISH_MAX_TECH_LEVEL_SLIDER")))
        shown_maximum_revision_ = slider->edit_revision();
    const auto label = [&](const char* id, const std::int64_t value) {
        if (auto* control = Object::cast_to<EawrUiLabel>(gadget(id))) control->set_text(text(std::to_string(value)));
    };
    label("IDC_SKIRMISH_STARTING_TECH_LEVEL_VALUE", static_cast<std::int64_t>(shown_starting_tech_));
    label("IDC_SKIRMISH_MAX_TECH_LEVEL_VALUE", static_cast<std::int64_t>(shown_maximum_tech_));
    if (const auto* variant = model_->variant()) label("IDC_SKIRMISH_CREDITS_VALUE", variant->starting_credits);
}
void CampaignSetupScreen::finish_options(const bool accept) {
    model_->close_options(accept);
    if (advanced_) advanced_->frame->hide();
    built_->frame->show();
    if (preview_) preview_->show();
    refresh();
}
bool CampaignSetupScreen::cancel_options() {
    if (!model_ || !model_->options_open()) return false;
    finish_options(false); return true;
}

} // namespace eawr::presentation::godot_backend

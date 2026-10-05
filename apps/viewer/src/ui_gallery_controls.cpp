#include "ui_gallery_internal.hpp"

namespace eawr::presentation::godot_backend {

namespace {

[[nodiscard]] String text(const std::string_view value) {
    return String::utf8(value.data(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] std::string_view state_name(const KitState state) {
    switch (state) {
    case KitState::normal: return "normal";
    case KitState::hover: return "hover";
    case KitState::pressed: return "pressed";
    case KitState::disabled: return "disabled";
    default: return "automatic";
    }
}
// --eawr-ui-hud-cards: <type>[*<count>][:<ABILITY>][@<health>][~<shield>], comma-separated; made-up
// entity IDs from 1 in order.
[[nodiscard]] std::optional<std::vector<model::CardUnit>> demo_card_units(const std::string& spec) {
    std::vector<model::CardUnit> units;
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        std::string entry = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? spec.size() + 1 : comma + 1;
        const auto take = [&entry](const char mark) -> std::optional<std::string> {
            const std::size_t at = entry.find(mark);
            if (at == std::string::npos) return std::nullopt;
            std::size_t end = entry.find_first_of("*:@~", at + 1);
            std::string value = entry.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
            entry.erase(at, end == std::string::npos ? std::string::npos : end - at);
            return value;
        };
        const auto shield = take('~');
        const auto health = take('@');
        const auto ability = take(':');
        const auto count = take('*');
        if (entry.empty()) return std::nullopt;
        std::size_t copies = 1;
        double health_value = 1.0;
        std::optional<double> shield_value;
        try {
            if (count) copies = static_cast<std::size_t>(std::stoul(*count));
            if (health) health_value = std::stod(*health);
            if (shield) shield_value = std::stod(*shield);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (copies == 0 || copies > 200 || units.size() + copies > 400) return std::nullopt;
        for (std::size_t copy = 0; copy < copies; ++copy) {
            model::CardUnit unit;
            unit.id = static_cast<sim::EntityId>(units.size() + 1);
            unit.members = {unit.id};
            unit.type = entry;
            unit.ability = ability ? model::ability_index(*ability) : model::ability_none;
            unit.health = health_value;
            unit.shield = shield_value;
            units.push_back(std::move(unit));
        }
    }
    return units;
}

} // namespace

String UiGalleryMode::State::caption(const std::string_view key) {
    if (!text_lookup) return text(key);
    return text(data::to_utf8(text_lookup->text(key)));
}

void UiGalleryMode::State::build_controls(Control& root) {
    const float k = static_cast<float>(height) / 720.0F;
    const auto at = [k](const float x, const float y, const float w, const float h) {
        return Rect2(std::round(x * k), std::round(y * k), std::round(w * k), std::round(h * k));
    };
    const auto place = [&](Control* control, const Rect2& rect, const std::string& kind, const KitState state,
                           const std::string& variation = std::string(model::theme_type)) {
        control->set_position(rect.position);
        control->set_size(rect.size);
        control->set_theme_type_variation(text(variation));
        root.add_child(control);
        samples.push_back({kind, std::string(state_name(state)), variation, control, rect, 0U});
        return control;
    };
    const auto label = [&](const String& value, const Rect2& rect, const std::string_view role,
                           const HorizontalAlignment alignment = HORIZONTAL_ALIGNMENT_LEFT,
                           const std::string& variation = std::string(model::theme_type)) {
        auto* item = memnew(EawrUiLabel);
        item->set_text(value);
        item->set_role(text(role));
        item->set_alignment(alignment);
        item->set_wrap(false);
        item->set_position(rect.position);
        item->set_size(rect.size);
        item->set_theme_type_variation(text(variation));
        root.add_child(item);
        return item;
    };

    auto* panel = memnew(EawrUiFrame);
    place(panel, at(28.0F, 28.0F, 1224.0F, 664.0F), "frame", KitState::normal);
    label(caption("TEXT_GAME_OPTIONS_TITLE") + " - UI kit (UI-06)", at(48.0F, 40.0F, 800.0F, 30.0F), "L_Text",
          HORIZONTAL_ALIGNMENT_LEFT, "EawrUi__IDC_STATIC_MEDIUM");

    const std::array<KitState, 4> states{KitState::normal, KitState::hover, KitState::pressed, KitState::disabled};
    const std::array<const char*, 4> headers{"Normal", "Mouse over", "Pressed / on", "Disabled"};
    const float column0 = 232.0F;
    const float column_step = 170.0F;
    const float cell = 156.0F;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        label(headers[index], at(column0 + column_step * static_cast<float>(index), 78.0F, cell, 16.0F),
              "Global_Default", HORIZONTAL_ALIGNMENT_CENTER);
    }
    float row = 100.0F;
    const float row_step = 46.0F;
    const auto row_label = [&](const char* name) {
        label(name, at(48.0F, row + 3.0F, 176.0F, 18.0F), "L_Text");
    };
    const auto column = [&](const std::size_t index, const float height_px, const float width_px = 156.0F) {
        return at(column0 + column_step * static_cast<float>(index), row, width_px, height_px);
    };
    const float button_height = 24.0F * 0.9375F;

    row_label("Push button");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* button = memnew(EawrUiButton);
        button->set_text(caption("TEXT_BUTTON_RESUME_GAME"));
        button->set_forced_state(states[index]);
        if (states[index] == KitState::disabled) button->set_disabled(true);
        place(button, column(index, button_height), "button", states[index]);
    }
    row += row_step;

    row_label("Button variation");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* button = memnew(EawrUiButton);
        button->set_text(caption("TEXT_BUTTON_CANCEL"));
        button->set_forced_state(states[index]);
        place(button, column(index, button_height), "button", states[index], "EawrUi__IDC_BUTTON_KICK_PLAYER0");
    }
    row += row_step;

    row_label("Check box");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* check = memnew(EawrUiCheck);
        check->set_text(caption("TEXT_CHECK_USE_MOVIE_SUBTITLES"));
        check->set_forced_state(states[index]);
        check->set_pressed(states[index] == KitState::pressed || states[index] == KitState::disabled);
        place(check, column(index, 22.0F), "check", states[index]);
    }
    row += row_step;

    row_label("Radio button");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* radio = memnew(EawrUiCheck);
        radio->set_radio(true);
        radio->set_text("Easy");
        radio->set_forced_state(states[index]);
        radio->set_pressed(states[index] == KitState::pressed || states[index] == KitState::disabled);
        place(radio, column(index, 22.0F), "radio", states[index]);
    }
    row += row_step;

    row_label("Slider");
    const std::array<std::pair<KitState, KitState>, 4> slider_states{{
        {KitState::normal, KitState::normal},
        {KitState::hover, KitState::hover},
        {KitState::pressed, KitState::pressed},
        {KitState::disabled, KitState::disabled},
    }};
    for (std::size_t index = 0; index < slider_states.size(); ++index) {
        auto* slider = memnew(EawrUiSlider);
        slider->set_max(100.0);
        slider->set_value(20.0 + 20.0 * static_cast<double>(index));
        slider->set_forced_states(slider_states[index].first, slider_states[index].second);
        place(slider, column(index, 24.0F), "slider", states[index]);
    }
    row += row_step;

    row_label("Combo box");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* combo = memnew(EawrUiCombo);
        combo->add_item("Easy AI");
        combo->add_item("Hard AI");
        combo->set_forced_state(states[index]);
        if (states[index] == KitState::disabled) combo->set_disabled(true);
        place(combo, column(index, 25.0F * 0.9375F), "combo", states[index]);
    }
    row += row_step;

    row_label("Edit box");
    const std::array<const char*, 4> edits{"Commander", "Commander", "", "Commander"};
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* edit = memnew(EawrUiEdit);
        edit->set_text(edits[index]);
        edit->set_placeholder("Player name");
        if (states[index] == KitState::disabled) edit->set_editable(false);
        place(edit, column(index, 20.0F), "edit", states[index]);
        if (states[index] == KitState::pressed) edit->grab_focus();
    }
    row += row_step;

    row_label("Progress bar");
    const std::array<double, 4> fills{0.0, 35.0, 70.0, 100.0};
    for (std::size_t index = 0; index < fills.size(); ++index) {
        auto* bar = memnew(EawrUiBar);
        bar->set_max(100.0);
        bar->set_value(fills[index]);
        place(bar, column(index, 14.0F), "bar", KitState::normal);
    }
    row += row_step;

    row_label("Text roles");
    struct Role final {
        const char* role;
        const char* sample;
        const char* variation;
        HorizontalAlignment alignment;
    };
    const std::array<Role, 4> roles{{
        {"Global_Default", "Global_Default", "EawrUi", HORIZONTAL_ALIGNMENT_LEFT},
        {"L_Text", "L_Text x1.3", "EawrUi", HORIZONTAL_ALIGNMENT_LEFT},
        {"L_Text", "Gradient", "EawrUi__IDD_PLANET_QUICK_REFERENCE", HORIZONTAL_ALIGNMENT_LEFT},
        {"Overlay_Caption_Text", "Outline", "EawrUi", HORIZONTAL_ALIGNMENT_LEFT},
    }};
    for (std::size_t index = 0; index < roles.size(); ++index) {
        auto* item = label(roles[index].sample, column(index, 22.0F), roles[index].role, roles[index].alignment,
                           roles[index].variation);
        samples.push_back({"label", roles[index].role, roles[index].variation, item, column(index, 22.0F), 0U});
    }
    row += row_step;

    // The list and a bare small frame sit in a column on the right.
    label("List box", at(930.0F, 78.0F, 290.0F, 16.0F), "Global_Default", HORIZONTAL_ALIGNMENT_CENTER);
    auto* list = memnew(EawrUiList);
    for (const char* name : {"(2) Alderaan", "(2) Bespin", "(4) Shipyards of Kuat", "(4) Tatooine Straits",
                             "(4) Utapau Insurgence", "(6) Alderaan Defense", "(6) Chaos Above Kashyyyk",
                             "(6) Coruscant Siege", "(6) High Point Felucia", "(6) Kamino Storm",
                             "(6) Resistance Over Shola", "(6) The Maw Installation", "(8) Hoth Conflict",
                             "(8) Yavin Lost", "(9) Saleucami Unseen"}) {
        list->add_item(name);
    }
    list->select(7);
    place(list, at(930.0F, 100.0F, 290.0F, 226.0F), "list", KitState::normal);
    label("Small frame", at(930.0F, 344.0F, 290.0F, 16.0F), "Global_Default", HORIZONTAL_ALIGNMENT_CENTER);
    auto* small = memnew(EawrUiFrame);
    small->set_small(true);
    place(small, at(930.0F, 366.0F, 290.0F, 60.0F), "small_frame", KitState::normal);
    label("Dialog frame: this panel", at(930.0F, 444.0F, 290.0F, 16.0F), "Global_Default",
          HORIZONTAL_ALIGNMENT_CENTER);
}

void UiGalleryMode::State::build_dialog_page(Control& root) {
    const data::Dialog* found = catalog->script.find(options.dialog);
    if (found == nullptr) {
        failure = "dialog " + options.dialog + " is not in the catalogue";
        return;
    }
    dialog.emplace(build_dialog(*found, *theme_model, text_lookup ? &*text_lookup : nullptr,
                                model::Placement::centre));
    root.add_child(dialog->frame);
    const model::PixelRect& frame = dialog->layout.frame;
    samples.push_back({"dialog", "normal", theme_model->variation(*found, nullptr), dialog->frame,
                       Rect2(static_cast<float>(frame.x), static_cast<float>(frame.y), static_cast<float>(frame.width),
                             static_cast<float>(frame.height)),
                       0U});
    for (const BuiltGadget& gadget : dialog->gadgets) {
        if (gadget.control == nullptr) continue;
        samples.push_back({gadget.kind, "normal", gadget.variation, gadget.control,
                           Rect2(static_cast<float>(gadget.rect.x), static_cast<float>(gadget.rect.y),
                                 static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)),
                           0U});
    }
}

void UiGalleryMode::State::build_hud_page(Node3D& parent) {
    TacticalHud::Options hud_options;
    const auto faction = model::hud_faction_from(options.hud);
    if (!faction) {
        failure = "--eawr-ui-hud expects empire, rebel or underworld";
        return;
    }
    hud_options.faction = *faction;
    hud_options.rules = options.rules == "retail" ? model::LayoutRules::retail : model::LayoutRules::aspect_correct;
    const std::filesystem::path directory = cache_directory();
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = directory,
                               .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto font_mount = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    const vfs::Vfs unmounted;
    hud_options.font_cache = model::load_font_cache(font_mount ? font_mount.value() : unmounted, ViewerPath::utf8(directory));
    hud_options.font_cache_source = font_cache_source;
    // The planet: the map's root field 0x09 and its Planet object (hud_shell.hpp).
    // The HUD keeps reading the objects (the unit cards' Icon_Name and Text_ID), so they live in the state.
    std::optional<eawr::data::Catalog>& objects = hud_objects;
    if (auto loaded = eawr::data::load_catalog(*filesystem, eawr::data::Profile::foc)) {
        objects.emplace(std::move(loaded.value().catalog));
    } else {
        notes.push_back("objects: " + core::format_diagnostic(loaded.error()));
    }
    std::optional<std::string> context;
    const assets::ObjectTypeCatalog types = objects ? assets::object_type_catalog(*objects) : assets::ObjectTypeCatalog{};
    if (auto map = assets::load_map(*filesystem, options.hud_map, types)) {
        context = map.value().context_name;
    } else {
        notes.push_back("--eawr-ui-hud-map: " + core::format_diagnostic(map.error()));
    }
    hud = std::make_unique<TacticalHud>(std::move(hud_options));
    if (!hud->build(*filesystem, objects ? &*objects : nullptr, context, parent)) {
        failure = "--eawr-ui-hud: " + hud->failure();
        return;
    }
    if (!options.hud_cards.empty()) {
        auto units = demo_card_units(options.hud_cards);
        if (!units) {
            failure = "--eawr-ui-hud-cards expects <type>[*<count>][:<ABILITY>][@<health>][~<shield>], comma-separated";
            return;
        }
        if (EawrUnitCards* cards = hud->unit_cards()) {
            demo_cards = std::move(*units);
            hud->set_unit_cards(model::layout_unit_cards(demo_cards, cards->slot_count()), demo_cards);
        } else {
            notes.push_back("--eawr-ui-hud-cards: the shell has no unit card slots");
        }
    }
}

void UiGalleryMode::State::build_movie_page(Control& root, const Vector2 size) {
    const float side = 2.0F * movie_slot_units * size.y / reference_height;
    const Rect2 rect((size - Vector2(side, side)) * 0.5F, Vector2(side, side));
    ColorRect* band = memnew(ColorRect);
    band->set_color(Color(0.55F, 0.58F, 0.62F));
    band->set_position(Vector2(size.x * 0.5F, 0.0F));
    band->set_size(Vector2(size.x * 0.5F, size.y));
    band->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    root.add_child(band);
    const std::u8string cache = options.movie_cache.generic_u8string();
    auto player = attach_hud_movie(root, *movie,
        String::utf8(reinterpret_cast<const char*>(cache.data()), static_cast<int64_t>(cache.size())), rect, true);
    if (!player) {
        failure = core::format_diagnostic(player.error());
        return;
    }
    movie_player = player.value();
    movie_deadline = std::chrono::steady_clock::now() + movie_minimum_time;
}

void UiGalleryMode::State::sample_movie() {
    const double position = movie_player->get_stream_position();
    if (position < movie_last_position) ++movie_loops;
    movie_last_position = position;
    if (movie_ticks++ % 10 != 0) return;
    const Ref<Texture2D> texture = movie_player->get_video_texture();
    if (texture.is_null()) return;
    const Ref<Image> frame = texture->get_image();
    if (frame.is_null() || frame->is_empty()) return;
    const PackedByteArray bytes = frame->get_data();
    movie_frame_hashes.insert(core::sha256_hex({bytes.ptr(), static_cast<std::size_t>(bytes.size())}));
}
} // namespace eawr::presentation::godot_backend

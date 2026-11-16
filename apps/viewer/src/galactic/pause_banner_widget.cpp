#include "pause_banner_widget.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/battle_overlay.hpp"
#include "ui/font_provider.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include <cctype>

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace eawr::presentation::godot_backend {
namespace model = presentation::ui;

namespace {
godot::Color colour_of(const data::ui::Rgba8 value) {
    return godot::Color(static_cast<float>(value.r) / 255.0F, static_cast<float>(value.g) / 255.0F,
        static_cast<float>(value.b) / 255.0F, static_cast<float>(value.a) / 255.0F);
}
} // namespace

// The pause part of the tactical HUD's battle overlay, built from the galactic HUD's own catalog, fonts and textures:
// TM-09 (the pause shell, its text and button anchors, the component fonts, Battle_Pending_Message_Color, the texts
// from the text database, the Resume button's rollover art).
void PauseBannerWidget::build(GalacticMode::State& state) {
    tried_ = true;
    auto database = data::ui::load_language_text_database(*state.filesystem, "english");
    std::shared_ptr<data::ui::TextDatabase> text;
    if (database) text = std::make_shared<data::ui::TextDatabase>(std::move(database).value());
    text_ = text;
    const auto game_text = [&](const std::string_view key) {
        const auto* entry = text ? text->find(key) : nullptr;
        if (!entry) godot::UtilityFunctions::push_warning(godot::String("[eawr-pause] ") + godot::String::utf8(std::string(key).c_str()) + " is not in the text DB; the key is shown");
        return godot::String::utf8((entry ? data::ui::to_utf8(entry->value) : std::string(key)).c_str());
    };
    const auto& catalog = state.command_catalog;
    const auto component_font = [&](const std::string_view name) -> std::pair<std::string, std::int32_t> {
        const auto* component = catalog.find(name);
        if (!component) return {"EmpireAtWar-Medium", 7};
        const auto faces = component->list(data::ui::Field::font_name);
        return {faces.empty() ? std::string("EmpireAtWar-Medium") : faces.front(),
                component->integer(data::ui::Field::font_point_size).value_or(7)};
    };
    const auto font_of = [&](const std::string& face, const std::int32_t points) {
        const auto resolved = state.hud_fonts->resolve({face, points}, "english");
        return EawrBattleOverlay::Font{state.hud_fonts->font(resolved), resolved.point_size};
    };
    const auto looks = model::battle_message_looks(*state.filesystem);
    looks_ = looks;
    const auto* resume_component = catalog.find("attack_button");
    const data::ui::Rgba8 resume_colour = resume_component
        ? resume_component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255, 64, 64, 255})
        : data::ui::Rgba8{255, 64, 64, 255};
    EawrBattleOverlay::Style style;
    const auto [banner_face, banner_points] = component_font("text_attack");
    style.banner = font_of(banner_face, banner_points);
    style.paused = colour_of(looks.pending);
    const auto [button_face, button_points] = component_font("attack_button");
    style.button = font_of(button_face, 12);
    style.button_colour = colour_of(resume_colour);
    style.native_button = font_of(button_face, button_points);
    style.button_emboss = resume_component && resume_component->flag(data::ui::Field::text_emboss);
    style.button_outline = resume_component && resume_component->flag(data::ui::Field::text_outline);
    auto pause = model::pause_shell(*state.filesystem, catalog);
    if (!pause) {
        godot::UtilityFunctions::push_warning(godot::String("[eawr-pause] the pause shell was not built: ")
            + godot::String::utf8(core::format_diagnostic(pause.error()).c_str()));
        return;
    }
    for (const auto& anchor : pause.value().anchors.anchors()) {
        if (anchor.name == "attack_button" || anchor.bone == "attack_button") style.pause_button = anchor.rect;
        if (anchor.name == "text_attack" || anchor.bone == "text_attack") style.pause_text = anchor.rect;
        if (anchor.name == "attack_frame" && pause.value().text_origin) {
            const auto origin = *pause.value().text_origin;
            style.pause_text = data::ui::ReferenceRect{origin.x - anchor.rect.width / 2.0F, origin.y, anchor.rect.width, 0.0F};
        }
    }
    for (const auto& mesh : pause.value().meshes)
        style.pause_meshes.push_back({mesh.blend, state.command_texture(mesh.texture), mesh.triangles});
    if (resume_component) {
        const auto rollover = resume_component->list(data::ui::Field::mouse_over_texture_name);
        if (!rollover.empty() && !rollover.front().empty()) style.pause_rollover = state.command_texture(rollover.front());
    }
    style.paused_text = game_text(model::pause_text_key);
    style.resume_text = game_text(model::pause_resume_key);
    style.offer_quit = false; // the Quit Game button under a battle's banner is a project addition; the map has none
    style.space = [&state] {
        return model::reference_space({state.camera.width, state.camera.height});
    };
    overlay_ = memnew(EawrBattleOverlay);
    overlay_->set_name("galactic_pause_banner");
    overlay_->set_z_index(19);
    overlay_->set_visible(false);
    state.hud->add_child(overlay_);
    // Resume Game is admitted exactly as the clock panel's pause button is (HCB-73): a closed clock (tutorial lock, a
    // multiplayer role without the clock) refuses it.
    overlay_->setup(std::move(style), [&state] { if (state.clock_controls_enabled) state.request_pause(false); }, [] {});
}

namespace {
[[nodiscard]] std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
} // namespace

// WGEP-173: what a type authors for its build countdown: the enabling flag, the text id (looked up in the text database)
// and the colour. Read from the type's effective fields once per type.
const PauseBannerWidget::CountdownType& PauseBannerWidget::countdown_type(GalacticMode::State& state, const std::string& type) {
    const auto found = countdown_types_.find(type);
    if (found != countdown_types_.end()) return found->second;
    CountdownType result;
    if (state.audio_catalog) {
        const auto object = state.audio_catalog->resolve(type, data::Category::game_object);
        if (object) {
            const auto field = [&](const std::string_view tag) {
                const auto* entry = object.value().value(tag);
                if (!entry) return std::string{};
                data::tag_trace::used(entry->value);
                return trimmed(entry->value.raw_text);
            };
            auto flag = field("Build_Countdown_Timer");
            for (auto& c : flag) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            result.enabled = flag == "YES" || flag == "TRUE" || flag == "1";
            const auto key = field("Build_Countdown_Text_ID");
            if (const auto* entry = text_ && !key.empty() ? text_->find(key) : nullptr)
                result.label = godot::String::utf8(data::ui::to_utf8(entry->value).c_str());
            else if (!key.empty())
                result.label = godot::String::utf8(key.c_str());
            if (const auto parsed = model::parse_countdown_colour(field("Build_Countdown_Text_RGBA"))) {
                result.colour = colour_of(*parsed);
                result.coloured = true;
            }
        }
    }
    return countdown_types_.emplace(type, std::move(result)).first->second;
}

// WGEP-173: every production queue front whose type enables the countdown timer shows "<label> MM:SS" in the stacked
// strategic countdown list: all producers in planet order, both queues of each, whoever owns them and whether or not the
// local player can see them (the original's lookup passes no owner or visibility filter).
std::vector<EawrBattleOverlay::CountdownLine> PauseBannerWidget::countdown_lines(GalacticMode::State& state) {
    std::vector<EawrBattleOverlay::CountdownLine> lines;
    if (!state.game || state.tactical || state.loading) return lines;
    const auto snapshot = state.game->frame(sim::SeatId{state.galaxy->local.seat}).latest;
    if (!snapshot || !snapshot->production()) return lines;
    for (const auto& planet : snapshot->production()->planets)
        for (const auto& queue : planet.queues) {
            if (queue.empty()) continue;
            const auto& front = queue.front();
            const auto name = state.production_names.find(front.type);
            if (name == state.production_names.end()) continue;
            const auto& type = countdown_type(state, name->second);
            if (!type.enabled || !type.coloured || type.label.is_empty()) continue;
            const float remaining = static_cast<float>(static_cast<double>(front.last_remaining.raw())
                / static_cast<double>(sim::math::Fixed::scale));
            lines.push_back({godot::String::utf8(model::production_countdown_line(type.label.utf8().get_data(), remaining).c_str()),
                type.colour});
        }
    return lines;
}

void PauseBannerWidget::service(GalacticMode::State& state) {
    if (!state.hud || !state.filesystem || !state.hud_fonts || !state.galaxy) return;
    if (!overlay_ && !tried_) build(state);
    if (!overlay_) return;
    const bool shown = state.paused && !state.tactical && !state.loading;
    overlay_->show_paused(shown);
    overlay_->set_resume_enabled(state.clock_controls_enabled);
    auto lines = countdown_lines(state);
    const bool counting = !lines.empty();
    overlay_->show_countdown_lines(std::move(lines), looks_.strategic_countdown_x, looks_.strategic_countdown_y,
        looks_.strategic_countdown_spacing);
    overlay_->set_visible(shown || counting);
}

std::string PauseBannerWidget::report_json() const {
    if (!overlay_) return "null";
    return overlay_->report_json();
}

void service_pause_banner(GalacticMode::State& state) {
    if (!state.pause_banner) state.pause_banner = std::make_unique<PauseBannerWidget>();
    state.pause_banner->service(state);
}
} // namespace eawr::presentation::godot_backend

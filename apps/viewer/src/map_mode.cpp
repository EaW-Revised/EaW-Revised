#include "frame_timer.hpp"
#include "eawr/core/text.hpp"
#include "startup_trace.hpp"
#include "battle_content.hpp"
#include "map_mode.hpp"
#include "map_mode_internal.hpp"
#include "battle_audio_internal.hpp"
#include "eawr/presentation/camera/overview.hpp"
#include "render_profile_viewport.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace eawr::presentation::godot_backend {
namespace {





} // namespace

namespace map_mode_detail {

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    return core::iequals(left, right);
}

[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}


// Authored names carry the source-art suffix where the shipped asset often
// carries another, so a reference is probed as written and then by stem. This
// mirrors the asset layer's own rule rather than inventing a second one.
[[nodiscard]] std::optional<std::string> probe_reference(
    const vfs::Vfs& filesystem, const std::string_view root,
    const std::string_view name, const std::span<const std::string_view> suffixes) {
    std::string canonical;
    canonical.reserve(name.size());
    for (const char character : name) {
        const char folded = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A')) : character;
        canonical.push_back(folded == '\\' ? '/' : folded);
    }
    if (canonical.empty()) return std::nullopt;
    const std::string base = std::string(root) + canonical;
    if (filesystem.stat(base)) return base;
    std::string stem = canonical;
    for (const std::string_view suffix : suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : suffixes) {
        const std::string candidate = std::string(root) + stem + std::string(suffix);
        if (filesystem.stat(candidate)) return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] assets::Texture placeholder_texture() {
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{190}, std::byte{190}, std::byte{190}, std::byte{255}};
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

} // namespace map_mode_detail

MapMode::MapMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
MapMode::~MapMode() = default;
std::string_view MapMode::failure() const noexcept { return state_->failure; }
MapMode::MapMode(MapMode&&) noexcept = default;
MapMode& MapMode::operator=(MapMode&&) noexcept = default;
bool MapMode::briefing_queue_empty() const {
    return !state_->battle_audio || state_->battle_audio->briefing_queue_empty();
}
bool MapMode::campaign_briefing(const std::string_view event_name) {
    if (!state_->battle_audio || !state_->battle_audio->campaign_briefing(event_name)) return false;
    // SND-41/69: movie failure does not cancel accepted speech or the battle.
    if (state_->hud) static_cast<void>(state_->hud->play_command_bar_movie(event_name));
    return true;
}
bool MapMode::command_bar_movie_playing() const { return state_->hud && state_->hud->command_bar_movie_playing(); }
void MapMode::write_command_bar_movie_report(std::ostream& output) const {
    if (state_->hud) state_->hud->write_command_bar_movie_report(output); else output << "null";
}
TacticalHud* MapMode::tactical_hud() const noexcept { return state_->hud.get(); }
void MapMode::update_retreat_countdown(const sim::tactical::RetreatState& state, const std::uint64_t tick, const bool cinematic_playing) {
    if (!state_->hud) return;
    auto& cache = state_->retreat_countdown;
    if (!state_->live_session || !state_->catalog || !ui::retreat_countdown_visible(state, cinematic_playing)) {
        if (cache.shown) state_->hud->set_retreat_countdown({}, {255, 255, 255, 255});
        cache.shown = false;
        return;
    }
    if (cache.shown && cache.phase == state.phase && cache.tick == tick) return;
    const auto& faction_name = state_->live_session->player_faction(state.player);
    if (cache.faction != faction_name || cache.text.empty()) {
        cache.faction = faction_name;
        cache.presentation = battle_audio_detail::read_retreat_faction(*state_->catalog, faction_name);
        const auto& id = cache.presentation.countdown_text_id;
        cache.text = state_->hud->localized_text(id, id);
    }
    cache.shown = true;
    cache.phase = state.phase;
    cache.tick = tick;
    state_->hud->set_retreat_countdown(
        ui::retreat_countdown_line(cache.text, state, tick, sim::tactical::logical_frames_per_second), cache.presentation.colour);
}

void MapMode::retreat_refused(const sim::tactical::RetreatRefusal refusal) {
    if (state_->battle_audio) state_->battle_audio->retreat_refused(refusal);
}
void MapMode::set_menu_handler(std::function<void()> handler) { state_->menu_handler = std::move(handler); }
bool MapMode::leave_to_menu() const noexcept { return state_->leave_to_menu; }



} // namespace eawr::presentation::godot_backend

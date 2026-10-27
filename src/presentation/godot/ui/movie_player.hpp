#pragma once

// HUD movie playback (G12, #237; docs/ui/hud-movies.md). Plays the Theora
// entry the player converted for a resolved movie (data/ui/movie.hpp) with
// Godot's built-in VideoStreamTheora. An Alpha movie is stored as colour in
// the left half and opacity in the right half of each frame; a canvas shader
// on the player composites it with straight alpha.
//
// The shell (#83) owns placement and lifetime: it passes the slot's rect
// (Movie_tactical / Movie_galactic, moved by the movie's Commandbar_Offset),
// keeps the returned node, and stops or hides it for STOP_COMMANDBAR_MOVIE.
// The cache entry is a file the player made, outside the game data, so it is
// read through Godot's FileAccess, as VideoStreamTheora reads it.

#include "eawr/core/result.hpp"
#include "eawr/data/ui/movie.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/video_stream_player.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
#include <optional>
#include <ostream>
#include <cstdint>
#include <chrono>

namespace eawr::presentation::godot_backend {

// Adds a playing VideoStreamPlayer for `movie` under `parent` (which owns it),
// filling `rect`, ignoring the mouse, muted, looping when `loop` is set.
// `cache_directory` is a native path or a user:// one. Errors: EAWR-UI-0705
// when the cache has no entry for the movie's bytes (tools/ui/
// convert_hud_movie.py makes it), EAWR-UI-0708 when the entry is not Ogg.
[[nodiscard]] core::Result<godot::VideoStreamPlayer*> attach_hud_movie(godot::Control& parent,
    const data::ui::HudMovie& movie, const godot::String& cache_directory, const godot::Rect2& rect, bool loop);

// EAWR-UI-0708 unless the player has decoded a frame. Call it once the
// player has had a few frames; a stream Godot cannot decode never gets one.
[[nodiscard]] core::Result<void> validate_hud_movie(const godot::VideoStreamPlayer& player,
    const data::ui::HudMovie& movie);

// SND-69/G12: reusable command-bar consumer. The caller supplies an authored
// request, shell bone and speech-presence/pause facts, never a live session.
class CommandBarMovie final {
public:
    CommandBarMovie() = default;
    ~CommandBarMovie();
    CommandBarMovie(const CommandBarMovie&) = delete;
    CommandBarMovie& operator=(const CommandBarMovie&) = delete;
    void prepare(const vfs::Vfs&, std::string_view movie, presentation::ui::ReferencePoint bone, godot::Control& parent);
    // Loading-only callback; true once two decoded frames have actually drawn.
    [[nodiscard]] bool warmup();
    [[nodiscard]] bool start(godot::Control& parent, std::string_view speech);
    void service(bool speech_present, bool paused, const presentation::ui::ShellPlacement& placement);
    void stop();
    [[nodiscard]] bool active() const { return player() != nullptr; }
    [[nodiscard]] bool playing() const;
    [[nodiscard]] const std::string& speech() const { return speech_; }
    void write_report(std::ostream&) const;
private:
    [[nodiscard]] godot::VideoStreamPlayer* player() const;
    void release_prepared();
    std::optional<data::ui::HudMovie> movie_;
    data::ui::CommandBarMovieStyle style_;
    presentation::ui::ReferencePoint bone_;
    std::int64_t player_id_{};
    std::int64_t prepared_player_id_{};
    std::chrono::steady_clock::time_point serviced_at_;
    double decode_seconds_{};
    bool decoded_{};
    std::uint64_t warmup_first_draw_{};
    bool warming_{};
    godot::String cache_;
    std::string speech_, problem_, requested_movie_, requested_speech_;
};

} // namespace eawr::presentation::godot_backend

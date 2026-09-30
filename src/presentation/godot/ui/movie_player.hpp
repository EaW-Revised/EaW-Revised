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

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/video_stream_player.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>

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

} // namespace eawr::presentation::godot_backend

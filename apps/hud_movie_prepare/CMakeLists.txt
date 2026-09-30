# The VFS half of the user-side HUD movie conversion (docs/ui/hud-movies.md);
# tools/ui/convert_hud_movie.py drives it and the player's FFmpeg.
add_executable(hud_movie_prepare main.cpp)
target_link_libraries(hud_movie_prepare PRIVATE eawr::ui_data eawr::vfs eawr::core)
target_compile_features(hud_movie_prepare PRIVATE cxx_std_20)
eawr_target_project_warnings(hud_movie_prepare)

# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_UI_MODEL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/layout.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/fonts.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/hud.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/hud_shell.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shell_alpha.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/theme.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ability_buttons.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/battle_messages.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/battle_results.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/command_sink.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/input_routing.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/minimap.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/perf_stats.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/selection.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/time_controls.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/production.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_cards.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/world_ui.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/cursors.cpp"
)

# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_UI_DATA_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/text_database.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/dialog_catalog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/dialog_script.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/dialog_script_tokens.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/dialog_script_parser.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/resource_symbols.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/command_bar.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shell_anchors.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sfnt.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/movie.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/cursors.cpp"
)

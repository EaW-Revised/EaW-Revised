# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_ASSETS_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/animation.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/map.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/map_decode.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/map_references.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/mega_texture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/model.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/texture.cpp"
)

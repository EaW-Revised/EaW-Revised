# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_SCENE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/idle_tags.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/scene.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/scene_assets.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/scene_build.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/scene_evidence.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space_population.cpp"
)

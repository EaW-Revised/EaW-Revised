# Galactic conquest data loaders (docs/galactic-data.md), part of eawr_data.
# Included directly by the root library and viewer; paths are absolute so both
# can both consume the list.

set(EAWR_GALACTIC_DATA_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/galactic_campaigns.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/galactic_data.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/galactic_world.cpp"
)

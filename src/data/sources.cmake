# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_DATA_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tag_trace.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/xml.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/xml_merge.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/xml_override.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/xml_registry.cpp"
)

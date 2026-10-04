# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.
# ROOT_ONLY lists preserve the root build's existing additional units.

set(EAWR_CORE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/diagnostic.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sha256.cpp"
)

set(EAWR_CORE_ROOT_ONLY_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/warning_probe.cpp"
)

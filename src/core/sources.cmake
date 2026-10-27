# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.
# ROOT_ONLY lists preserve the root build's existing additional units.

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/EawrBuildIdentity.cmake")

set(EAWR_CORE_SOURCES
    "${EAWR_BUILD_IDENTITY_CPP}"
    "${CMAKE_CURRENT_LIST_DIR}/diagnostic.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/load_note.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/json_text.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sha256.cpp"
)

set(EAWR_CORE_ROOT_ONLY_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/warning_probe.cpp"
)

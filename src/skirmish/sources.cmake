# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_SKIRMISH_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/ai.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/census.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/content.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/economy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/inputs.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/melee.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/placement.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/roster_gate.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/setup.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/start.cpp"
)

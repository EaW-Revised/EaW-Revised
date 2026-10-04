# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.
# ROOT_ONLY lists preserve the root build's existing additional units.

set(EAWR_PLATFORM_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/live_session.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sim_workers.cpp"
)

set(EAWR_PLATFORM_ROOT_ONLY_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/executable.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/publish_files.cpp"
)

set(EAWR_PLATFORM_LIVE_AI_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/live_ai.cpp"
)

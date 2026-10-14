# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.
# ROOT_ONLY lists preserve the root build's existing additional units.

include("${CMAKE_CURRENT_LIST_DIR}/galactic/sources.cmake")

set(EAWR_PLATFORM_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/live_session.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sim_workers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/publish_files.cpp"
)

set(EAWR_PLATFORM_ROOT_ONLY_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/executable.cpp"
)

set(EAWR_PLATFORM_LIVE_AI_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/live_ai.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/battle_scoring.cpp"
)

# Personal UI stores, kept separate from the session platform library to avoid a link cycle.
set(EAWR_PLATFORM_UI_FILE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/ui_files.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/replay_store.cpp"
)

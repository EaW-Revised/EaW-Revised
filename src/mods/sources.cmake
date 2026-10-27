# Shared translation units of the mod core. Paths are absolute so other projects can include this list.
# EAWR_MODS_STEAM_SOURCES use the standard library only: EAWR.exe (apps/launcher) compiles them itself with
# its static C runtime, so keep them free of other EAWR libraries.

set(EAWR_MODS_STEAM_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/steam/library_folders.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/steam/paths.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/steam/text_formats.cpp"
)

set(EAWR_MODS_SOURCES
    ${EAWR_MODS_STEAM_SOURCES}
    "${CMAKE_CURRENT_LIST_DIR}/chain.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/discovery.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/fingerprint.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/file_index.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/file_index_build.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/profile.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/record.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shader_translation.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/workshop.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/workshop_watch.cpp"
)
if(WIN32)
    list(APPEND EAWR_MODS_SOURCES "${CMAKE_CURRENT_LIST_DIR}/profile_windows.cpp")
endif()

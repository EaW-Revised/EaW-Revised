# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.
# ROOT_ONLY lists preserve the root build's existing additional units.

include("${CMAKE_CURRENT_LIST_DIR}/galactic/sources.cmake")

set(EAWR_PLATFORM_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/live_session.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/sim_workers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/publish_files.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/load_notes.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/degradation_summary.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/land_debug_overlay.cpp"
)

set(EAWR_PLATFORM_ROOT_ONLY_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/executable.cpp"
)

# The model facts cache the unit-table loads use (what the loader keeps from each model, kept in a DerivedStore). Its own
# library: it needs units, scene and the VFS, which eawr_platform does not link.
set(EAWR_PLATFORM_MODEL_FACTS_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/model_facts.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/setup_map_facts.cpp"
)
# Plus the generated source that defines the facts code id (the epoch's hash of the files that decide a record's content;
# cmake/EawrFactsCodeId.cmake). The source inventory scripts only evaluate the list, so they get the path and no rule.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/EawrFactsCodeId.cmake")
if(EAWR_SOURCE_INVENTORY)
    set(EAWR_FACTS_CODE_ID_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/facts_code_id.cpp")
else()
    eawr_facts_code_id_source(EAWR_FACTS_CODE_ID_SOURCE)
endif()
list(APPEND EAWR_PLATFORM_MODEL_FACTS_SOURCES "${EAWR_FACTS_CODE_ID_SOURCE}")

set(EAWR_PLATFORM_LIVE_AI_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/live_ai.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/battle_scoring.cpp"
)

# Personal UI stores, kept separate from the session platform library to avoid a link cycle.
set(EAWR_PLATFORM_UI_FILE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/ui_files.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/replay_store.cpp"
)

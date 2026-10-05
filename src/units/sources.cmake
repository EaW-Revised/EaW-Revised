# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.

set(EAWR_UNITS_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/unit_abilities.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_combat.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_durability.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_identity.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_motion.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_priority.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_support.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_tables.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_tables_decode.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unit_tables_profiles.cpp"
)

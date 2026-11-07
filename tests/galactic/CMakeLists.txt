# M3 galactic tests (docs/galactic-architecture.md, testing).
include(EawrTests)
include("${CMAKE_CURRENT_SOURCE_DIR}/AreaTests.cmake")

# Numeric prefixes preserve registration order, the tier boundary and late
# cross-target properties. New independent cases may use any unused prefix.
# include(), rather than add_subdirectory(), keeps source/binary paths unchanged.
# In fragments, use CMAKE_CURRENT_SOURCE_DIR/BINARY_DIR; LIST_DIR/FILE refer to cases/.
file(GLOB galactic_test_cases CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cases/*.cmake")
list(SORT galactic_test_cases)
foreach(galactic_test_case IN LISTS galactic_test_cases)
    include("${galactic_test_case}")
endforeach()

# Dispatchers are emitted after every fragment has registered its area cases.
eawr_finish_galactic_areas()

# Explicit format-upgrade operation; never part of ALL or ordinary CTest.
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
add_custom_target(galactic_corpus_upgrade
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/replay/upgrade_galactic_corpus.py"
        --root "${PROJECT_SOURCE_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/corpus-upgrade.zip"
        --basic "$<TARGET_FILE:galactic_replay_corpus_generate>"
        --economy "$<TARGET_FILE:galactic_area_campaign_economy>"
        --transitions "$<TARGET_FILE:galactic_area_campaign_transitions>"
        --checkpoint "$<TARGET_FILE:galactic_area_checkpoint>"
        --control "$<TARGET_FILE:galactic_control_tests>"
    DEPENDS galactic_replay_corpus_generate galactic_area_campaign_economy
        galactic_area_campaign_transitions galactic_area_checkpoint galactic_control_tests
    VERBATIM
)

# Read-only dry run of the same upgrade (tools/gc_quality/corpus_upgrade.py): regenerates the corpus twice into a staging
# folder, requires byte-identical output and, until a format bump lands, a corpus equal to the checked-in one. A bump
# slice replaces --expect clean by --expect moved for its own review run and lists what moved from report.md.
add_test(NAME galactic_corpus_upgrade_dry_run
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gc_quality/corpus_upgrade.py"
        --root "${PROJECT_SOURCE_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/corpus-dry-run" --twice --expect clean
        --basic "$<TARGET_FILE:galactic_replay_corpus_generate>"
        --economy "$<TARGET_FILE:galactic_area_campaign_economy>"
        --transitions "$<TARGET_FILE:galactic_area_campaign_transitions>"
        --checkpoint "$<TARGET_FILE:galactic_area_checkpoint>"
        --control "$<TARGET_FILE:galactic_control_tests>")
set_tests_properties(galactic_corpus_upgrade_dry_run PROPERTIES LABELS "corpus;batch-only" TIMEOUT 1800)

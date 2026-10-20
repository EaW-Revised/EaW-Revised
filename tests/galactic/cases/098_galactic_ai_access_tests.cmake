eawr_add_galactic_area_case(
    AREA galactic_area_ai_access
    NO_ARGS

    NAME galactic_ai_access_tests
    SOURCES ai_access_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ai_access_contracts
)

set_tests_properties(galactic_ai_access_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")

# S9-early (GC AI redesign): per-player galactic random streams and identity ranges, and the
# partition-order tripwire over the galactic AI barrier. Synthetic fixtures, no game data.

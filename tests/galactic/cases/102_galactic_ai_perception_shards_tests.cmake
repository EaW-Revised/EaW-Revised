eawr_add_galactic_area_case(
    AREA galactic_area_ai_components
    NO_ARGS

    NAME galactic_ai_perception_shards_tests
    SOURCES ai_perception_shards_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::platform
    TESTS galactic_ai_perception_shards_contracts
)

# Retail-cadence 600-frame campaign on the installed game: per-tick hashes and committed events equal on
# 1/2/4/8 workers, and a save at the window midpoint restored on a different worker count continues equal.
eawr_add_game_data_test(NAME galactic_ai_s9_retail600_stock COMMAND galactic_ai_campaign_tests --live-cadence
    "${CMAKE_CURRENT_BINARY_DIR}/s9-retail600" 600)
set_tests_properties(galactic_ai_s9_retail600_stock PROPERTIES LABELS game-data TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

# GC AI S3a: the world reader's structural index, fog bits and double-buffered view (synthetic, 1/2/4/8 workers).

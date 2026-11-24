eawr_add_test_case(
    NAME galactic_corruption_actor_matrix_stock_tests
    SOURCES corruption_actor_matrix_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
)

eawr_add_game_data_test(NAME galactic_corruption_actor_matrix_stock COMMAND galactic_corruption_actor_matrix_stock_tests)
set_tests_properties(galactic_corruption_actor_matrix_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")

# The same matrix over the installed Republic at War mod: its diplomat is a hero company whose member carries the
# corruption ability, so the drop preview must offer corruption for it in orbit and on the surface.
eawr_add_game_data_test(NAME galactic_corruption_actor_matrix_raw COMMAND galactic_corruption_actor_matrix_stock_tests)
set_tests_properties(galactic_corruption_actor_matrix_raw PROPERTIES LABELS "game-data;mod-data" SKIP_REGULAR_EXPRESSION "SKIPPED:" SKIP_RETURN_CODE 77
    TIMEOUT 3600 ENVIRONMENT "EAWR_ACTOR_MATRIX_MOD_ID=1129810972;EAWR_ACTOR_MATRIX_CAMPAIGN_FILE=Campaign_Republic_at_War_Clone_Wars.xml;EAWR_ACTOR_MATRIX_CAMPAIGN=Sandbox_The_Clone_Wars_Empire")

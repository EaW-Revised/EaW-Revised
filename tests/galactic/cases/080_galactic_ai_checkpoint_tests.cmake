eawr_add_galactic_area_case(
    AREA galactic_area_checkpoint

    NAME galactic_ai_checkpoint_tests
    SOURCES ai_checkpoint_stock_tests.cpp
    LIBS eawr::checkpoint eawr::platform
)

add_test(NAME galactic_ai_checkpoint_contracts COMMAND galactic_area_checkpoint galactic_ai_checkpoint_tests "${CMAKE_CURRENT_BINARY_DIR}/native-ai-saves")

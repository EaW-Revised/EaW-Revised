eawr_add_test_case(
    NAME galactic_campaign_scoring_tests
    SOURCES campaign_scoring_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::data eawr::vfs
    TESTS galactic_campaign_scoring_contracts
)

eawr_add_game_data_test(NAME galactic_campaign_scoring_stock COMMAND galactic_campaign_scoring_tests --installed)
set_tests_properties(galactic_campaign_scoring_stock PROPERTIES LABELS game-data)

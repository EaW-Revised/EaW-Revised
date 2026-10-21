eawr_add_test_case(
    NAME galactic_live_campaign_setup_tests
    SOURCES live_campaign_setup_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/planet_hud.cpp"
    LIBS eawr::campaign eawr::checkpoint eawr::platform eawr::ui_model
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    TESTS galactic_live_campaign_setup_contracts
)

eawr_add_game_data_test(NAME galactic_structure_slots_stock COMMAND galactic_live_campaign_setup_tests --structure-slots)
set_tests_properties(galactic_structure_slots_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")

eawr_add_game_data_test(NAME galactic_production_domain_stock COMMAND galactic_live_campaign_setup_tests --production-domains)
set_tests_properties(galactic_production_domain_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_starting_tech_stock COMMAND galactic_live_campaign_setup_tests --starting-tech)
set_tests_properties(galactic_starting_tech_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME galactic_accepted_starting_tech_stock COMMAND galactic_live_campaign_setup_tests --accepted-starting-tech)
set_tests_properties(galactic_accepted_starting_tech_stock PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
eawr_add_game_data_test(NAME galactic_origin_starbase_stock COMMAND galactic_live_campaign_setup_tests --origin-starbases)
set_tests_properties(galactic_origin_starbase_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
eawr_add_game_data_test(NAME galactic_unscouted_intel_stock COMMAND galactic_live_campaign_setup_tests --unscouted-intel)
set_tests_properties(galactic_unscouted_intel_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")

eawr_add_game_data_test(NAME galactic_mp_heroes_tarkin_stock COMMAND galactic_live_campaign_setup_tests --mp-heroes-tarkin)
eawr_add_game_data_test(NAME galactic_mp_heroes_protection_live_stock COMMAND galactic_live_campaign_setup_tests --mp-hero-protection-live)
eawr_add_game_data_test(NAME galactic_authored_protection_source_live_stock COMMAND galactic_live_campaign_setup_tests --authored-protection-source)
set_tests_properties(galactic_mp_heroes_tarkin_stock galactic_mp_heroes_protection_live_stock
    galactic_authored_protection_source_live_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")

eawr_add_game_data_test(NAME galactic_solo_protection_sources_live_stock COMMAND galactic_live_campaign_setup_tests --solo-protection-sources)
set_tests_properties(galactic_solo_protection_sources_live_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")

eawr_add_game_data_test(NAME galactic_mp_all_stock_protection_live COMMAND galactic_live_campaign_setup_tests --all-mp-protection-sources)
set_tests_properties(galactic_mp_all_stock_protection_live PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")

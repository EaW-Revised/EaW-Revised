eawr_add_test_case(
    NAME galactic_corruption_mod_proof_tests
    SOURCES corruption_mod_proof_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
)

# #7753 (corruption PR 3): the corruption system on installed mod data, headless, through the live session and the viewer's
# own drop preview and order path. Each mode names its mod; a host without the mod skips (77).
#   raw-chain  Republic at War: charge, intimidation use counters, named hero to the respawn timer, 5 s transition, bonus,
#              racketeering upgrade.   raw-own  a hero of the corrupting faction on its own planet removes nothing.
#   raw-info   the information menu, slotted and empty.   remake  corruption stays dormant.   bfc  information menu, bribe.
eawr_add_game_data_test(NAME galactic_corruption_mod_proof_raw_chain COMMAND galactic_corruption_mod_proof_tests raw-chain)
set_tests_properties(galactic_corruption_mod_proof_raw_chain PROPERTIES LABELS "game-data;mod-data" TIMEOUT 3600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_corruption_mod_proof_raw_own COMMAND galactic_corruption_mod_proof_tests raw-own)
set_tests_properties(galactic_corruption_mod_proof_raw_own PROPERTIES LABELS "game-data;mod-data" TIMEOUT 3600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_corruption_mod_proof_raw_info COMMAND galactic_corruption_mod_proof_tests raw-info)
set_tests_properties(galactic_corruption_mod_proof_raw_info PROPERTIES LABELS "game-data;mod-data" TIMEOUT 3600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_corruption_mod_proof_remake COMMAND galactic_corruption_mod_proof_tests remake)
set_tests_properties(galactic_corruption_mod_proof_remake PROPERTIES LABELS "game-data;mod-data" TIMEOUT 3600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
eawr_add_game_data_test(NAME galactic_corruption_mod_proof_bfc COMMAND galactic_corruption_mod_proof_tests bfc)
set_tests_properties(galactic_corruption_mod_proof_bfc PROPERTIES LABELS "game-data;mod-data" TIMEOUT 3600 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

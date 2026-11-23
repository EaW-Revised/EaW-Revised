# #8161: the standing authored-versus-offered build-list check on installed content. Each mode names its stack; a host without
# the mod skips (77). Any authored build-tab type that a campaign's faction should be offered but is dropped or refused without
# a retail reason fails the test.
eawr_add_test_case(
    NAME galactic_build_list_check_tests
    SOURCES build_list_check_tests.cpp
    LIBS eawr::campaign eawr::platform
)
foreach(stack stock raw remake bfc)
    eawr_add_game_data_test(NAME galactic_build_list_check_${stack} COMMAND galactic_build_list_check_tests ${stack})
    set_tests_properties(galactic_build_list_check_${stack} PROPERTIES LABELS "game-data;mod-data" TIMEOUT 3600 SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
endforeach()
# The units-gap check at data level (no campaign): old rule against new rule for stock and each named stack (BFC, Remake and its two submods, RaW, Humanity's Retaliation, the Remake booster).
eawr_add_game_data_test(NAME galactic_build_list_gap_data COMMAND galactic_build_list_check_tests
    gap:2712013423,1770851727,2794270450+1770851727,3229239424+2794270450+1770851727,1129810972,3460641869,3689306867)
set_tests_properties(galactic_build_list_gap_data PROPERTIES LABELS "game-data;mod-data;batch-only" TIMEOUT 3600 SKIP_RETURN_CODE 77
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
